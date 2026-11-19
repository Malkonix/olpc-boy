#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "font8x16.h"

void gfx_fill(const struct gfx *g, int x, int y, int w, int h, uint16_t c)
{
	int xx, yy, x1 = x + w, y1 = y + h;

	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x1 > g->w) x1 = g->w;
	if (y1 > g->h) y1 = g->h;
	if (x1 <= x || y1 <= y) return;
	for (yy = y; yy < y1; yy++) {
		uint16_t *p = g->px + (size_t)yy * g->stride;
		for (xx = x; xx < x1; xx++) p[xx] = c;
	}
}

int gfx_text_w(const unsigned char *s, int sc) { return (int)strlen((const char *)s) * 8 * sc; }

void gfx_text(const struct gfx *g, int x, int y, int sc, uint16_t fg, const unsigned char *s)
{
	for (; *s; s++, x += 8 * sc) {
		const unsigned char *gl;
		int row, col, sx, sy;
		unsigned c = *s;

		if (c < 0x20) c = '?';
		gl = font8x16[c - 0x20];
		if (x < 0 || x + 8 * sc > g->w || y < 0 || y + 16 * sc > g->h) continue;
		for (row = 0; row < 16; row++) {
			unsigned bits = gl[row];
			if (!bits) continue;
			for (sy = 0; sy < sc; sy++) {
				uint16_t *d = g->px + (size_t)(y + row * sc + sy) * g->stride + x;
				for (col = 0; col < 8; col++)
					if (bits & (0x80u >> col))
						for (sx = 0; sx < sc; sx++) d[col * sc + sx] = fg;
			}
		}
	}
}

void gfx_stroke(const struct gfx *g, int x0, int y0, int x1, int y1, uint16_t c)
{
	int i, n = (abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0)) / 3;

	for (i = 0; i <= n; i++)
		gfx_fill(g, x0 + (x1 - x0) * i / (n ? n : 1) - 1, y0 + (y1 - y0) * i / (n ? n : 1) - 1, 3, 3, c);
}

uint16_t gfx_blend(uint16_t a, uint16_t b, int t)
{
	int r = (((a >> 11) & 31) * (256 - t) + ((b >> 11) & 31) * t) >> 8;
	int gg = (((a >> 5) & 63) * (256 - t) + ((b >> 5) & 63) * t) >> 8;
	int bl = ((a & 31) * (256 - t) + (b & 31) * t) >> 8;

	return (uint16_t)((r << 11) | (gg << 5) | bl);
}

static void ind_num(const struct gfx *g, int x, int y, int w, int pct, uint16_t col)
{
	char b[16];

	if (pct < 0) snprintf(b, sizeof b, " --"); else snprintf(b, sizeof b, "%3d", pct);
	gfx_text(g, x + w - 48, y + IND_SLOT_H / 2 - 16, 2, col, (const unsigned char *)b);
}

void ind_wifi(const struct gfx *g, int x, int y, const struct ind_pal *p, int st, int bars)
{
	int i, n = st == WS_CONNECTED ? bars : st == WS_CONNECTING ? 2 : 0;
	uint16_t on = st == WS_CONNECTING ? p->dim : p->on, off = p->faint;

	gfx_fill(g, x, y, IND_W_WIFI, IND_SLOT_H, p->bg);
	for (i = 0; i < 4; i++) { int h = 12 + 10 * i; gfx_fill(g, x + i * 12, y + 54 - h, 8, h, i < n ? on : off); }
	if (st != WS_CONNECTED && st != WS_CONNECTING) gfx_stroke(g, x + 2, y + 14, x + IND_W_WIFI - 2, y + 52, p->strike);
}

void ind_vol(const struct gfx *g, int x, int y, const struct ind_pal *p, int pct, int mute)
{
	int cy = y + IND_SLOT_H / 2;
	uint16_t col = pct < 0 ? p->faint : mute ? p->dim : p->on;

	gfx_fill(g, x, y, IND_W_VOL, IND_SLOT_H, p->bg);
	gfx_fill(g, x, cy - 7, 8, 14, col);                              /* speaker: box, then a stepped cone */
	gfx_fill(g, x + 8, cy - 9, 4, 18, col); gfx_fill(g, x + 12, cy - 13, 4, 26, col); gfx_fill(g, x + 16, cy - 17, 4, 34, col);
	if (!mute && pct > 0) gfx_fill(g, x + 22, cy - 5, 3, 10, col);
	if (!mute && pct >= 34) gfx_fill(g, x + 27, cy - 11, 3, 22, col);
	if (mute) { gfx_stroke(g, x + 22, cy - 9, x + 38, cy + 9, p->strike); gfx_stroke(g, x + 38, cy - 9, x + 22, cy + 9, p->strike); }
	ind_num(g, x, y, IND_W_VOL, pct, col);
}

void ind_bri(const struct gfx *g, int x, int y, const struct ind_pal *p, int pct)
{
	static const int ray[8][2] = { { 15, 0 }, { -15, 0 }, { 0, 15 }, { 0, -15 }, { 11, 11 }, { -11, 11 }, { 11, -11 }, { -11, -11 } };
	int cx = x + 18, cy = y + IND_SLOT_H / 2, dy, i;
	uint16_t col = pct < 0 ? p->faint : p->on;

	gfx_fill(g, x, y, IND_W_BRI, IND_SLOT_H, p->bg);
	for (dy = -8; dy <= 8; dy++) {                                   /* sun: disc and eight rays */
		int hw = 0;
		while ((hw + 1) * (hw + 1) + dy * dy <= 64) hw++;
		gfx_fill(g, cx - hw, cy + dy, 2 * hw + 1, 1, col);
	}
	for (i = 0; i < 8; i++) gfx_fill(g, cx + ray[i][0] - 2, cy + ray[i][1] - 2, 4, 4, col);
	ind_num(g, x, y, IND_W_BRI, pct, col);
}
