/* Save states: <statedir>/<base>.st1..st4, plus <base>.slot (last slot used).  Files are written
 * atomically (sv_write_atomic).  The core is reached through three callbacks so this stays testable. */
#ifndef OLPC_RUN_STATE_H
#define OLPC_RUN_STATE_H
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define ST_SLOTS 4

struct core_ser {
	size_t (*size)(void);
	bool (*ser)(void *, size_t);
	bool (*unser)(const void *, size_t);
};

enum { ST_OK = 0, ST_IO = -1, ST_UNSUPPORTED = -2, ST_BADSIZE = -3, ST_REFUSED = -4, ST_EMPTY = -5 };

void st_path(char *out, size_t cap, const char *dir, const char *base, int slot);
int  st_save(const struct core_ser *c, const char *dir, const char *path);
int  st_load(const struct core_ser *c, const char *path);
/* modification time of the slot file; 0 -1 when it does not exist */
time_t st_stamp(const char *path);
/* "2:32 PM Nov 20" / "14:32 Nov 20" */
void st_when(char *out, size_t cap, time_t t, int h24);
/* last slot used (1..ST_SLOTS, default 1) and its writer */
int  st_slot_read(const char *dir, const char *base);
void st_slot_write(const char *dir, const char *base, int slot);
#endif
