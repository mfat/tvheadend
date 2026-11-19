/*
 *  Functions for storing program settings
 *  Copyright (C) 2008 Andreas Öman
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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include "htsmsg.h"
#include "htsmsg_binary.h"
#include "htsmsg_binary2.h"
#include "htsmsg_json.h"
#include "settings.h"
#include "settings_sqlite.h"
#include "tvheadend.h"
#include "filebundle.h"

#include "../vendor/xdg-user-dirs/xdg-user-dir-lookup.c"

static char *settingspath = NULL;

/**
 *
 */
const char *
hts_settings_get_root(void)
{
  return settingspath;
}

/**
 *
 */
void
hts_settings_init(const char *confpath)
{
  if (confpath)
    settingspath = realpath(confpath, NULL);
  /* A database that is present but cannot be opened must not fall back to
   * leftover files: those files are no longer the copy being updated. */
  if (hts_settings_sqlite_init(settingspath))
    exit(1);
}

/**
 *
 */
void
hts_settings_done(void)
{
  hts_settings_sqlite_done();
  free(settingspath);
  settingspath = NULL;
}

/**
 *
 */
int
hts_settings_makedirs ( const char *inpath )
{
  size_t x = strlen(inpath) - 1;
  char *path = alloca(x + 2);

  if (path == NULL) return -1;
  strcpy(path, inpath);

  while (x) {
    if (path[x] == '/') {
      path[x] = 0;
      break;
    }
    x--;
  }
  if (hts_settings_sqlite_active() && hts_settings_sqlite_makedirs(inpath))
    return -1;
  return makedirs(LS_SETTINGS, path, 0700, 1, -1, -1);
}

/**
 *
 */
static void
_hts_settings_buildpath
  (char *dst, size_t dstsize, const char *fmt, va_list ap, const char *prefix)
{
  char tmp[PATH_MAX];
  char *n = dst;

  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  if (*tmp != '/' && prefix)
    snprintf(dst, dstsize, "%s/%s", prefix, tmp);
  else
    strlcpy(dst, tmp, dstsize);

  while(*n) {
    if(*n == ':' || *n == '?' || *n == '*' || *n > 127 || *n < 32)
      *n = '_';
    n++;
  }
}

int
hts_settings_buildpath
  (char *dst, size_t dstsize, const char *fmt, ...)
{
  va_list va;
  if (!settingspath)
    return 1;
  va_start(va, fmt);
  _hts_settings_buildpath(dst, dstsize, fmt, va, settingspath);
  va_end(va);
  return 0;
}

/**
 * Write the same bytes a settings file contains.
 * Returns 0 on success.
 */
static int
hts_settings_pack(const char *path)
{
#if ENABLE_ZLIB
  return strstr(path, "/muxes/") != NULL && /* ugly, redesign API */
         strstr(path, "/networks/") != NULL &&
         strstr(path, "/input/") != NULL;
#else
  return 0;
#endif
}

static int
hts_settings_write_record(int fd, htsmsg_t *record, const char *path, const char *what)
{
  htsbuf_queue_t hq;
  htsbuf_data_t *hd;

  if (!hts_settings_pack(path)) {
    htsbuf_queue_init(&hq, 0);
    htsmsg_json_serialize(record, &hq, 1);
    TAILQ_FOREACH(hd, &hq.hq_q, hd_link)
      if (tvh_write(fd, hd->hd_data + hd->hd_data_off, hd->hd_data_len)) {
        tvhalert(LS_SETTINGS, "Failed to write \"%s\" - %s",
                 what, strerror(errno));
        htsbuf_queue_flush(&hq);
        return -1;
      }
    htsbuf_queue_flush(&hq);
    return 0;
  }
#if ENABLE_ZLIB
  {
    void *msgdata = NULL;
    size_t msglen;
    int r = htsmsg_binary2_serialize0(record, &msgdata, &msglen, 2*1024*1024);
    if (!r && msglen >= 4)
      r = tvh_gzip_deflate_fd_header(fd, msgdata, msglen, NULL, 3, "01");
    else {
      tvhalert(LS_SETTINGS, "Unable to pack the configuration data \"%s\"", path);
      r = -1;
    }
    free(msgdata);
    return r ? -1 : 0;
  }
#else
  return -1;
#endif
}

/* Serialize directly into memory for SQLite. This avoids Linux-only memfd
 * APIs and filesystem work, while retaining the existing JSON/GZIP01 format. */
