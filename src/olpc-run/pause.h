/* In-game pause menu: the model (cursor, slot, texts) and its RGB565 rendering into RAM.  Black on
 * white with an inverted cursor row, so it reads the same in DCON mono.  The caller (olpc-run.c)
 * owns the input loop, the core and the framebuffer. */
#ifndef OLPC_RUN_PAUSE_H
#define OLPC_RUN_PAUSE_H
#include <stdint.h>
#include <time.h>
#include "state.h"

#define PM_W 600
#define PM_H 472

enum { PM_RESUME, PM_SAVE, PM_LOAD, PM_SLOT, PM_QUIT, PM_N };
enum { PK_UP, PK_DOWN, PK_LEFT, PK_RIGHT, PK_A, PK_B };
enum { PA_NONE, PA_REDRAW, PA_RESUME, PA_SAVE, PA_LOAD, PA_QUIT };

struct pmenu {
	int cur;                       /* PM_* row */
	int slot;                      /* 1..ST_SLOTS */
	int supported;                 /* the core serializes */
	time_t when[ST_SLOTS + 1];     /* mtime per slot, -1 = empty */
	int h24;
	char msg[48];
	uint16_t px[PM_W * PM_H];
};

void pm_init(struct pmenu *m, int slot, int supported, int h24);
/* PA_* for the caller to carry out; cursor and slot changes are done here (PA_REDRAW) */
int  pm_key(struct pmenu *m, int key);
void pm_msg(struct pmenu *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void pm_draw(struct pmenu *m);
#endif
