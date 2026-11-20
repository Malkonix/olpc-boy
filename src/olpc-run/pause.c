/* Pause menu model and rendering, see pause.h. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "pause.h"
#include "../common/gfx.h"

#define BLACK 0x0000
#define WHITE 0xffff
#define BORDER 8
#define ROW_Y0 88
#define ROW_H 56

static const char *const ITEM[PM_N] = { "Resume", "Save state", "Load state", NULL, "Quit game" };

void pm_init(struct pmenu *m, int slot, int supported, int h24)
{
	int i;

	m->cur = PM_RESUME;
	m->slot = slot;
	m->supported = supported;
	m->h24 = h24;
	m->msg[0] = 0;
	for (i = 0; i <= ST_SLOTS; i++) m->when[i] = (time_t)-1;
	if (!supported) snprintf(m->msg, sizeof m->msg, "Save states not supported");
}

void pm_msg(struct pmenu *m, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(m->msg, sizeof m->msg, fmt, ap);
	va_end(ap);
}

int pm_key(struct pmenu *m, int key)
{
	switch (key) {
	case PK_UP: m->cur = (m->cur + PM_N - 1) % PM_N; m->msg[0] = 0; return PA_REDRAW;
	case PK_DOWN: m->cur = (m->cur + 1) % PM_N; m->msg[0] = 0; return PA_REDRAW;
	case PK_LEFT: m->slot = m->slot == 1 ? ST_SLOTS : m->slot - 1; m->msg[0] = 0; return PA_REDRAW;
	case PK_RIGHT: m->slot = m->slot == ST_SLOTS ? 1 : m->slot + 1; m->msg[0] = 0; return PA_REDRAW;
	case PK_B: return PA_RESUME;
	case PK_A:
		switch (m->cur) {
		case PM_RESUME: return PA_RESUME;
		case PM_SAVE: return PA_SAVE;
		case PM_LOAD: return PA_LOAD;
		case PM_QUIT: return PA_QUIT;
		default: return PA_NONE;
		}
	}
	return PA_NONE;
}

void pm_draw(struct pmenu *m)
{
	struct gfx g = { m->px, PM_W, PM_W, PM_H };
	char line[64], when[32];
	int i;

	gfx_fill(&g, 0, 0, PM_W, PM_H, BLACK);
	gfx_fill(&g, BORDER, BORDER, PM_W - 2 * BORDER, PM_H - 2 * BORDER, WHITE);
	gfx_text(&g, 32, 28, 3, BLACK, (const unsigned char *)"PAUSED");
	gfx_fill(&g, 24, 78, PM_W - 48, 4, BLACK);
	for (i = 0; i < PM_N; i++) {
		int y = ROW_Y0 + i * ROW_H, sel = i == m->cur;

		if (i == PM_SLOT) snprintf(line, sizeof line, "Slot: < %d >", m->slot);
		else snprintf(line, sizeof line, "%s", ITEM[i]);
		if (sel) gfx_fill(&g, 24, y, PM_W - 48, 48 + 8, BLACK);
		gfx_text(&g, 40, y + 4, 3, sel ? WHITE : BLACK, (const unsigned char *)line);
	}
	if (m->supported) {
		if (m->when[m->slot] == (time_t)-1) snprintf(line, sizeof line, "Slot %d: empty", m->slot);
		else { st_when(when, sizeof when, m->when[m->slot], m->h24); snprintf(line, sizeof line, "Slot %d: saved %s", m->slot, when); }
		gfx_text(&g, 32, ROW_Y0 + PM_N * ROW_H + 8, 2, BLACK, (const unsigned char *)line);
	}
	gfx_text(&g, 32, ROW_Y0 + PM_N * ROW_H + 48, 2, BLACK, (const unsigned char *)m->msg);
}
