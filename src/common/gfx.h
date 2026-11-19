/* RGB565 drawing shared by olpc-menu (title bar) and olpc-run (in-game strip): rectangles, Terminus
 * text, 3x3-square lines and the WiFi / speaker / sun icons.  Everything draws into RAM (struct gfx);
 * the caller decides how the pixels reach the framebuffer (the framebuffer is never read). */
#ifndef OLPC_GFX_H
#define OLPC_GFX_H
#include <stdint.h>
#include "status.h"

struct gfx { uint16_t *px; int stride, w, h; };      /* stride in pixels */

/* clipped to the surface */
void     gfx_fill(const struct gfx *g, int x, int y, int w, int h, uint16_t c);
int      gfx_text_w(const unsigned char *s, int sc);
/* Latin-1 text with the 8x16 font at scale sc, glyph pixels only (the caller painted the background);
 * a glyph that does not fit entirely inside the surface is skipped */
void     gfx_text(const struct gfx *g, int x, int y, int sc, uint16_t fg, const unsigned char *s);
/* a line of 3x3 squares between two points */
void     gfx_stroke(const struct gfx *g, int x0, int y0, int x1, int y1, uint16_t c);
uint16_t gfx_blend(uint16_t a, uint16_t b, int t);   /* t/256 of b */

/* Indicator icons.  A slot is IND_SLOT_H pixels tall and has a fixed width per icon; the slot is
 * cleared to pal->bg first, so a value change repaints only its own slot.  (x, y) = top-left. */
#define IND_SLOT_H 64
#define IND_W_WIFI 44
#define IND_W_VOL  88
#define IND_W_BRI  92
#define IND_W_BAT  96
struct ind_pal {
	uint16_t bg;
	uint16_t on;        /* normal strokes and digits */
	uint16_t dim;       /* muted speaker, WiFi connecting */
	uint16_t faint;     /* unlit WiFi bars, value unknown */
	uint16_t strike;    /* the cross over a muted speaker, the slash over WiFi off */
};
/* state = WS_*; bars = 1..4, used only when connected */
void ind_wifi(const struct gfx *g, int x, int y, const struct ind_pal *p, int state, int bars);
/* pct < 0 = unknown ("--") */
void ind_vol(const struct gfx *g, int x, int y, const struct ind_pal *p, int pct, int mute);
void ind_bri(const struct gfx *g, int x, int y, const struct ind_pal *p, int pct);
/* Battery: outline with a nub, fill proportional to pct, the percentage on the right.  pct < 0 = no battery
 * information: nothing is drawn but the cleared slot.  ac: adapter online (1), chg: charging (1) - either
 * puts a bolt in the body.  Low (<= 15 %, bat_level) shows an exclamation mark and a thinner fill; critical
 * (<= 5 %) an empty body with a heavy outline and the mark: shape and contrast, not colour, tell the
 * states apart in DCON monochrome.  low/crit strokes use p->strike. */
void ind_bat(const struct gfx *g, int x, int y, const struct ind_pal *p, int pct, int ac, int chg);
#endif
