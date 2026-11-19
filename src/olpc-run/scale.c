#include <stdlib.h>
#include <string.h>
#include "scale.h"

void pixtab_init(struct pixtab *t, int bpp, int ro, int rl, int go, int gl, int bo, int bl)
{
	int i;

	memset(t, 0, sizeof *t);
	t->B = bpp / 8;
	for (i = 0; i < 32; i++) {
		unsigned r8 = (i << 3) | (i >> 2);
		t->r[i] = (uint32_t)(r8 >> (8 - rl)) << ro;
		t->b[i] = (uint32_t)(r8 >> (8 - bl)) << bo;
	}
	for (i = 0; i < 64; i++) {
		unsigned g8 = (i << 2) | (i >> 4);
		t->g[i] = (uint32_t)(g8 >> (8 - gl)) << go;
	}
	t->ident16 = bpp == 16 && ro == 11 && rl == 5 && go == 5 && gl == 6 && bo == 0 && bl == 5;
}

static inline uint32_t conv_px(const struct pixtab *t, uint16_t v)
{
	return t->r[v >> 11] + t->g[(v >> 5) & 63] + t->b[v & 31];
}

static inline uint16_t px565(int fmt, const uint8_t *p, int i)
{
	if (fmt == SRC_XRGB8888) {
		uint32_t v = ((const uint32_t *)p)[i];
		return (uint16_t)(((v >> 8) & 0xf800) | ((v >> 5) & 0x07e0) | ((v >> 3) & 0x1f));
	}
	if (fmt == SRC_0RGB1555) {
		uint16_t v = ((const uint16_t *)p)[i];
		uint16_t g5 = (v >> 5) & 31;
		return (uint16_t)(((v & 0x7c00) << 1) | (((g5 << 1) | (g5 >> 4)) << 5) | (v & 31));
	}
	return ((const uint16_t *)p)[i];
}

static void src_to_565(int fmt, const uint8_t *row, int w, uint8_t *out)
{
	int i;

	if (fmt == SRC_RGB565) { memcpy(out, row, (size_t)w * 2); return; }
	for (i = 0; i < w; i++) ((uint16_t *)out)[i] = px565(fmt, row, i);
}

/* RGB565 row -> destination-format row */
static void conv_row(const struct pixtab *t, const uint8_t *s565, int w, uint8_t *out)
{
	const uint16_t *s = (const uint16_t *)s565;
	int i;

	if (t->ident16) { memcpy(out, s565, (size_t)w * 2); return; }
	if (t->B == 2) for (i = 0; i < w; i++) ((uint16_t *)out)[i] = (uint16_t)conv_px(t, s[i]);
	else for (i = 0; i < w; i++) ((uint32_t *)out)[i] = conv_px(t, s[i]);
}

/* Each pixel of crow (w pixels, B bytes) k times. */
static void expand_c(const uint8_t *crow, int w, int B, int k, uint8_t *out)
{
	int i, j;

	if (B == 2) {
		uint16_t *o = (uint16_t *)out;
		for (i = 0; i < w; i++) for (j = 0; j < k; j++) *o++ = ((const uint16_t *)crow)[i];
	} else {
		uint32_t *o = (uint32_t *)out;
		for (i = 0; i < w; i++) for (j = 0; j < k; j++) *o++ = ((const uint32_t *)crow)[i];
	}
}

int scale_pick(int sw, int sh, int dw, int dh)
{
	int k = dw / sw < dh / sh ? dw / sw : dh / sh;

	return k < 1 ? 1 : k;
}

int blit_alloc(struct blit *b, int sw, int sh, int k, int srcfmt, const struct pixtab *t, size_t stride)
{
	b->sw = sw; b->sh = sh; b->k = k; b->srcfmt = srcfmt; b->t = t; b->stride = stride;
	b->c565 = malloc((size_t)sw * 2 + 16);
	b->crow = malloc((size_t)sw * 4 + 16);
	b->rowbuf = malloc((size_t)sw * k * 4 + 16);
	return b->c565 && b->crow && b->rowbuf ? 0 : -1;
}

void blit_free(struct blit *b)
{
	free(b->c565); free(b->crow); free(b->rowbuf);
	b->c565 = b->crow = b->rowbuf = NULL;
}

static size_t src_bytes(int fmt) { return fmt == SRC_XRGB8888 ? 4 : 2; }

int blit_rows(const struct blit *b, const void *src, size_t pitch, uint8_t *prev, int full, uint8_t *dst)
{
	int B = b->t->B, k = b->k, y, j, rows = 0;
	size_t rb = (size_t)b->sw * k * B, sb = (size_t)b->sw * src_bytes(b->srcfmt);

	for (y = 0; y < b->sh; y++) {
		const uint8_t *sr = (const uint8_t *)src + (size_t)y * pitch;
		uint8_t *pr = prev ? prev + (size_t)y * pitch : NULL;

		if (pr) {
			if (!full && !memcmp(sr, pr, sb)) continue;
			memcpy(pr, sr, sb);
		}
		src_to_565(b->srcfmt, sr, b->sw, b->c565);
		conv_row(b->t, b->c565, b->sw, b->crow);
		expand_c(b->crow, b->sw, B, k, b->rowbuf);
		for (j = 0; j < k; j++)
			memcpy(dst + (size_t)(y * k + j) * b->stride, b->rowbuf, rb);
		rows++;
	}
	return rows;
}

void blit_naive(const struct blit *b, const void *src, size_t pitch, uint8_t *dst)
{
	int dw = b->sw * b->k, dh = b->sh * b->k, B = b->t->B, x, y;

	for (y = 0; y < dh; y++) {
		int sy = y * b->sh / dh;
		uint8_t *row = dst + (size_t)y * b->stride;
		for (x = 0; x < dw; x++) {
			int sx = x * b->sw / dw;
			uint32_t v = conv_px(b->t, px565(b->srcfmt, (const uint8_t *)src + (size_t)sy * pitch, sx));
			if (B == 2) *(uint16_t *)(row + (size_t)x * 2) = (uint16_t)v;
			else *(uint32_t *)(row + (size_t)x * 4) = v;
		}
	}
}
