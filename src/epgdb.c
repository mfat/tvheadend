/*
 *  Electronic Program Guide - Database File functions
 *  Copyright (C) 2007 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "tvheadend.h"
#include "sbuf.h"
#include "htsmsg_binary.h"
#include "htsmsg_binary2.h"
#include "settings.h"
#include "channels.h"
#include "epg.h"
#include "epggrab.h"
#include "config.h"
#include "memoryinfo.h"

#define EPG_DB_VERSION 3
#define EPG_DB_ALLOC_STEP (1024*1024)

extern epg_object_tree_t epg_episodes;

/* **************************************************************************
 * Load
 * *************************************************************************/

/*
 * Process v3 data
 */
static void
_epgdb_v3_process( char **sect, htsmsg_t *m, epggrab_stats_t *stats )
{
  int save = 0;
  const char *s;

  /* New section */
  if ( (s = htsmsg_get_str(m, "__section__")) ) {
    if (*sect) free(*sect);
    *sect = strdup(s);
  
  /* Broadcasts */
  } else if ( !strcmp(*sect, "broadcasts") ) {
    if (epg_broadcast_deserialize(m, 1, &save)) stats->broadcasts.total++;

  /* Global config */
  } else if ( !strcmp(*sect, "config") ) {
    if (epg_config_deserialize(m)) stats->config.total++;

  /* Unknown */
  } else {
    tvhdebug(LS_EPGDB, "malformed database section [%s]", *sect);
    //htsmsg_print(m);
  }
}

/*
 * Memoryinfo
 */

static void epg_memoryinfo_broadcasts_update(memoryinfo_t *my)
{
  channel_t *ch;
  epg_broadcast_t *ebc;
  int64_t size = 0, count = 0;

  CHANNEL_FOREACH(ch) {
    if (ch->ch_epg_parent) continue;
    RB_FOREACH(ebc, &ch->ch_epg_schedule, sched_link) {
      size += sizeof(*ebc);
      size += tvh_strlen(ebc->image);
      size += tvh_strlen(ebc->epnum.text);
      size += lang_str_size(ebc->title);
      size += lang_str_size(ebc->subtitle);
      size += lang_str_size(ebc->summary);
      size += lang_str_size(ebc->description);
      count++;
    }
  }
  memoryinfo_update(my, size, count);
}

static memoryinfo_t epg_memoryinfo_broadcasts = {
  .my_name = "EPG Broadcasts",
  .my_update = epg_memoryinfo_broadcasts_update
};

/*
 * Reading and unpacking the database file needs nothing else from the
 * server, so it runs on a helper thread while networks, muxes and channels
 * load. epg_init waits for it and restores the broadcasts at the same point
 * of startup as before.
 */
typedef struct epgdb_file {
  int       found;   /* a database file was read */
  int       ver;
  uint8_t  *data;    /* the unpacked database */
  size_t    size;
} epgdb_file_t;

static pthread_t    epgdb_read_tid;
static int          epgdb_read_started;
static epgdb_file_t epgdb_read_result;

static void
epgdb_read_file ( epgdb_file_t *f )
{
  int fd = -1, ver = EPG_DB_VERSION;
  struct stat st;
  uint8_t *mem = NULL;
  size_t got = 0;
  ssize_t r;

  memset(f, 0, sizeof(*f));

  /* Find the right file (and version) */
  while (fd < 0 && ver > 0) {
    fd = hts_settings_open_file(0, "epgdb.v%d", ver);
    if (fd > 0) break;
    ver--;
  }
  if ( fd < 0 )
    fd = hts_settings_open_file(0, "epgdb");
  if ( fd < 0 ) {
    tvhdebug(LS_EPGDB, "database does not exist");
    return;
  }

  if ( fstat(fd, &st) != 0 ) {
    tvherror(LS_EPGDB, "failed to detect database size");
    goto end;
  }
  if ( !st.st_size ) {
    tvhdebug(LS_EPGDB, "database is empty");
    goto end;
  }
  mem = malloc(st.st_size);
  if ( !mem ) {
    tvherror(LS_EPGDB, "failed to allocate %jd bytes for the database",
             (intmax_t)st.st_size);
    goto end;
  }
  while ( got < (size_t)st.st_size ) {
    r = read(fd, mem + got, st.st_size - got);
    if ( r < 0 && (errno == EINTR || errno == EAGAIN) ) continue;
    if ( r <= 0 ) break;
    got += r;
  }
  if ( got != (size_t)st.st_size ) {
    tvherror(LS_EPGDB, "failed to read the database");
    goto end;
  }

#if ENABLE_ZLIB
  if (got > 12 && memcmp(mem, "\xff\xffGZIP01", 8) == 0 &&
      (mem[7] == '0' || mem[7] == '1')) {
    uint32_t orig = (mem[8] << 24) | (mem[9] << 16) | (mem[10] << 8) | mem[11];
    int64_t inflate_at = getmonoclock();
    uint8_t *unpacked;
    tvhinfo(LS_EPGDB, "gzip format detected, inflating (ratio %.1f%% deflated size %zd)",
            ((got * 100.0) / orig), got);
    unpacked = tvh_gzip_inflate(mem + 12, got - 12, orig);
    free(mem);
    mem = unpacked;
    got = mem ? orig : 0;
    tvhtrace(LS_MAIN, "startup epg inflate: %"PRId64" ms",
            mono2ms(getmonoclock() - inflate_at));
  }
#endif

  /* A file that fails to unpack still counts as found: it restores
   * nothing, like an empty one would, and the defaults apply. */
  f->found = 1;
  f->ver  = ver;
  f->data = mem;
  f->size = got;
  mem = NULL;
end:
  free(mem);
  close(fd);
}

