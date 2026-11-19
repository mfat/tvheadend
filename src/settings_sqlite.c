/*
 * Settings stored as one database row per file.
 *
 * The bytes in each row are the bytes the file backend would have written,
 * including the gzip packing used for mux files. Directory order is the
 * order rows were first inserted. The programme database and image-cache
 * pictures stay on the filesystem; they use hts_settings_open_file.
 *
 * Copyright (C) 2026 TVHplayer contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <errno.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tvheadend.h"
#include "atomic.h"
#include "htsmsg_binary.h"
#include "htsmsg_binary2.h"
#include "htsmsg_json.h"
#include "settings_sqlite.h"

#define SETTINGS_PARALLEL_MIN     256
#define SETTINGS_PARALLEL_THREADS 8

static sqlite3 *settings_db;
static char *settings_root;
static int settings_db_active;
static tvh_mutex_t settings_db_lock = TVH_THREAD_MUTEX_INITIALIZER;

static int settings_sqlite_exec(const char *sql);
static int settings_sqlite_begin(void);
static int settings_sqlite_commit(void);
static void settings_sqlite_rollback(void);
static const char *settings_sqlite_rel(const char *fullpath);
static int settings_sqlite_lookup(const char *rel, int *kind);
static int settings_sqlite_ensure_dir(const char *rel);
static htsmsg_t *settings_sqlite_decode(const char *fullpath, char *mem, size_t size);
static htsmsg_t *settings_sqlite_load_rel(const char *rel, int depth);

/* a and b are joined with a slash when both are non-empty. */
static int
settings_sqlite_join(char *dst, size_t dstsize, const char *a, const char *b)
{
  size_t na = strlen(a), nb = strlen(b);
  int slash = a[0] && b[0];

  if (na + (size_t)slash + nb + 1 > dstsize)
    return -1;
  memcpy(dst, a, na);
  if (slash)
    dst[na] = '/';
  memcpy(dst + na + slash, b, nb + 1);
  return 0;
}

int
hts_settings_sqlite_init(const char *root)
{
  const char *env = getenv("TVH_SETTINGS_SQLITE");
  char path[PATH_MAX];
  char *errmsg = NULL;
  int created = 0;

  if (env && (!strcmp(env, "0") || !strcmp(env, "off")))
    return 0;
  if (env && *env) {
    strlcpy(path, env, sizeof(path));
    created = 1;
  } else if (root) {
    snprintf(path, sizeof(path), "%s/settings.sqlite", root);
    if (access(path, F_OK) != 0)
      return 0;
  } else {
    return 0;
  }

  if (sqlite3_open_v2(path, &settings_db,
                      SQLITE_OPEN_READWRITE | (created ? SQLITE_OPEN_CREATE : 0),
                      NULL) != SQLITE_OK) {
    tvhalert(LS_SETTINGS, "Unable to open settings database \"%s\" - %s",
             path, settings_db ? sqlite3_errmsg(settings_db) : strerror(errno));
    sqlite3_close(settings_db);
    settings_db = NULL;
    return -1;
  }
  sqlite3_busy_timeout(settings_db, 5000);
  if (settings_sqlite_exec("PRAGMA journal_mode=WAL") ||
      settings_sqlite_exec("PRAGMA synchronous=NORMAL") ||
      sqlite3_exec(settings_db,
                   "CREATE TABLE IF NOT EXISTS nodes ("
                   " path TEXT PRIMARY KEY,"
                   " parent TEXT,"
                   " name TEXT NOT NULL,"
                   " kind INTEGER NOT NULL,"
                   " ordinal INTEGER NOT NULL,"
                   " data BLOB);"
                   "CREATE INDEX IF NOT EXISTS nodes_children"
                   " ON nodes(parent, ordinal);"
                   "INSERT OR IGNORE INTO nodes(path, parent, name, kind, ordinal, data)"
                   " VALUES('', NULL, '', 1, 0, NULL);",
                   NULL, NULL, &errmsg) != SQLITE_OK) {
    tvhalert(LS_SETTINGS, "Unable to prepare settings database \"%s\" - %s",
             path, errmsg ? errmsg : sqlite3_errmsg(settings_db));
    sqlite3_free(errmsg);
    sqlite3_close(settings_db);
    settings_db = NULL;
    return -1;
  }
  settings_root = strdup(root ? root : "");
  settings_db_active = 1;
  tvhinfo(LS_SETTINGS, "settings database %s", path);
  return 0;
}

