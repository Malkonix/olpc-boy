#include <string.h>
#include "wallpaper.h"

/* Base colours (8-bit): deep indigo at the top, dark teal at the bottom.  Kept dark so UI text keeps its
 * contrast and the luminance (what DCON monochrome shows) rises gently, 25 -> 40 of 255. */
#define TOP_R 26
#define TOP_G 18
#define TOP_B 64
#define BOT_R 8
#define BOT_G 52
#define BOT_B 60

/* 4x4 Bayer matrix, 0..15 */
static const uint8_t BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

/* Sprites: '#' outline (brighter), '+' fill (fainter), anything else transparent.  All original shapes. */
struct sprite { int w, h; const char *rows[14]; };
static const struct sprite SPR[] = {
	{ 12, 10, { "....####....", "....#++#....", "....#++#....", "#####++#####", "#++++++++++#",
		    "#++++++++++#", "#####++#####", "....#++#....", "....#++#....", "....####...." } },     /* cross pad */
	{ 14, 10, { ".........####.", "........#++++#", "........#++++#", "........#++++#", ".####...#++++#",
		    "#++++#...####.", "#++++#........", "#++++#........", "#++++#........", ".####........." } },   /* two round buttons */
	{ 12, 10, { "..########..", ".#++++++++#.", "#+########+#", "#+#++++++#+#", "#+#++++++#+#",
		    "#+#++++++#+#", "#+########+#", "#++++++++++#", "#++++++++++#", "############" } },   /* cartridge */
	{ 9, 9, { "....#....", "...#+#...", "...#+#...", "####+####", ".#+++++#.",
		  "..#+++#..", "..#+#+#..", ".#+#.#+#.", ".##...##." } },                                /* star */
	{ 11, 9, { ".###...###.", "#+++#.#+++#", "#++++#++++#", "#+++++++++#", ".#+++++++#.",
		   "..#+++++#..", "...#+++#...", "....#+#....", ".....#....." } },                      /* heart */
	{ 10, 10, { "...#######", "...#+++++#", "...#######", "...#.....#", "...#.....#",
		    "...#.....#", "...#.....#", "####..####", "#++#..#++#", ".##....##." } },            /* beamed notes */
	{ 12, 10, { ".#........#.", ".##......##.", "..########..", ".#++++++++#.", "#+..++++..+#",
		    "#+..++++..+#", "#++++++++++#", "#++++..++++#", ".#++++++++#.", ".##.####.##." } },    /* little critter */
};
#define NSPR ((int)(sizeof SPR / sizeof SPR[0]))

static uint32_t rng_state;
static uint32_t rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

/* lift each RGB565 field by (r5, g6, b5) steps, saturating */
static uint16_t lift(uint16_t p, int dr, int dg, int db)
{
	int r = (p >> 11) + dr, g = ((p >> 5) & 63) + dg, b = (p & 31) + db;

	if (r > 31) r = 31;
	if (g > 63) g = 63;
	if (b > 31) b = 31;
	return (uint16_t)((r << 11) | (g << 5) | b);
}

static void stamp(uint16_t *dst, int w, int h, int x, int y, const struct sprite *s, int sc)
{
	int j, i, a, b;

	for (j = 0; j < s->h; j++)
		for (i = 0; i < s->w; i++) {
			char c = s->rows[j][i];
			int dr, dg, db;
			if (c == '#') { dr = 1; dg = 3; db = 2; }
			else if (c == '+') { dr = 0; dg = 2; db = 1; }
			else continue;
			for (b = 0; b < sc; b++) {
				int py = y + j * sc + b;
				if (py < 0 || py >= h) continue;
				for (a = 0; a < sc; a++) {
					int px = x + i * sc + a;
					if (px >= 0 && px < w) dst[(size_t)py * w + px] = lift(dst[(size_t)py * w + px], dr, dg, db);
				}
			}
		}
}

/* distance from x to the nearest multiple of period, as a triangle wave 0..period/2 */
static int tri(int x, int period)
{
	int m = x % period;

	return m < period / 2 ? m : period - m;
}

