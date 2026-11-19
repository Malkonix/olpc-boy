/*
 * Test helper (host only).
 *   t_gen png TYPE W H out.png   write a PNG: rgba pal grey blue checker noise huge
 *   t_gen obc file.obc [X Y]     check the header and size; print "W H" and, with X Y, "PIXEL hex565"
 * PNGs use stored (uncompressed) deflate blocks, so no zlib is needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t crc_table[256];
static void crc_init(void)
{
	uint32_t c, n; int k;
	for (n = 0; n < 256; n++) {
		c = n;
		for (k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
		crc_table[n] = c;
	}
}
static uint32_t crc(uint32_t c, const unsigned char *b, size_t n)
{
	while (n--) c = crc_table[(c ^ *b++) & 255] ^ (c >> 8);
	return c;
}
static void be32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *type, const unsigned char *d, uint32_t n)
{
	unsigned char b[4];
	uint32_t c;
	be32(b, n); fwrite(b, 1, 4, f);
	fwrite(type, 1, 4, f);
	if (n) fwrite(d, 1, n, f);
	c = crc(0xffffffffu, (const unsigned char *)type, 4);
	c = crc(c, d, n) ^ 0xffffffffu;
	be32(b, c); fwrite(b, 1, 4, f);
}

static void pix(const char *t, int x, int y, int w, unsigned char *p, int *nb)
{
	if (!strcmp(t, "rgba")) {
		p[0] = x < w / 2 ? 255 : 255; p[1] = x < w / 2 ? 0 : 255; p[2] = x < w / 2 ? 0 : 255;
		p[3] = x < w / 2 ? 255 : 0; *nb = 4;
	} else if (!strcmp(t, "pal")) { p[0] = (x + y) % 4; *nb = 1;
	} else if (!strcmp(t, "grey")) { p[0] = x * 60; *nb = 1;
	} else if (!strcmp(t, "blue")) { p[0] = 0; p[1] = 0; p[2] = 255; *nb = 3;
	} else if (!strcmp(t, "checker")) { p[0] = p[1] = p[2] = ((x + y) & 1) ? 255 : 0; *nb = 3;
	} else { unsigned v = (unsigned)(x * 2654435761u + y * 40503u); p[0] = v >> 8; p[1] = v >> 16; p[2] = v >> 24; *nb = 3; }
}

static int mkpng(const char *t, int w, int h, const char *out)
{
	int ct = !strcmp(t, "rgba") ? 6 : !strcmp(t, "pal") ? 3 : !strcmp(t, "grey") ? 0 : 2;
	int bpp = ct == 6 ? 4 : ct == 2 ? 3 : 1, x, y, nb;
	unsigned char ihdr[13], *raw, *z, p[4];
	size_t rawn = (size_t)(w * bpp + 1) * h, zn, off = 0, o = 0, blocks;
	uint32_t a = 1, b = 0, i;
	FILE *f = fopen(out, "wb");
	if (!f) return 1;
	crc_init();
	fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
	be32(ihdr, w); be32(ihdr + 4, h); ihdr[8] = 8; ihdr[9] = ct; ihdr[10] = ihdr[11] = ihdr[12] = 0;
	chunk(f, "IHDR", ihdr, 13);
	if (!strcmp(t, "huge")) { chunk(f, "IEND", NULL, 0); fclose(f); return 0; }
	if (ct == 3) {
		static const unsigned char pl[] = { 255,0,0, 0,255,0, 0,0,255, 255,255,255 };
		chunk(f, "PLTE", pl, 12);
	}
	raw = malloc(rawn);
	for (y = 0; y < h; y++) {
		raw[off++] = 0;
		for (x = 0; x < w; x++) { pix(t, x, y, w, p, &nb); memcpy(raw + off, p, nb); off += nb; }
	}
	blocks = rawn / 65535 + 1;
	zn = 2 + rawn + blocks * 5 + 4;
	z = malloc(zn);
	z[o++] = 0x78; z[o++] = 0x01;
	for (i = 0; i < rawn; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
	for (off = 0; off < rawn || off == 0;) {
		size_t n = rawn - off > 65535 ? 65535 : rawn - off;
		z[o++] = (off + n >= rawn); z[o++] = n & 255; z[o++] = n >> 8;
		z[o++] = ~n & 255; z[o++] = (~n >> 8) & 255;
		memcpy(z + o, raw + off, n); o += n; off += n;
		if (!n) break;
	}
	be32(z + o, (b << 16) | a); o += 4;
	chunk(f, "IDAT", z, (uint32_t)o);
	chunk(f, "IEND", NULL, 0);
	fclose(f); free(raw); free(z);
	return 0;
}

static int chkobc(const char *fn, int px, int py, int dopx)
{
	unsigned char h[16];
	long sz;
	int w, hh, k;
	FILE *f = fopen(fn, "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", fn); return 1; }
	if (fread(h, 1, 16, f) != 16 || memcmp(h, "OBC1", 4)) { fprintf(stderr, "bad magic\n"); return 1; }
	for (k = 8; k < 16; k++) if (h[k]) { fprintf(stderr, "reserved not zero\n"); return 1; }
	w = h[4] | h[5] << 8; hh = h[6] | h[7] << 8;
	fseek(f, 0, SEEK_END); sz = ftell(f);
	if (sz != 16 + (long)w * hh * 2) { fprintf(stderr, "bad size %ld\n", sz); return 1; }
	printf("%d %d\n", w, hh);
	if (dopx) {
		unsigned char v[2];
		fseek(f, 16 + ((long)py * w + px) * 2, SEEK_SET);
		if (fread(v, 1, 2, f) != 2) return 1;
		printf("PIXEL %04x\n", v[0] | v[1] << 8);
	}
	fclose(f);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 6 && !strcmp(argv[1], "png")) return mkpng(argv[2], atoi(argv[3]), atoi(argv[4]), argv[5]);
	if (argc >= 3 && !strcmp(argv[1], "obc")) return chkobc(argv[2], argc > 4 ? atoi(argv[3]) : 0, argc > 4 ? atoi(argv[4]) : 0, argc > 4);
	fprintf(stderr, "usage\n");
	return 2;
}