static void *
epgdb_read_thread ( void *aux )
{
  int64_t at = getmonoclock();
  epgdb_read_file(&epgdb_read_result);
  tvhtrace(LS_MAIN, "startup epg read: %"PRId64" ms", mono2ms(getmonoclock() - at));
  return NULL;
}

void epg_read_start ( void )
{
  if (!tvh_thread_create(&epgdb_read_tid, NULL, epgdb_read_thread, NULL, "epgdb"))
    epgdb_read_started = 1;
}

/*
 * Load data
 */
void epg_init ( void )
{
  int r;
  size_t remain;
  uint8_t *rp;
  epggrab_stats_t stats;
  epgdb_file_t file;
  char *sect = NULL;
  int profile = tvhtrace_enabled();
  int64_t decode_us = 0, restore_us = 0, destroy_us = 0, profile_at;

  memoryinfo_register(&epg_memoryinfo_broadcasts);

  /* The file was read while the configuration loaded, or is read now */
  if (epgdb_read_started) {
    int64_t wait_at = getmonoclock();
    pthread_join(epgdb_read_tid, NULL);
    epgdb_read_started = 0;
    file = epgdb_read_result;
    memset(&epgdb_read_result, 0, sizeof(epgdb_read_result));
    tvhtrace(LS_MAIN, "startup epg read wait: %"PRId64" ms",
             mono2ms(getmonoclock() - wait_at));
  } else {
    epgdb_read_file(&file);
  }
  if (!file.found)
    return;
  rp = file.data;
  remain = file.size;

  tvhinfo(LS_EPGDB, "parsing %zd bytes", remain);

  /* Process */
  memset(&stats, 0, sizeof(stats));
  int64_t parse_at = profile ? getmonoclock() : 0;
  while ( remain > 4 ) {

    /* Get message length */
    size_t msglen = remain;
    htsmsg_t *m;
    profile_at = profile ? getmonoclock() : 0;
    r = htsmsg_binary2_deserialize(&m, rp, &msglen, NULL);
    if (profile) decode_us += getmonoclock() - profile_at;

    /* Safety check */
    if (r) {
      tvherror(LS_EPGDB, "corruption detected, some/all data lost");
      break;
    }

    /* Next */
    rp     += msglen;
    remain -= msglen;

    /* Skip */
    if (!m) continue;

    /* Process */
    profile_at = profile ? getmonoclock() : 0;
    switch (file.ver) {
      case 3:
        _epgdb_v3_process(&sect, m, &stats);
        break;
      default:
        break;
    }

    if (profile) restore_us += getmonoclock() - profile_at;
    /* Cleanup */
    profile_at = profile ? getmonoclock() : 0;
    htsmsg_destroy(m);
    if (profile) destroy_us += getmonoclock() - profile_at;
  }
  tvhtrace(LS_MAIN, "startup epg parse: %"PRId64" ms",
          mono2ms(getmonoclock() - parse_at));

  tvhtrace(LS_MAIN, "startup epg decode/restore/destroy: %"PRId64"/%"PRId64"/%"PRId64" ms",
           mono2ms(decode_us), mono2ms(restore_us), mono2ms(destroy_us));
  free(sect);

  if (!stats.config.total) {
    htsmsg_t *m = htsmsg_create_map();
    /* it's not correct, but at least something */
    htsmsg_add_u32(m, "last_id", 64 * 1024 * 1024);
    if (!epg_config_deserialize(m))
      assert(0);
    htsmsg_destroy(m);
  }

  /* Stats */
  tvhinfo(LS_EPGDB, "loaded v%d", file.ver);
  tvhinfo(LS_EPGDB, "  config     %d", stats.config.total);
  tvhinfo(LS_EPGDB, "  broadcasts %d", stats.broadcasts.total);

  free(file.data);
}

void epg_done ( void )
{
  channel_t *ch;

  tvh_mutex_lock(&global_lock);
  CHANNEL_FOREACH(ch)
    epg_channel_unlink(ch);
  epg_skel_done();
  memoryinfo_unregister(&epg_memoryinfo_broadcasts);
  tvh_mutex_unlock(&global_lock);
}