static int
hts_settings_store_record(htsmsg_t *record, const char *path)
{
  char *json;
  int r;

  if (!hts_settings_pack(path)) {
    json = htsmsg_json_serialize_to_str(record, 1);
    if (!json)
      return -1;
    r = hts_settings_sqlite_put(path, json, strlen(json));
    free(json);
    return r;
  }
#if ENABLE_ZLIB
  {
    void *data = NULL;
    uint8_t *gzip, *packed;
    size_t len, size = 0;
    r = htsmsg_binary2_serialize0(record, &data, &len, 2*1024*1024);
    if (r || len < 4) {
      free(data);
      return -1;
    }
    gzip = tvh_gzip_deflate(data, len, &size);
    free(data);
    if (!gzip)
      return -1;
    packed = malloc(size + 12);
    if (!packed) {
      free(gzip);
      return -1;
    }
    memcpy(packed, "\xff\xffGZIP01", 8);
    packed[8] = (len >> 24) & 0xff;
    packed[9] = (len >> 16) & 0xff;
    packed[10] = (len >> 8) & 0xff;
    packed[11] = len & 0xff;
    memcpy(packed + 12, gzip, size);
    free(gzip);
    r = hts_settings_sqlite_put(path, packed, size + 12);
    free(packed);
    return r;
  }
#else
  return -1;
#endif
}

/**
 *
 */
void
hts_settings_save(htsmsg_t *record, const char *pathfmt, ...)
{
  char path[PATH_MAX];
  char tmppath[PATH_MAX + 4];
  int fd;
  va_list ap;
  int ok, r;

  if(settingspath == NULL)
    return;

  /* Clean the path */
  va_start(ap, pathfmt);
  _hts_settings_buildpath(path, sizeof(path), pathfmt, ap, settingspath);
  va_end(ap);

  tvhdebug(LS_SETTINGS, "saving to %s", path);

  /* The database, once selected, is the only copy. Writing the file as
   * well would make the next startup read whichever copy it opened first. */
  if (hts_settings_sqlite_active() && hts_settings_sqlite_owns(path)) {
    if (hts_settings_store_record(record, path))
      tvhalert(LS_SETTINGS, "Unable to store \"%s\"", path);
    return;
  }

  /* Only the file backend needs physical parent directories. */
  if (hts_settings_makedirs(path)) return;

  /* Create tmp file */
  snprintf(tmppath, sizeof(tmppath), "%s.tmp", path);
  if((fd = tvh_open(tmppath, O_CREAT | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR)) < 0) {
    tvhalert(LS_SETTINGS, "Unable to create \"%s\" - %s",
	     tmppath, strerror(errno));
    return;
  }

  ok = hts_settings_write_record(fd, record, path, tmppath) == 0;
  close(fd);

  /* Move */
  if(ok) {
    r = rename(tmppath, path);
    if (r && errno == EISDIR) {
      rmtree(path);
      r = rename(tmppath, path);
    }
    if (r)
      tvhalert(LS_SETTINGS, "Unable to rename file \"%s\" to \"%s\" - %s",
	       tmppath, path, strerror(errno));
  
  /* Delete tmp */
  } else
    unlink(tmppath);
}

/**
 *
 */
static htsmsg_t *
hts_settings_load_one(const char *filename, const fb_dir *dir, const char *name)
{
  ssize_t n, size;
  char *mem;
  fb_file *fp;
  htsmsg_t *r = NULL;

  /* Open */
  fp = dir ? fb_open2(dir, name, 1, 0) : fb_open(filename, 1, 0);
  if (!fp) return NULL;
  size = fb_size(fp);

  /* Load data */
  mem    = malloc(size+1);
  n      = fb_read(fp, mem, size);
  if (n >= 0) mem[n] = 0;

  /* Decode */
  if(n == size) {
    if (size > 12 && memcmp(mem, "\xff\xffGZIP0", 7) == 0 &&
        (mem[7] == '0' || mem[7] == '1')) {
#if ENABLE_ZLIB
      uint32_t orig = (mem[8] << 24) | (mem[9] << 16) | (mem[10] << 8) | mem[11];
      if (orig > 10*1024*1024U) {
        tvhalert(LS_SETTINGS, "too big gzip for %s", filename);
        r = NULL;
      } else if (orig > 0) {
        uint8_t *unpacked = tvh_gzip_inflate((uint8_t *)mem + 12, size - 12, orig);
        if (unpacked) {
          if (mem[7] == '1') {
            r = htsmsg_binary2_deserialize0(unpacked, orig, NULL);
          } else {
            r = htsmsg_binary_deserialize0(unpacked, orig, NULL);
          }
          free(unpacked);
        }
      }
#endif
    } else {
      r = htsmsg_json_deserialize(mem);
    }
  }

  /* Close */
  fb_close(fp);
  free(mem);

  return r;
}

