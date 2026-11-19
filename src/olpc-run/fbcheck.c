/* fbcheck FILE X0 Y0 W H [SX SY SW SH]: count non-black RGB565 pixels of a 1200x900 raw dump inside
 * and outside the game rectangle.  With a strip rectangle (the in-game indicators) the pixels in it
 * are counted apart and are not "outside".  Prints "inside=N outside=M" (plus " strip=K" with a strip).
 *
 * fbcheck --diff A B X0 Y0 W H [SX SY SW SH]: compare two dumps; prints "rect=N other=M" (plus
 * " strip=K"): pixels that differ inside the game rectangle, outside both rectangles, and inside the
 * strip.  --png FILE OUT.ppm: write a binary PPM (for looking at a dump). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

enum { FW = 1200, FH = 900 };
static uint16_t pa[FW * FH], pb[FW * FH];

static int load(const char *name, uint16_t *px)
{
	FILE *f = fopen(name, "rb");

	if (!f || fread(px, 2, FW * FH, f) != FW * FH) { fprintf(stderr, "fbcheck: short or missing %s\n", name); if (f) fclose(f); return -1; }
	fclose(f);
	return 0;
}

static int inr(int x, int y, const int *r) { return r[2] > 0 && x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]; }

int main(int argc, char **argv)
{
	int g[4] = { 0, 0, 0, 0 }, s[4] = { 0, 0, 0, 0 }, hasstrip, x, y, a, base;
	long c0 = 0, c1 = 0, c2 = 0;

	if (argc == 4 && !strcmp(argv[1], "--png")) {
		FILE *o;
		if (load(argv[2], pa) < 0 || !(o = fopen(argv[3], "wb"))) return 2;
		fprintf(o, "P6\n%d %d\n255\n", FW, FH);
		for (y = 0; y < FH * FW; y++) {
			uint16_t v = pa[y];
			unsigned char p[3] = { (unsigned char)(((v >> 11) & 31) * 255 / 31), (unsigned char)(((v >> 5) & 63) * 255 / 63), (unsigned char)((v & 31) * 255 / 31) };
			fwrite(p, 1, 3, o);
		}
		fclose(o);
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "--diff")) {
		if (argc != 8 && argc != 12) { fprintf(stderr, "usage: fbcheck --diff a b x0 y0 w h [sx sy sw sh]\n"); return 2; }
		if (load(argv[2], pa) < 0 || load(argv[3], pb) < 0) return 2;
		base = 4;
	} else {
		if (argc != 6 && argc != 10) { fprintf(stderr, "usage: fbcheck file x0 y0 w h [sx sy sw sh]\n"); return 2; }
		if (load(argv[1], pa) < 0) return 2;
		base = 2;
	}
	for (a = 0; a < 4; a++) g[a] = atoi(argv[base + a]);
	hasstrip = argc > base + 4;
	if (hasstrip) for (a = 0; a < 4; a++) s[a] = atoi(argv[base + 4 + a]);
	for (y = 0; y < FH; y++)
		for (x = 0; x < FW; x++) {
			int hit = base == 4 ? pa[y * FW + x] != pb[y * FW + x] : pa[y * FW + x] != 0;
			if (!hit) continue;
			if (inr(x, y, g)) c0++; else if (inr(x, y, s)) c2++; else c1++;
		}
	if (base == 4) printf("rect=%ld other=%ld", c0, c1); else printf("inside=%ld outside=%ld", c0, c1);
	if (hasstrip) printf(" strip=%ld", c2);
	printf("\n");
	return 0;
}