void wp_render(uint16_t *dst, int w, int h, int skyline_y)
{
	int x, y, cx, cy;
	enum { CELL_W = 168, CELL_H = 140, GLYPH_SC = 4 };

	/* 1. gradient + dither + scanline / pixel-grid texture, 16-bit fixed point per channel */
	for (y = 0; y < h; y++) {
		int t = (int)((long)y * 256 / (h > 1 ? h - 1 : 1));         /* 0..256 */
		int r = TOP_R * 256 + (BOT_R - TOP_R) * t;                    /* 16-bit fixed point */
		int g = (TOP_G * 256 + (BOT_G - TOP_G) * t);
		int b = (TOP_B * 256 + (BOT_B - TOP_B) * t);
		uint16_t *row = dst + (size_t)y * w;
		const uint8_t *bay = BAYER[y & 3];
		int row_off = (y % 4) == 3 ? -520 : 0;                       /* faint scanline */
		int grid_row = (y % 12) == 0;                                /* faint 12 px pixel grid */
		int n = w < 12 ? w : 12;

		/* the row repeats every 12 px (lcm of the 4 px dither and the 12 px grid): compute one tile, replicate it */
		for (x = 0; x < n; x++) {
			int d = bay[x & 3];
			int off = row_off + (grid_row || x == 0 ? 700 : 0);
			int rr = r + off + d * 128, gg = g + off + d * 64, bb = b + off + d * 128;   /* steps: 2048 (r,b) / 1024 (g) */

			rr = rr < 0 ? 0 : rr >> 11; gg = gg < 0 ? 0 : gg >> 10; bb = bb < 0 ? 0 : bb >> 11;
			if (rr > 31) rr = 31;
			if (gg > 63) gg = 63;
			if (bb > 31) bb = 31;
			row[x] = (uint16_t)((rr << 11) | (gg << 5) | bb);
		}
		for (x = n; x < w; x *= 2) memcpy(row + x, row, (size_t)(w - x < x ? w - x : x) * 2);   /* doubles each pass */
	}

	/* 2. skyline: two layers of stepped (8 px columns) mountains standing on skyline_y */
	if (skyline_y > 60 && skyline_y <= h) {
		for (x = 0; x < w; x += 8) {
			int c = x / 8;
			int h1 = 24 + tri(c * 8 + 40, 360) / 3 + tri(c * 8, 104) / 4;
			int h2 = 14 + tri(c * 8 + 170, 250) / 4 + tri(c * 8 + 31, 70) / 5;
			int k;
			for (k = 0; k < 8 && x + k < w; k++) {
				for (y = skyline_y - h1; y < skyline_y; y++) dst[(size_t)y * w + x + k] = lift(dst[(size_t)y * w + x + k], 0, 1, 1);
				for (y = skyline_y - h2; y < skyline_y; y++) dst[(size_t)y * w + x + k] = lift(dst[(size_t)y * w + x + k], 0, 1, 1);
			}
		}
	}

	/* 3. glyphs on a jittered lattice */
	rng_state = 0x0A1C0DE5u;
	for (cy = 0; cy * CELL_H < h; cy++)
		for (cx = 0; cx * CELL_W < w; cx++) {
			const struct sprite *s = &SPR[(int)(rnd() % NSPR)];
			int gw = s->w * GLYPH_SC, gh = s->h * GLYPH_SC;
			int jx = (int)(rnd() % (CELL_W - gw)), jy = (int)(rnd() % (CELL_H - gh));
			if (cy % 2) jx = (jx + CELL_W / 2) % (CELL_W - gw);      /* brick offset on odd rows */
			stamp(dst, w, h, cx * CELL_W + jx, cy * CELL_H + jy, s, GLYPH_SC);
		}
}

void wp_blit(uint16_t *dst, int dstride, const uint16_t *wp, int wstride, int x, int y, int w, int h, int dim)
{
	int j, i;

	for (j = y; j < y + h; j++) {
		uint16_t *d = dst + (size_t)j * dstride + x;
		const uint16_t *s = wp + (size_t)j * wstride + x;
		if (!dim) memcpy(d, s, (size_t)w * 2);
		else for (i = 0; i < w; i++) d[i] = (uint16_t)((s[i] >> 1) & 0x7BEF);
	}
}
