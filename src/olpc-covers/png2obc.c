/*
 * png2obc -- convert a PNG (or JPEG) cover into the launcher's .obc format.
 *
 *   png2obc in.png out.obc [maxw maxh]       (default box 480x480)
 *
 * .obc: 16-byte header ("OBC1", u16le width, u16le height, 8 zero bytes) then
 * width*height RGB565 little-endian pixels, top row first.
 *
 * The image is composited over a dark neutral background, scaled down (never
 * up) to fit the box with an exact area (box) filter, and written atomically
 * (temp file in the destination directory + rename).
 *
 * Exit status: 0 ok, 1 usage, 2 not decodable, 3 too large, 4 I/O or memory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define STBI_NO_SIMD            /* no SSE2 paths: the Geode LX has no SSE */
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_MAX_DIMENSIONS (1 << 16)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define MAX_SIDE   4096
#define MAX_PIXELS (8u * 1000u * 1000u)   /* RGBA decode = 4 bytes/pixel */
#define BG_R 24
#define BG_G 24
#define BG_B 24

static int die(int code, const char *m1, const char *m2)
{
	fprintf(stderr, "png2obc: %s%s%s\n", m1, m2 ? ": " : "", m2 ? m2 : "");
	return code;
}

static unsigned short rgb565(int r, int g, int b)
{
	return (unsigned short)((((r * 31 + 127) / 255) << 11) |
				(((g * 63 + 127) / 255) << 5) |
				((b * 31 + 127) / 255));
}

/* Fits (sw,sh) in (mw,mh), keeping aspect, never enlarging. */
static void fit(int sw, int sh, int mw, int mh, int *dw, int *dh)
{
	if (sw <= mw && sh <= mh) { *dw = sw; *dh = sh; return; }
	if ((long long)sw * mh > (long long)sh * mw) {
		*dw = mw;
		*dh = (int)(((long long)sh * mw + sw / 2) / sw);
	} else {
		*dh = mh;
		*dw = (int)(((long long)sw * mh + sh / 2) / sh);
	}
	if (*dw < 1) *dw = 1;
	if (*dh < 1) *dh = 1;
}

/* Composite one RGBA pixel over the background: out[0..2] = r,g,b. */
static void comp(const unsigned char *p, float *o)
{
	int a = p[3];
	o[0] = (float)((p[0] * a + BG_R * (255 - a)) / 255.0);
	o[1] = (float)((p[1] * a + BG_G * (255 - a)) / 255.0);
	o[2] = (float)((p[2] * a + BG_B * (255 - a)) / 255.0);
}

/*
 * Exact area-average scaling.  In integer units where the source row is sw
 * long and the destination dw long, source pixel i covers [i*dw,(i+1)*dw) and
 * destination pixel o covers [o*sw,(o+1)*sw); the weight is the overlap.
 * Rows are scaled horizontally one at a time and accumulated vertically, so
 * memory is one accumulator row plus one scratch row.
 */
static int scale(const unsigned char *src, int sw, int sh, unsigned short *dst,
		 int dw, int dh)
{
	float *acc = malloc((size_t)dw * 3 * sizeof *acc);
	float *row = malloc((size_t)dw * 3 * sizeof *row);
	int oy, x, i;
	int cached = -1;

	if (!acc || !row) { free(acc); free(row); return -1; }

	for (oy = 0; oy < dh; oy++) {
		long long y0 = (long long)oy * sh, y1 = y0 + sh; /* /dh units */
		int sy0 = (int)(y0 / dh), sy1 = (int)((y1 - 1) / dh);
		int sy;

		memset(acc, 0, (size_t)dw * 3 * sizeof *acc);
		for (sy = sy0; sy <= sy1 && sy < sh; sy++) {
			long long a = (long long)sy * dh, b = a + dh;
			long long lo = a > y0 ? a : y0, hi = b < y1 ? b : y1;
			float wy = (float)(hi - lo) / (float)sh;
			const unsigned char *s = src + (size_t)sy * sw * 4;

			if (sy != cached) {
				for (x = 0; x < dw; x++) {
					long long x0 = (long long)x * sw, x1 = x0 + sw;
					int sx0 = (int)(x0 / dw), sx1 = (int)((x1 - 1) / dw);
					float sum[3] = { 0, 0, 0 }, px[3];
					for (i = sx0; i <= sx1 && i < sw; i++) {
						long long c = (long long)i * dw, d = c + dw;
						long long l2 = c > x0 ? c : x0, h2 = d < x1 ? d : x1;
						float wx = (float)(h2 - l2) / (float)sw;
						comp(s + (size_t)i * 4, px);
						sum[0] += px[0] * wx;
						sum[1] += px[1] * wx;
						sum[2] += px[2] * wx;
					}
					row[x * 3] = sum[0];
					row[x * 3 + 1] = sum[1];
					row[x * 3 + 2] = sum[2];
				}
				cached = sy;
			}
			for (x = 0; x < dw * 3; x++)
				acc[x] += row[x] * wy;
		}
		for (x = 0; x < dw; x++) {
			int r = (int)(acc[x * 3] + 0.5f), g = (int)(acc[x * 3 + 1] + 0.5f),
			    b = (int)(acc[x * 3 + 2] + 0.5f);
			if (r > 255) r = 255;
			if (g > 255) g = 255;
			if (b > 255) b = 255;
			dst[(size_t)oy * dw + x] = rgb565(r, g, b);
		}
	}
	free(acc);
	free(row);
	return 0;
}