void
hts_settings_sqlite_done(void)
{
  tvh_mutex_lock(&settings_db_lock);
  settings_db_active = 0;
  sqlite3_close(settings_db);
  settings_db = NULL;
  free(settings_root);
  settings_root = NULL;
  tvh_mutex_unlock(&settings_db_lock);
}

int
hts_settings_sqlite_active(void)
{
  return settings_db_active;
}

static const char *
settings_sqlite_rel(const char *fullpath)
{
  size_t n;
  const char *rel, *p;

  if (!settings_root || !settings_root[0] || !fullpath)
    return NULL;
  n = strlen(settings_root);
  if (strncmp(fullpath, settings_root, n))
    return NULL;
  if (fullpath[n] == 0)
    return "";
  if (fullpath[n] != '/')
    return NULL;
  rel = fullpath + n + 1;
  if (rel[0] == 0)
    return "";
  for (p = rel; *p; p++) {
    if (*p == '/' && (p[1] == '/' || p[1] == 0))
      return NULL;
  }
  p = rel;
  while (*p) {
    const char *slash = strchr(p, '/');
    size_t len = slash ? (size_t)(slash - p) : strlen(p);
    if (len == 0 || (len == 1 && p[0] == '.') ||
        (len == 2 && p[0] == '.' && p[1] == '.'))
      return NULL;
    if (!slash)
      break;
    p = slash + 1;
  }
  return rel;
}

int
hts_settings_sqlite_owns(const char *fullpath)
{
  return settings_sqlite_rel(fullpath) != NULL;
}

static int
settings_sqlite_exec(const char *sql)
{
  char *errmsg = NULL;
  int rc = sqlite3_exec(settings_db, sql, NULL, NULL, &errmsg);
  if (rc != SQLITE_OK) {
    tvhalert(LS_SETTINGS, "settings database: %s", errmsg ? errmsg : sql);
    sqlite3_free(errmsg);
    return -1;
  }
  return 0;
}

static int
settings_sqlite_begin(void)
{
  return settings_sqlite_exec("BEGIN IMMEDIATE");
}

static int
settings_sqlite_commit(void)
{
  return settings_sqlite_exec("COMMIT");
}

static void
settings_sqlite_rollback(void)
{
  sqlite3_exec(settings_db, "ROLLBACK", NULL, NULL, NULL);
}