/*
 * Loading a directory of many settings files (one per IPTV mux or channel)
 * spends most of its time reading, unpacking and parsing each file. That
 * work is independent per file, so large directories load their files on a
 * few threads; the results are still added in directory order.
 */
#define SETTINGS_PARALLEL_MIN     256
#define SETTINGS_PARALLEL_THREADS 8

typedef struct settings_parallel {
  const fb_dir  *dir;
  const char    *fullpath;
  fb_dirent    **namelist;
  int            depth;
  int            count;
  volatile int   next;
  htsmsg_t     **result;
} settings_parallel_t;

static int
hts_settings_skip_name(const char *name)
{
  return name[0] == '.' || name[0] == '\0' || name[strlen(name)-1] == '~';
}

static void *
hts_settings_parallel_worker(void *aux)
{
  settings_parallel_t *sp = aux;
  char child[PATH_MAX];
  fb_dirent *d;
  int i;

  while ((i = atomic_add(&sp->next, 1)) < sp->count) {
    d = sp->namelist[i];
    if (hts_settings_skip_name(d->name))
      continue;
    if (d->type == FB_DIR && sp->depth > 0)
      continue;                  /* subdirectories load in directory order */
    snprintf(child, sizeof(child), "%s/%s", sp->fullpath, d->name);
    sp->result[i] = hts_settings_load_one(child, sp->dir, d->name);
  }
  return NULL;
}

/* Load the plain files of a directory into result[], indexed like namelist. */
static void
hts_settings_load_parallel
  (const char *fullpath, const fb_dir *dir, fb_dirent **namelist,
   int n, int depth, htsmsg_t **result)
{
  settings_parallel_t sp = {
    .dir = dir, .fullpath = fullpath, .namelist = namelist,
    .depth = depth, .count = n, .next = 0, .result = result,
  };
  pthread_t threads[SETTINGS_PARALLEL_THREADS - 1];
  long cpus = sysconf(_SC_NPROCESSORS_ONLN);
  int i, started = 0;
  int wanted = (int)MIN(MAX(cpus, 1), SETTINGS_PARALLEL_THREADS) - 1;

  for (i = 0; i < wanted; i++) {
    if (tvh_thread_create(&threads[started], NULL,
                          hts_settings_parallel_worker, &sp, "settings"))
      break;
    started++;
  }
  /* This thread works too, so loading completes even without helpers. */
  hts_settings_parallel_worker(&sp);
  for (i = 0; i < started; i++)
    pthread_join(threads[i], NULL);
}

/*
 * Freeing the settings tree of a large directory (every IPTV mux with its
 * services, every channel) is a walk of hundreds of thousands of small
 * allocations. Once the objects are created from it, nothing refers to it,
 * so it is freed on a short-lived thread instead of holding up startup.
 */
static void *
hts_settings_free_thread(void *aux)
{
  htsmsg_destroy(aux);
  return NULL;
}

void
hts_settings_free_later(htsmsg_t *settings)
{
  pthread_t tid;

  if (settings == NULL)
    return;
  if (tvh_thread_create(&tid, NULL, hts_settings_free_thread, settings,
                        "settings-free")) {
    htsmsg_destroy(settings);
    return;
  }
  pthread_detach(tid);
}

/**
 *
 */
static htsmsg_t *
hts_settings_load_path(const char *fullpath, int depth)
{
  char child[PATH_MAX];
  const char *name;
  struct filebundle_stat st;
  fb_dirent **namelist, *d;
  fb_dir *dir;
  htsmsg_t *r, *c, **loaded;
  int n, i;

  if (hts_settings_sqlite_active() && hts_settings_sqlite_owns(fullpath))
    return hts_settings_sqlite_load(fullpath, depth);

  /* Invalid */
  if (fb_stat(fullpath, &st)) return NULL;

  /* Directory */
  if (st.is_dir) {

    /* Reuse the directory for all children instead of opening it again
     * for every settings file. Keep filebundle fallback semantics. */
    if (!(dir = fb_opendir(fullpath)))
      return NULL;
    if ((n = fb_scandir(fullpath, &namelist)) < 0) {
      fb_closedir(dir);
      return NULL;
    }

    /* Read files */
    loaded = NULL;
    if (fb_dir_is_direct(dir) && n >= SETTINGS_PARALLEL_MIN &&
        (loaded = calloc(n, sizeof(*loaded))) != NULL)
      hts_settings_load_parallel(fullpath, dir, namelist, n, depth, loaded);
    r = htsmsg_create_map();
    for(i = 0; i < n; i++) {
      d = namelist[i];
      name = d->name;
      if(!hts_settings_skip_name(name)) {

        snprintf(child, sizeof(child), "%s/%s", fullpath, d->name);
        if(d->type == FB_DIR && depth > 0) {
          c = hts_settings_load_path(child, depth - 1);
        } else if (loaded) {
          c = loaded[i];
        } else {
          c = hts_settings_load_one(child, dir, d->name);
        }
        if(c != NULL)
          htsmsg_add_msg(r, d->name, c);

      }
      free(d);
    }
    free(loaded);
    free(namelist);
    fb_closedir(dir);

  /* File */
  } else {
    r = hts_settings_load_one(fullpath, NULL, NULL);
  }

  return r;
}

