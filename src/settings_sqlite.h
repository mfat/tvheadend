/*
 * One settings file, one database row.
 *
 * Copyright (C) 2026 TVHplayer contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef SETTINGS_SQLITE_H__
#define SETTINGS_SQLITE_H__

#include "htsmsg.h"

/* Open <config>/settings.sqlite when that file already exists.
 * TVH_SETTINGS_SQLITE=/path creates and uses that file.
 * TVH_SETTINGS_SQLITE=off keeps the file tree even when the database exists.
 * Returns 0 when settings stay on files or the database opened. */
int hts_settings_sqlite_init(const char *root);

void hts_settings_sqlite_done(void);

int hts_settings_sqlite_active(void);

/* True when fullpath is the settings root or a path inside it. */
int hts_settings_sqlite_owns(const char *fullpath);

/* NULL when the row is absent, so the bundled data/conf fallback still runs. */
htsmsg_t *hts_settings_sqlite_load(const char *fullpath, int depth);

/* Store the same bytes a settings file would contain. 0 on success. */
int hts_settings_sqlite_put(const char *fullpath, const void *data, size_t len);

void hts_settings_sqlite_remove(const char *fullpath);

int hts_settings_sqlite_exists(const char *fullpath);

/* Create the parent directory rows of a file path. 0 on success. */
int hts_settings_sqlite_makedirs(const char *fullpath);

#endif /* SETTINGS_SQLITE_H__ */