static int
settings_sqlite_lookup(const char *rel, int *kind)
{
  sqlite3_stmt *st = NULL;
  int rc, found = 0;

  if (sqlite3_prepare_v2(settings_db,
                         "SELECT kind FROM nodes WHERE path=?1",
                         -1, &st, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(st, 1, rel, -1, SQLITE_STATIC);
  rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    *kind = sqlite3_column_int(st, 0);
    found = 1;
  }
  sqlite3_finalize(st);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    return -1;
  return found;
}

/* rel is a directory path. "" is the root, which is created at open. */
static int
settings_sqlite_ensure_dir(const char *rel)
{
  const char *cursor = rel;
  char path[PATH_MAX];
  size_t filled = 0;

  if (!rel[0])
    return 0;
  while (*cursor) {
    const char *slash = strchr(cursor, '/');
    size_t nlen = slash ? (size_t)(slash - cursor) : strlen(cursor);
    char parent[PATH_MAX];
    sqlite3_stmt *st = NULL;
    int kind = 0, found;

    if (filled + 1 + nlen >= sizeof(path))
      return -1;
    if (filled) {
      strlcpy(parent, path, sizeof(parent));
      path[filled++] = '/';
    } else {
      parent[0] = 0;
    }
    memcpy(path + filled, cursor, nlen);
    filled += nlen;
    path[filled] = 0;

    found = settings_sqlite_lookup(path, &kind);
    if (found < 0)
      return -1;
    if (found) {
      if (kind != 1) {
        tvhalert(LS_SETTINGS, "settings path \"%s\" is a file, not a directory", path);
        return -1;
      }
    } else {
      if (sqlite3_prepare_v2(settings_db,
                             "INSERT INTO nodes(path, parent, name, kind, ordinal, data)"
                             " VALUES(?1, ?2, ?3, 1,"
                             " COALESCE((SELECT MAX(ordinal) FROM nodes WHERE parent=?2), -1) + 1,"
                             " NULL)",
                             -1, &st, NULL) != SQLITE_OK)
        return -1;
      sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
      sqlite3_bind_text(st, 2, parent, -1, SQLITE_STATIC);
      sqlite3_bind_text(st, 3, cursor, (int)nlen, SQLITE_STATIC);
      if (sqlite3_step(st) != SQLITE_DONE) {
        tvhalert(LS_SETTINGS, "settings database insert failed for \"%s\" - %s",
                 path, sqlite3_errmsg(settings_db));
        sqlite3_finalize(st);
        return -1;
      }
      sqlite3_finalize(st);
    }
    if (!slash)
      break;
    cursor = slash + 1;
  }
  return 0;
}

static htsmsg_t *
settings_sqlite_decode(const char *fullpath, char *mem, size_t size)
{
  htsmsg_t *r = NULL;

  if (size > 12 && memcmp(mem, "\xff\xffGZIP0", 7) == 0 &&
      (mem[7] == '0' || mem[7] == '1')) {
#if ENABLE_ZLIB
    uint32_t orig = ((uint32_t)mem[8] << 24) | ((uint32_t)mem[9] << 16) |
                    ((uint32_t)mem[10] << 8) | (uint32_t)mem[11];
    if (orig > 10 * 1024 * 1024U) {
      tvhalert(LS_SETTINGS, "too big gzip for %s", fullpath);
    } else if (orig > 0) {
      uint8_t *unpacked = tvh_gzip_inflate((uint8_t *)mem + 12, size - 12, orig);
      if (unpacked) {
        if (mem[7] == '1')
          r = htsmsg_binary2_deserialize0(unpacked, orig, NULL);
        else
          r = htsmsg_binary_deserialize0(unpacked, orig, NULL);
        free(unpacked);
      }
    }
#else
    tvhalert(LS_SETTINGS, "gzip settings in \"%s\" but zlib is disabled", fullpath);
#endif
  } else {
    mem[size] = 0;
    r = htsmsg_json_deserialize(mem);
  }
  return r;
}

typedef struct settings_sqlite_row {
  char    *name;
  int      kind;
  char    *data;
  int      size;
  htsmsg_t *msg;
} settings_sqlite_row_t;

typedef struct settings_sqlite_parallel {
  settings_sqlite_row_t *rows;
  char                  *fullpath;
  int                    count;
  volatile int           next;
} settings_sqlite_parallel_t;

static int
settings_sqlite_skip_name(const char *name)
{
  size_t n = strlen(name);
  return name[0] == '.' || name[0] == '\0' || (n && name[n - 1] == '~');
}

static void *
settings_sqlite_decode_worker(void *aux)
{
  settings_sqlite_parallel_t *sp = aux;
  int i;

  while ((i = atomic_add(&sp->next, 1)) < sp->count) {
    settings_sqlite_row_t *row = &sp->rows[i];
    char child[PATH_MAX];
    if (row->kind != 0 || settings_sqlite_skip_name(row->name) || !row->data)
      continue;
    if (settings_sqlite_join(child, sizeof(child), sp->fullpath, row->name))
      continue;
    row->msg = settings_sqlite_decode(child, row->data, row->size);
  }
  return NULL;
}

static void
settings_sqlite_decode_parallel(const char *fullpath, settings_sqlite_row_t *rows, int count)
{
  settings_sqlite_parallel_t sp = {
    .rows = rows, .fullpath = (char *)fullpath, .count = count, .next = 0,
  };
  pthread_t threads[SETTINGS_PARALLEL_THREADS - 1];
  long cpus = sysconf(_SC_NPROCESSORS_ONLN);
  int i, started = 0;
  int wanted = (int)MIN(MAX(cpus, 1), SETTINGS_PARALLEL_THREADS) - 1;

  for (i = 0; i < wanted; i++) {
    if (tvh_thread_create(&threads[started], NULL,
                          settings_sqlite_decode_worker, &sp, "settings"))
      break;
    started++;
  }
  settings_sqlite_decode_worker(&sp);
  for (i = 0; i < started; i++)
    pthread_join(threads[i], NULL);
}

static htsmsg_t *
settings_sqlite_load_rel(const char *rel, int depth)
{
  sqlite3_stmt *st = NULL;
  int kind = 0, found, count = 0, cap = 0, i, parallel = 0;
  settings_sqlite_row_t *rows = NULL;
  htsmsg_t *map;
  char fullpath[PATH_MAX];

  found = settings_sqlite_lookup(rel, &kind);
  if (found <= 0)
    return NULL;
  if (kind == 0) {
    char *mem = NULL;
    int size = 0;
    htsmsg_t *msg;
    if (sqlite3_prepare_v2(settings_db,
                           "SELECT data FROM nodes WHERE path=?1",
                           -1, &st, NULL) != SQLITE_OK)
      return NULL;
    sqlite3_bind_text(st, 1, rel, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
      size = sqlite3_column_bytes(st, 0);
      mem = malloc(size + 1);
      if (mem)
        memcpy(mem, sqlite3_column_blob(st, 0), size);
    }
    sqlite3_finalize(st);
    if (!mem)
      return NULL;
    if (settings_sqlite_join(fullpath, sizeof(fullpath), settings_root, rel))
      return NULL;
    msg = settings_sqlite_decode(fullpath, mem, size);
    free(mem);
    return msg;
  }

  if (sqlite3_prepare_v2(settings_db,
                         "SELECT name, kind, data FROM nodes"
                         " WHERE parent=?1 ORDER BY ordinal, rowid",
                         -1, &st, NULL) != SQLITE_OK) {
    tvhalert(LS_SETTINGS, "settings database read failed - %s",
             sqlite3_errmsg(settings_db));
    return NULL;
  }
  sqlite3_bind_text(st, 1, rel, -1, SQLITE_STATIC);
  while ((kind = sqlite3_step(st)) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(st, 0);
    settings_sqlite_row_t *grown;
    int nsize;
    if (!name)
      continue;
    if (count == cap) {
      cap = cap ? cap * 2 : 64;
      grown = realloc(rows, cap * sizeof(*rows));
      if (!grown) {
        sqlite3_finalize(st);
        goto fail;
      }
      rows = grown;
    }
    rows[count].name = strdup(name);
    if (!rows[count].name) {
      sqlite3_finalize(st);
      goto fail;
    }
    rows[count].kind = sqlite3_column_int(st, 1);
    rows[count].msg = NULL;
    rows[count].data = NULL;
    rows[count].size = 0;
    if (rows[count].kind == 0 && sqlite3_column_type(st, 2) != SQLITE_NULL) {
      nsize = sqlite3_column_bytes(st, 2);
      rows[count].data = malloc(nsize + 1);
      if (!rows[count].data) {
        sqlite3_finalize(st);
        goto fail;
      }
      memcpy(rows[count].data, sqlite3_column_blob(st, 2), nsize);
      rows[count].size = nsize;
    }
    count++;
  }
  if (kind != SQLITE_DONE) {
    tvhalert(LS_SETTINGS, "settings database read failed - %s",
             sqlite3_errmsg(settings_db));
    sqlite3_finalize(st);
    goto fail;
  }
  sqlite3_finalize(st);
  st = NULL;

  if (settings_sqlite_join(fullpath, sizeof(fullpath), settings_root, rel))
    return NULL;
  if (count >= SETTINGS_PARALLEL_MIN) {
    /* Rows are already copied. Decoding does not touch the database, and
     * startup drops the global lock around this read so a channel dump can
     * run. Releasing this lock too avoids a lock-order inversion. */
    tvh_mutex_unlock(&settings_db_lock);
    settings_sqlite_decode_parallel(fullpath, rows, count);
    tvh_mutex_lock(&settings_db_lock);
    parallel = 1;
  }

  map = htsmsg_create_map();
  for (i = 0; i < count; i++) {
    htsmsg_t *child = NULL;
    if (!settings_sqlite_skip_name(rows[i].name)) {
      if (rows[i].kind == 1 && depth > 0) {
        char sub[PATH_MAX];
        if (!settings_sqlite_join(sub, sizeof(sub), rel, rows[i].name))
          child = settings_sqlite_load_rel(sub, depth - 1);
      } else if (rows[i].kind == 0) {
        if (parallel)
          child = rows[i].msg;
        else if (rows[i].data) {
          char childpath[PATH_MAX];
          if (!settings_sqlite_join(childpath, sizeof(childpath), fullpath, rows[i].name))
            child = settings_sqlite_decode(childpath, rows[i].data, rows[i].size);
        }
        rows[i].msg = NULL;
      }
      if (child)
        htsmsg_add_msg(map, rows[i].name, child);
    } else if (rows[i].msg) {
      htsmsg_destroy(rows[i].msg);
    }
    free(rows[i].name);
    free(rows[i].data);
  }
  free(rows);
  return map;

fail:
  for (i = 0; i < count; i++) {
    htsmsg_destroy(rows[i].msg);
    free(rows[i].name);
    free(rows[i].data);
  }
  free(rows);
  return NULL;
}

htsmsg_t *
hts_settings_sqlite_load(const char *fullpath, int depth)
{
  const char *rel;
  htsmsg_t *msg;

  if (!settings_db_active)
    return NULL;
  tvh_mutex_lock(&settings_db_lock);
  rel = settings_sqlite_rel(fullpath);
  msg = rel ? settings_sqlite_load_rel(rel, depth) : NULL;
  tvh_mutex_unlock(&settings_db_lock);
  return msg;
}

int
hts_settings_sqlite_put(const char *fullpath, const void *data, size_t len)
{
  const char *rel, *slash;
  char parent[PATH_MAX], name[PATH_MAX];
  sqlite3_stmt *st = NULL;
  int kind = 0, found, rc = -1;

  if (!settings_db_active)
    return -1;
  tvh_mutex_lock(&settings_db_lock);
  rel = settings_sqlite_rel(fullpath);
  if (!rel || !rel[0]) {
    tvhalert(LS_SETTINGS, "refusing to store a settings row outside the database");
    goto out;
  }
  if (settings_sqlite_begin())
    goto out;
  slash = strrchr(rel, '/');
  if (slash) {
    if ((size_t)(slash - rel) >= sizeof(parent))
      goto rollback;
    memcpy(parent, rel, slash - rel);
    parent[slash - rel] = 0;
    strlcpy(name, slash + 1, sizeof(name));
  } else {
    parent[0] = 0;
    strlcpy(name, rel, sizeof(name));
  }
  if (settings_sqlite_ensure_dir(parent))
    goto rollback;
  found = settings_sqlite_lookup(rel, &kind);
  if (found < 0)
    goto rollback;
  if (found && kind == 1) {
    /* Legacy muxes are directories containing config/services; saving a
     * mux replaces that tree with one packed record, just like the file
     * backend's rename-over-directory path. Keep replacement atomic.
     * With BINARY path ordering, '/' <= suffix < '0' selects exactly
     * descendants and uses the primary-key index instead of a full scan. */
    if (sqlite3_prepare_v2(settings_db,
                           "DELETE FROM nodes WHERE path >= ?1 || '/'"
                           " AND path < ?1 || '0'",
                           -1, &st, NULL) != SQLITE_OK)
      goto rollback;
    sqlite3_bind_text(st, 1, rel, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_DONE) {
      sqlite3_finalize(st);
      goto rollback;
    }
    sqlite3_finalize(st);
    st = NULL;
  }
  if (found) {
    if (sqlite3_prepare_v2(settings_db,
                           "UPDATE nodes SET kind=0, data=?1 WHERE path=?2",
                           -1, &st, NULL) != SQLITE_OK)
      goto rollback;
    sqlite3_bind_blob(st, 1, data, (int)len, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, rel, -1, SQLITE_STATIC);
  } else {
    if (sqlite3_prepare_v2(settings_db,
                           "INSERT INTO nodes(path, parent, name, kind, ordinal, data)"
                           " VALUES(?1, ?2, ?3, 0,"
                           " COALESCE((SELECT MAX(ordinal) FROM nodes WHERE parent=?2), -1) + 1,"
                           " ?4)",
                           -1, &st, NULL) != SQLITE_OK)
      goto rollback;
    sqlite3_bind_text(st, 1, rel, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, parent, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, name, -1, SQLITE_STATIC);
    sqlite3_bind_blob(st, 4, data, (int)len, SQLITE_STATIC);
  }
  if (sqlite3_step(st) != SQLITE_DONE) {
    tvhalert(LS_SETTINGS, "settings database write failed for \"%s\" - %s",
             rel, sqlite3_errmsg(settings_db));
    sqlite3_finalize(st);
    goto rollback;
  }
  sqlite3_finalize(st);
  if (settings_sqlite_commit())
    goto rollback;
  rc = 0;
  goto out;
rollback:
  settings_sqlite_rollback();
out:
  tvh_mutex_unlock(&settings_db_lock);
  return rc;
}

static void
settings_sqlite_prune(const char *rel)
{
  char path[PATH_MAX];
  sqlite3_stmt *st = NULL;

  strlcpy(path, rel, sizeof(path));
  while (path[0]) {
    char *slash;
    int children = 0;
    if (sqlite3_prepare_v2(settings_db,
                           "SELECT COUNT(*) FROM nodes WHERE parent=?1",
                           -1, &st, NULL) != SQLITE_OK)
      return;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW)
      children = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    st = NULL;
    if (children)
      return;
    if (sqlite3_prepare_v2(settings_db,
                           "DELETE FROM nodes WHERE path=?1 AND kind=1",
                           -1, &st, NULL) != SQLITE_OK)
      return;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
    st = NULL;
    slash = strrchr(path, '/');
    if (!slash)
      path[0] = 0;
    else
      *slash = 0;
  }
}