/* Write header + pixels to a temp file next to out, then rename. */
static int write_obc(const char *out, const unsigned short *px, int w, int h)
{
	char tmp[4096], hdr[16] = { 'O', 'B', 'C', '1' };
	const char *slash = strrchr(out, '/');
	size_t n = (size_t)w * h, i;
	unsigned char *buf;
	FILE *f;
	int len;

	if (slash)
		len = snprintf(tmp, sizeof tmp, "%.*s/.%s.tmp.%d", (int)(slash - out),
			       out, slash + 1, (int)getpid());
	else
		len = snprintf(tmp, sizeof tmp, ".%s.tmp.%d", out, (int)getpid());
	if (len <= 0 || len >= (int)sizeof tmp) return -1;

	hdr[4] = (char)(w & 255); hdr[5] = (char)(w >> 8);
	hdr[6] = (char)(h & 255); hdr[7] = (char)(h >> 8);
	buf = malloc(n * 2);
	if (!buf) return -1;
	for (i = 0; i < n; i++) {
		buf[i * 2] = (unsigned char)(px[i] & 255);
		buf[i * 2 + 1] = (unsigned char)(px[i] >> 8);
	}
	f = fopen(tmp, "wb");
	if (!f) { free(buf); return -1; }
	if (fwrite(hdr, 1, 16, f) != 16 || fwrite(buf, 2, n, f) != n ||
	    fflush(f) != 0 || fsync(fileno(f)) != 0) {
		fclose(f); unlink(tmp); free(buf); return -1;
	}
	free(buf);
	if (fclose(f) != 0) { unlink(tmp); return -1; }
	if (rename(tmp, out) != 0) { unlink(tmp); return -1; }
	return 0;
}

/* stb_info also fails on absurd PNG sizes: report those as "too large". */
static int png_oversized(const char *fn)
{
	unsigned char h[24];
	unsigned long w, ht;
	FILE *f = fopen(fn, "rb");
	int n;

	if (!f) return 0;
	n = (int)fread(h, 1, 24, f);
	fclose(f);
	if (n != 24 || memcmp(h, "\x89PNG", 4)) return 0;
	w = (unsigned long)h[16] << 24 | h[17] << 16 | h[18] << 8 | h[19];
	ht = (unsigned long)h[20] << 24 | h[21] << 16 | h[22] << 8 | h[23];
	return w > MAX_SIDE || ht > MAX_SIDE;
}

int main(int argc, char **argv)
{
	int w, h, comp_n, mw = 480, mh = 480, dw, dh;
	unsigned char *img;
	unsigned short *out;

	if (argc != 3 && argc != 5)
		return die(1, "usage: png2obc in.png out.obc [maxw maxh]", NULL);
	if (argc == 5) {
		mw = atoi(argv[3]); mh = atoi(argv[4]);
		if (mw < 1 || mh < 1 || mw > 65535 || mh > 65535)
			return die(1, "bad maxw/maxh", NULL);
	}
	if (!stbi_info(argv[1], &w, &h, &comp_n)) {
		return die(png_oversized(argv[1]) ? 3 : 2, argv[1], stbi_failure_reason());
	}
	if (w < 1 || h < 1 || w > MAX_SIDE || h > MAX_SIDE ||
	    (unsigned long long)w * h > MAX_PIXELS)
		return die(3, "image too large", argv[1]);

	img = stbi_load(argv[1], &w, &h, &comp_n, 4);
	if (!img)
		return die(2, argv[1], stbi_failure_reason());

	fit(w, h, mw, mh, &dw, &dh);
	out = malloc((size_t)dw * dh * sizeof *out);
	if (!out) { stbi_image_free(img); return die(4, "out of memory", NULL); }
	if (scale(img, w, h, out, dw, dh) != 0) {
		stbi_image_free(img); free(out);
		return die(4, "out of memory", NULL);
	}
	stbi_image_free(img);
	if (write_obc(argv[2], out, dw, dh) != 0) {
		int e = errno;
		free(out);
		return die(4, argv[2], strerror(e));
	}
	free(out);
	return 0;
}
