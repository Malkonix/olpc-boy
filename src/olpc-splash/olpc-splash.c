/* olpc-splash: boot splash for the OLPC XO-1 (1200x900 RGB565 /dev/fb0), static, no dependencies beyond
 * ../common.  Usage:
 *   olpc-splash init [PCT TEXT]   console to KD_GRAPHICS, draw the whole screen, mark the splash active
 *   olpc-splash progress PCT TEXT redraw only the bar + status text (pwrite of a small band)
 *   olpc-splash done              forget the splash (the launcher takes the screen over by itself)
 *   olpc-splash text              back to the text console (KD_TEXT, fbcon repainted): for failures
 * Does nothing (exit 0) when /dev/fb0 is not a 16 bpp panel, or olpcboy.verbose is on the kernel command
 * line.  progress is a no-op unless init marked the splash active.  Never touches DCON mode or brightness.
 * Test hooks: OLPC_SPLASH_FB (fake fb file, 1200x900), OLPC_SPLASH_RUN (marker dir), OLPC_SPLASH_CMDLINE. */
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "../common/gfx.h"

#define C(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define BG     0xFFFF
#define INK    C(0x20, 0x20, 0x20)
#define SHADOW C(0xB0, 0xB0, 0xB0)
#define DIM    C(0x68, 0x68, 0x68)
#define ACCENT C(0x2E, 0x6B, 0x2E)        /* dark green: luma ~ dark grey in DCON mono */

#define BAR_W  480
#define BAR_H  14
/* the band repainted by progress: bar + one text line, relative to the screen centre column */
#define REG_W  800
#define REG_H  96
#define REG_Y  590

static int W = 1200, H = 900;
static size_t line;                        /* bytes per fb row */
static int fd = -1, tty = -1, fake;
static const char *rundir;

static void kd(int mode)
{
	if (!fake && tty >= 0) ioctl(tty, KDSETMODE, mode);
}

/* the dynamic part, drawn into a REG_W x REG_H surface: bar at the top, status text under it */
static void dyn(const struct gfx *g, int pct, const char *text)
{
	int bx = (REG_W - BAR_W) / 2, fill;

	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	fill = (BAR_W - 4) * pct / 100;
	gfx_fill(g, 0, 0, REG_W, REG_H, BG);
	gfx_fill(g, bx, 10, BAR_W, BAR_H, INK);
	gfx_fill(g, bx + 2, 12, BAR_W - 4, BAR_H - 4, BG);
	gfx_fill(g, bx + 2, 12, fill, BAR_H - 4, ACCENT);
	gfx_text(g, (REG_W - gfx_text_w((const unsigned char *)text, 2)) / 2, 44, 2, DIM, (const unsigned char *)text);
}

static void put(const uint16_t *src, int stride, int x, int y, int w, int h)
{
	for (int r = 0; r < h; r++)
		if (pwrite(fd, src + (size_t)r * stride, (size_t)w * 2, (off_t)((size_t)(y + r) * line + (size_t)x * 2)) < 0) return;
}

static void draw_all(int pct, const char *text)
{
	uint16_t *buf = malloc((size_t)W * H * 2);
	struct gfx g = { buf, W, W, H }, r;
	const unsigned char *title = (const unsigned char *)"OLPC-BOY", *sub = (const unsigned char *)"GB \xb7 GBC \xb7 NES";
	int cx = W / 2, tw, tx;

	if (!buf) return;
	gfx_fill(&g, 0, 0, W, H, BG);
	tw = gfx_text_w(title, 10);
	tx = cx - tw / 2;
	gfx_text(&g, tx + 6, 236, 10, SHADOW, title);
	gfx_text(&g, tx, 230, 10, INK, title);
	gfx_fill(&g, tx, 410, tw, 4, INK);
	gfx_text(&g, cx - gfx_text_w(sub, 3) / 2, 440, 3, DIM, sub);
	r.px = buf + (size_t)REG_Y * W + (cx - REG_W / 2); r.stride = W; r.w = REG_W; r.h = REG_H;
	dyn(&r, pct, text);
	put(buf, W, 0, 0, W, H);
	free(buf);
}

static void marker(int on)
{
	char p[256];

	snprintf(p, sizeof p, "%s/splash", rundir);
	if (on) { int f = open(p, O_WRONLY | O_CREAT, 0644); if (f >= 0) close(f); }
	else unlink(p);
}

static int verbose(void)
{
	const char *p = getenv("OLPC_SPLASH_CMDLINE");
	char buf[4096] = "";
	FILE *f = fopen(p ? p : "/proc/cmdline", "r");

	if (f) { if (!fgets(buf, sizeof buf, f)) buf[0] = 0; fclose(f); }
	return strstr(buf, "olpcboy.verbose") != NULL;
}

/* 1 = usable framebuffer */
static int fb_open(void)
{
	const char *ff = getenv("OLPC_SPLASH_FB");

	if (ff && *ff) {
		fake = 1;
		fd = open(ff, O_RDWR | O_CREAT, 0644);
		line = (size_t)W * 2;
		return fd >= 0 && ftruncate(fd, (off_t)(line * H)) == 0;
	}
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;

	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &f) < 0) return 0;
	if (v.bits_per_pixel != 16 || v.xres < 800 || v.yres < 600) return 0;
	W = (int)v.xres; H = (int)v.yres; line = f.line_length;
	tty = open("/dev/tty0", O_RDWR | O_NOCTTY);
	return 1;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "";
	int pct = argc > 2 ? atoi(argv[2]) : 0;
	const char *text = argc > 3 ? argv[3] : "";
	char p[256];

	rundir = getenv("OLPC_SPLASH_RUN") ? getenv("OLPC_SPLASH_RUN") : "/run/olpcboy";
	if (!strcmp(cmd, "done")) { marker(0); return 0; }
	if (!strcmp(cmd, "init")) {
		if (argc == 3) { text = argv[2]; pct = 0; }
		if (verbose() || !fb_open()) return 0;
		kd(KD_GRAPHICS);
		draw_all(pct, text);
		marker(1);
		return 0;
	}
	if (!strcmp(cmd, "progress")) {
		uint16_t band[REG_W * REG_H];
		struct gfx g = { band, REG_W, REG_W, REG_H };

		snprintf(p, sizeof p, "%s/splash", rundir);
		if (access(p, F_OK) != 0 || !fb_open()) return 0;
		dyn(&g, pct, text);
		put(band, REG_W, W / 2 - REG_W / 2, REG_Y, REG_W, REG_H);
		return 0;
	}
	if (!strcmp(cmd, "text")) {
		struct vt_stat st;

		marker(0);
		if (!fb_open() || fake || tty < 0) return 0;
		kd(KD_TEXT);
		/* bounce through another VT so fbcon repaints the text over the splash */
		if (ioctl(tty, VT_GETSTATE, &st) == 0 && st.v_active >= 1 && st.v_active < 63) {
			int cur = st.v_active, other = cur == 1 ? 2 : 1;
			if (ioctl(tty, VT_ACTIVATE, other) == 0) ioctl(tty, VT_WAITACTIVE, other);
			if (ioctl(tty, VT_ACTIVATE, cur) == 0) ioctl(tty, VT_WAITACTIVE, cur);
		}
		return 0;
	}
	fprintf(stderr, "usage: olpc-splash init [PCT TEXT] | progress PCT TEXT | done | text\n");
	return 2;
}