void
hts_settings_sqlite_remove(const char *fullpath)
{
  const char *rel;
  char parent[PATH_MAX];
  sqlite3_stmt *st = NULL;
  int kind = 0, found;

  if (!settings_db_active)
    return;
  tvh_mutex_lock(&settings_db_lock);
  rel = settings_sqlite_rel(fullpath);
  if (!rel)
    goto out;
  if (settings_sqlite_begin())
    goto out;
  found = settings_sqlite_lookup(rel, &kind);
  if (found < 0)
    goto rollback;
  if (found && !rel[0]) {
    /* Removing the settings root clears every record and leaves the root. */
    if (sqlite3_prepare_v2(settings_db,
                           "DELETE FROM nodes WHERE path<>''",
                           -1, &st, NULL) != SQLITE_OK)
      goto rollback;
  } else if (found && kind == 1) {
    if (sqlite3_prepare_v2(settings_db,
                           "DELETE FROM nodes WHERE path=?1"
                           " OR (path >= ?1 || '/'"
                           " AND path < ?1 || '0')",
                           -1, &st, NULL) != SQLITE_OK)
      goto rollback;
    sqlite3_bind_text(st, 1, rel, -1, SQLITE_STATIC);
  } else if (found) {
    if (sqlite3_prepare_v2(settings_db,
                           "DELETE FROM nodes WHERE path=?1",
                           -1, &st, NULL) != SQLITE_OK)
      goto rollback;
    sqlite3_bind_text(st, 1, rel, -1, SQLITE_STATIC);
  }
  if (st && sqlite3_step(st) != SQLITE_DONE) {
    tvhalert(LS_SETTINGS, "settings database remove failed for \"%s\" - %s",
             rel, sqlite3_errmsg(settings_db));
    sqlite3_finalize(st);
    goto rollback;
  }
  sqlite3_finalize(st);
  st = NULL;
  if (found && rel[0] && kind == 0) {
    const char *slash = strrchr(rel, '/');
    if (slash) {
      memcpy(parent, rel, slash - rel);
      parent[slash - rel] = 0;
    } else {
      parent[0] = 0;
    }
    if (parent[0])
      settings_sqlite_prune(parent);
  } else if (found && kind == 1 && rel[0]) {
    const char *slash = strrchr(rel, '/');
    if (slash) {
      memcpy(parent, rel, slash - rel);
      parent[slash - rel] = 0;
      if (parent[0])
        settings_sqlite_prune(parent);
    }
  }
  if (settings_sqlite_commit())
    goto rollback;
  goto out;
rollback:
  settings_sqlite_rollback();
out:
  tvh_mutex_unlock(&settings_db_lock);
}