/**
 *
 */
static htsmsg_t *
hts_settings_vload(const char *pathfmt, va_list ap, int depth)
{
  htsmsg_t *ret = NULL;
  char fullpath[PATH_MAX];
  va_list ap2;
  va_copy(ap2, ap);

  /* Try normal path */
  _hts_settings_buildpath(fullpath, sizeof(fullpath), 
                          pathfmt, ap, settingspath);
  ret = hts_settings_load_path(fullpath, depth);

  /* Try bundle path */
  if (!ret && *pathfmt != '/') {
    _hts_settings_buildpath(fullpath, sizeof(fullpath),
                            pathfmt, ap2, "data/conf");
    ret = hts_settings_load_path(fullpath, depth);
  }

  va_end(ap2);

  return ret;
}


/**
 *
 */
htsmsg_t *
hts_settings_load(const char *pathfmt, ...)
{
  va_list ap;
  va_start(ap, pathfmt);
  htsmsg_t *r = hts_settings_vload(pathfmt, ap, 0);
  va_end(ap);
  return r;
}


/**
 *
 */
htsmsg_t *
hts_settings_load_r(int depth, const char *pathfmt, ...)
{
  va_list ap;
  va_start(ap, pathfmt);
  htsmsg_t *r = hts_settings_vload(pathfmt, ap, depth);
  va_end(ap);
  return r;
}

/**
 *
 */
void
hts_settings_remove(const char *pathfmt, ...)
{
  char fullpath[PATH_MAX];
  va_list ap;
  struct stat st;

  va_start(ap, pathfmt);
  _hts_settings_buildpath(fullpath, sizeof(fullpath),
                          pathfmt, ap, settingspath);
  va_end(ap);
  if (hts_settings_sqlite_active() && hts_settings_sqlite_owns(fullpath))
    hts_settings_sqlite_remove(fullpath);
  if (stat(fullpath, &st) == 0) {
    if (S_ISDIR(st.st_mode))
      rmtree(fullpath);
    else {
      unlink(fullpath);
      while (rmdir(dirname(fullpath)) == 0);
    }
  }
}

/**
 *
 */
int
hts_settings_open_file(int flags, const char *pathfmt, ...)
{
  char path[PATH_MAX];
  int _flags;
  va_list ap;

  /* Build path */
  va_start(ap, pathfmt);
  _hts_settings_buildpath(path, sizeof(path), pathfmt, ap, settingspath);
  va_end(ap);

  /* Create directories */
  if (flags & HTS_SETTINGS_OPEN_WRITE)
    if (hts_settings_makedirs(path)) return -1;

  /* Open file */
  _flags = (flags & HTS_SETTINGS_OPEN_WRITE) ? O_CREAT | O_TRUNC | O_WRONLY : O_RDONLY;

  if (flags & HTS_SETTINGS_OPEN_DIRECT)
    return open(path, _flags, S_IRUSR | S_IWUSR);

  return tvh_open(path, _flags, S_IRUSR | S_IWUSR);
}

/*
 * Check if a path exists
 */
int
hts_settings_exists ( const char *pathfmt, ... )
{
  va_list ap;
  char path[PATH_MAX];
  struct stat st;

  /* Build path */
  va_start(ap, pathfmt);
  _hts_settings_buildpath(path, sizeof(path), pathfmt, ap, settingspath);
  va_end(ap);

  if (hts_settings_sqlite_active() && hts_settings_sqlite_owns(path) &&
      hts_settings_sqlite_exists(path))
    return 1;
  return (stat(path, &st) == 0);
}

/*
 * XDG user directory support
 */
char *
hts_settings_get_xdg_dir_lookup (const char *name)
{
  return xdg_user_dir_lookup (name);
}

char *
hts_settings_get_xdg_dir_with_fallback (const char *name, const char *fallback)
{
  return xdg_user_dir_lookup_with_fallback (name, fallback);
}
