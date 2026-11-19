/* Test-only interposer: pause the first SQLite mux read after global_lock
 * has been released. A file handshake makes concurrency checks deterministic. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int sqlite3_step(sqlite3_stmt *stmt)
{
  static int (*step)(sqlite3_stmt *);
  static int paused;
  const char *gate = getenv("TVH_TEST_STARTUP_GATE");
  char *sql;
  if (!step)
    step = dlsym(RTLD_NEXT, "sqlite3_step");
  sql = sqlite3_expanded_sql(stmt);
  if (gate && sql && strstr(sql, "SELECT name, kind, data FROM nodes") &&
      strstr(sql, "/muxes'") && !__sync_lock_test_and_set(&paused, 1)) {
    char marker[4096];
    snprintf(marker, sizeof(marker), "%s.entered", gate);
    FILE *f = fopen(marker, "w");
    if (f) fclose(f);
    /* Bound the pause even if the controlling test fails. */
    for (int n = 0; n < 6000 && access(gate, F_OK) == 0; n++)
      usleep(10000);
  }
  sqlite3_free(sql);
  return step(stmt);
}