int
hts_settings_sqlite_exists(const char *fullpath)
{
  const char *rel;
  int kind = 0, found = 0;

  if (!settings_db_active)
    return 0;
  tvh_mutex_lock(&settings_db_lock);
  rel = settings_sqlite_rel(fullpath);
  if (rel)
    found = settings_sqlite_lookup(rel, &kind) > 0;
  tvh_mutex_unlock(&settings_db_lock);
  return found;
}

int
hts_settings_sqlite_makedirs(const char *fullpath)
{
  const char *rel, *slash;
  char parent[PATH_MAX];
  int rc = -1;

  if (!settings_db_active)
    return 0;
  tvh_mutex_lock(&settings_db_lock);
  rel = settings_sqlite_rel(fullpath);
  if (!rel) {
    rc = 0;
    goto out;
  }
  slash = strrchr(rel, '/');
  if (!slash) {
    rc = 0; /* parent is the root */
    goto out;
  }
  if ((size_t)(slash - rel) >= sizeof(parent))
    goto out;
  memcpy(parent, rel, slash - rel);
  parent[slash - rel] = 0;
  if (settings_sqlite_begin())
    goto out;
  if (settings_sqlite_ensure_dir(parent)) {
    settings_sqlite_rollback();
    goto out;
  }
  if (settings_sqlite_commit()) {
    settings_sqlite_rollback();
    goto out;
  }
  rc = 0;
out:
  tvh_mutex_unlock(&settings_db_lock);
  return rc;
}
