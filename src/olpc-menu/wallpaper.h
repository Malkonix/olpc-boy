/* Procedural launcher wallpaper ("retro handheld night"): indigo-to-teal gradient with ordered dither,
 * a faint pixel grid, low-contrast pixel-art glyphs on a jittered lattice and a low mountain skyline.
 * Rendered once into a RAM buffer; screens restore regions from it with wp_blit() instead of a flat fill. */
#ifndef OLPC_WALLPAPER_H
#define OLPC_WALLPAPER_H
#include <stdint.h>

/* w*h RGB565 pixels, integer code only; skyline_y = baseline of the mountains (just above the hint bar) */
void wp_render(uint16_t *dst, int w, int h, int skyline_y);
/* copy a w x h rectangle at (x, y) from wallpaper wp to dst (both stride in pixels); dim = darken to half
 * (calm panel under list text) */
void wp_blit(uint16_t *dst, int dstride, const uint16_t *wp, int wstride, int x, int y, int w, int h, int dim);
#endif
