/* Integer scaler: source frame (RGB565 / 0RGB1555 / XRGB8888) -> framebuffer.
 * Method measured on the XO-1: expand each source row once, memcpy it for the
 * repeated lines, skip source rows that did not change since the last draw. */
#ifndef OLPC_RUN_SCALE_H
#define OLPC_RUN_SCALE_H
#include <stddef.h>
#include <stdint.h>

/* Same numbering as RETRO_PIXEL_FORMAT_* */
#define SRC_0RGB1555 0
#define SRC_XRGB8888 1
#define SRC_RGB565   2

/* Destination pixel = r[v>>11] + g[(v>>5)&63] + b[v&31] for an RGB565 value v. */
struct pixtab {
	int B;            /* destination bytes per pixel: 2 or 4 */
	int ident16;      /* destination is RGB565 */
	uint32_t r[32], g[64], b[32];
};

void pixtab_init(struct pixtab *t, int bpp, int ro, int rl, int go, int gl, int bo, int bl);

struct blit {
	int sw, sh, k;             /* source size and integer scale */
	int srcfmt;
	const struct pixtab *t;
	size_t stride;             /* destination line length in bytes */
	uint8_t *c565;             /* source row as RGB565 (sw*2) */
	uint8_t *crow;             /* row in destination format (sw*B) */
	uint8_t *rowbuf;           /* expanded row (sw*k*B) */
};

/* Largest integer k with sw*k <= dw and sh*k <= dh; at least 1. */
int scale_pick(int sw, int sh, int dw, int dh);

int  blit_alloc(struct blit *b, int sw, int sh, int k, int srcfmt, const struct pixtab *t, size_t stride);
void blit_free(struct blit *b);

/* Draw src (pitch bytes per row) at dst (top-left of the target rectangle).
 * prev is a copy of the last frame that reached the screen, same pitch; rows of
 * src equal to prev are skipped and changed rows are copied into prev.  With
 * full != 0 every row is drawn.  Returns the number of source rows drawn. */
int blit_rows(const struct blit *b, const void *src, size_t pitch, uint8_t *prev, int full, uint8_t *dst);

/* Reference implementation (per-pixel, slow) for the self-test. */
void blit_naive(const struct blit *b, const void *src, size_t pitch, uint8_t *dst);
#endif