/* **************************************************************************
 * Save
 * *************************************************************************/

static int _epg_write ( sbuf_t *sb, htsmsg_t *m )
{
  int ret = 1;
  size_t msglen;
  void *msgdata;
  if (m) {
    int r;
    r = htsmsg_binary2_serialize(m, &msgdata, &msglen, 0x10000);
    htsmsg_destroy(m);
    if (!r) {
      ret = 0;
      /* allocation helper - we fight with megabytes */
      if (sb->sb_size - sb->sb_ptr < 32 * 1024)
        sbuf_realloc(sb, (sb->sb_size - (sb->sb_size % EPG_DB_ALLOC_STEP)) + EPG_DB_ALLOC_STEP);
      sbuf_append(sb, msgdata, msglen);
      free(msgdata);
    }
  } else {
    ret = 0;
  }
  return ret;
}

static int _epg_write_sect ( sbuf_t *sb, const char *sect )
{
  htsmsg_t *m = htsmsg_create_map();
  htsmsg_add_str(m, "__section__", sect);
  return _epg_write(sb, m);
}

static void epg_save_tsk_callback ( void *p, int dearmed )
{
  char tmppath[PATH_MAX+4];
  char path[PATH_MAX];
  sbuf_t *sb = p;
  size_t size = sb->sb_ptr, orig;
  int fd, r;

  tvhinfo(LS_EPGDB, "save start");
  if(hts_settings_buildpath(tmppath, sizeof(tmppath), "epgdb.v%d", EPG_DB_VERSION)) {
    tvhinfo(LS_EPGDB, "No config dir, not saving EPG");
    goto done;
  }
  
  if (!realpath(tmppath, path))
    strlcpy(path, tmppath, sizeof(path));
  snprintf(tmppath, sizeof(tmppath), "%s.tmp", path);
  if (hts_settings_makedirs(tmppath)) {
    tvherror(LS_EPGDB, "Failed to create tmp directories for %s", tmppath);
    goto done;
  }

  fd = tvh_open(tmppath, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    tvherror(LS_EPGDB, "unable to open epgdb file");
    goto done;
  }
  
#if ENABLE_ZLIB
  if (config.epg_compress) {
    r = tvh_gzip_deflate_fd_header(fd, sb->sb_data, size, &orig, 3, "01") < 0;
  } else
#endif
    r = tvh_write(fd, sb->sb_data, orig = size);
  
  close(fd);
  if (r) {
    tvherror(LS_EPGDB, "write error (size %zd)", orig);
    if (remove(tmppath))
      tvherror(LS_EPGDB, "unable to remove file %s", tmppath);
  } else {
    tvhinfo(LS_EPGDB, "stored (size %zd)", orig);
    if (rename(tmppath, path))
      tvherror(LS_EPGDB, "unable to rename file %s to %s", tmppath, path);
  }

done:
  sbuf_free(sb);
  free(sb);
}

void epg_save_callback ( void *p )
{
  epg_save();
}

void epg_save ( void )
{
  sbuf_t *sb = malloc(sizeof(*sb));
  epg_broadcast_t *ebc;
  channel_t *ch;
  epggrab_stats_t stats;
  extern gtimer_t epggrab_save_timer;

  if (!sb)
    return;

  tvhinfo(LS_EPGDB, "snapshot start");

  sbuf_init_fixed(sb, EPG_DB_ALLOC_STEP);

  if (epggrab_conf.epgdb_periodicsave)
    gtimer_arm_rel(&epggrab_save_timer, epg_save_callback, NULL,
                   epggrab_conf.epgdb_periodicsave * 3600);

  memset(&stats, 0, sizeof(stats));
  if ( _epg_write_sect(sb, "config") ) goto error;
  if (_epg_write(sb, epg_config_serialize())) goto error;
  if ( _epg_write_sect(sb, "broadcasts") ) goto error;
  CHANNEL_FOREACH(ch) {
    if (ch->ch_epg_parent) continue;
    RB_FOREACH(ebc, &ch->ch_epg_schedule, sched_link) {
      if (_epg_write(sb, epg_broadcast_serialize(ebc))) goto error;
      stats.broadcasts.total++;
    }
  }

  tasklet_arm_alloc(epg_save_tsk_callback, sb);

  /* Stats */
  tvhinfo(LS_EPGDB, "queued to save (size %d)", sb->sb_ptr);
  tvhinfo(LS_EPGDB, "  broadcasts %d", stats.broadcasts.total);

  return;

error:
  tvherror(LS_EPGDB, "failed to store epg to disk");
  hts_settings_remove("epgdb.v%d", EPG_DB_VERSION);
  sbuf_free(sb);
  free(sb);
}
