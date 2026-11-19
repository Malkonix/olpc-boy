/* olpc-menu: game launcher for the OLPC XO-1.  Draws straight to /dev/fb0 (1200x900 RGB565), reads
 * the built-in buttons and USB pads through the olpc-run input code, starts olpc-run for the chosen
 * game.  Sleeps in ppoll() when nothing changes.
 *
 * Test hooks (all optional): OLPC_MENU_FAKEFB (file standing in for /dev/fb0), OLPC_MENU_SCAN (base
 * dirs, default /mnt:/media), OLPC_MENU_RUNDIR (default /run/olpcboy), OLPC_MENU_CHORD, OLPC_MENU_RUN,
 * OLPC_MENU_COVERS_BIN, OLPC_MENU_WIFI_BIN, OLPC_MENU_SDMOUNT, OLPC_MENU_POWEROFF, OLPC_MENU_REBOOT,
 * OLPC_MENU_INPUTDIR, OLPC_MENU_DEVDIR and --script FILE (timed key events, see test.sh).
 * Exit codes: 0 ok, 2 usage, 3 no usable framebuffer. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "menulib.h"
#include "font8x16.h"
#include "../olpc-run/input.h"

/* ------------------------------------------------------------------ config */

static const char *e_scan, *e_rundir, *e_chord, *e_run, *e_covers, *e_wifi, *e_sd, *e_poweroff, *e_reboot,
		  *e_inputdir, *e_devdir;
static char p_state[300], p_runerr[300], p_covstat[300], p_jobout[300];

static const char *envd(const char *n, const char *d) { const char *v = getenv(n); return v && *v ? v : d; }

#define J_B 0
#define J_SELECT 2
#define J_START 3
#define J_UP 4
#define J_DOWN 5
#define J_LEFT 6
#define J_RIGHT 7
#define J_A 8
#define BIT(n) ((uint16_t)(1u << (n)))
#define DIRS (BIT(J_UP) | BIT(J_DOWN) | BIT(J_LEFT) | BIT(J_RIGHT))
#define BTNS (BIT(J_B) | BIT(J_A) | BIT(J_START) | BIT(J_SELECT))

#define COVER_REST_MS 150

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
	va_list ap;
	fputs("olpc-menu: ", stderr);
	va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
	fputc('\n', stderr);
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------ framebuffer */

static int FBW = 1200, FBH = 900;
static uint16_t *bb;                       /* back buffer: what the screen shows; never read the fb */
static uint8_t *fbmap;
static size_t fbstride, fbmaplen;
static int fbfd = -1, g_fake, g_tty = -1;
static volatile sig_atomic_t g_kd_graphics;
static int g_kd_saved = KD_TEXT;

static void console_text(void)
{
	if (g_kd_graphics && g_tty >= 0) { ioctl(g_tty, KDSETMODE, g_kd_saved); g_kd_graphics = 0; }
}

/* back to KD_TEXT, then bounce through another VT so fbcon repaints (same as olpc-run) */
static void console_redraw(void)
{
	struct vt_stat st;

	console_text();
	if (g_tty >= 0 && ioctl(g_tty, VT_GETSTATE, &st) == 0 && st.v_active >= 1 && st.v_active < 63) {
		int cur = st.v_active, other = cur == 1 ? 2 : 1;
		if (ioctl(g_tty, VT_ACTIVATE, other) == 0) ioctl(g_tty, VT_WAITACTIVE, other);
		if (ioctl(g_tty, VT_ACTIVATE, cur) == 0) ioctl(g_tty, VT_WAITACTIVE, cur);
	}
}

/* 0 = ok, -1 = no usable framebuffer */
static int fb_open(void)
{
	const char *fake = getenv("OLPC_MENU_FAKEFB");

	if (fake && *fake) {
		struct stat st;
		fbfd = open(fake, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
		if (fbfd < 0) { logf_("fake fb %s: %s", fake, strerror(errno)); return -1; }
		fbmaplen = (size_t)FBW * FBH * 2;
		if (fstat(fbfd, &st) == 0 && (size_t)st.st_size < fbmaplen && ftruncate(fbfd, (off_t)fbmaplen) < 0) return -1;
		fbstride = (size_t)FBW * 2;
		g_fake = 1;
	} else {
		struct fb_var_screeninfo v;
		struct fb_fix_screeninfo f;
		char name[64] = "";
		FILE *nf = fopen("/sys/class/graphics/fb0/name", "r");

		if (nf) { if (!fgets(name, sizeof name, nf)) name[0] = 0; fclose(nf); }
		name[strcspn(name, "\n")] = 0;
		if (strcmp(name, "Geode LX") && !getenv("OLPC_MENU_ANYFB")) {
			logf_("no Geode LX framebuffer (fb0 name \"%s\"): nothing to draw on", name);
			return -1;
		}
		fbfd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
		if (fbfd < 0) { logf_("/dev/fb0: %s", strerror(errno)); return -1; }
		if (ioctl(fbfd, FBIOGET_VSCREENINFO, &v) < 0 || ioctl(fbfd, FBIOGET_FSCREENINFO, &f) < 0 ||
		    v.bits_per_pixel != 16 || v.xres < 800 || v.yres < 600) {
			logf_("unsupported framebuffer (%ux%u %u bpp)", v.xres, v.yres, v.bits_per_pixel);
			close(fbfd); fbfd = -1;
			return -1;
		}
		FBW = (int)v.xres; FBH = (int)v.yres;
		fbstride = f.line_length; fbmaplen = f.smem_len;
		logf_("/dev/fb0 %dx%d 16 bpp stride %zu", FBW, FBH, fbstride);
	}
	fbmap = mmap(NULL, fbmaplen, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (fbmap == MAP_FAILED) { logf_("mmap: %s", strerror(errno)); fbmap = NULL; close(fbfd); fbfd = -1; return -1; }
	bb = calloc((size_t)FBW * FBH, 2);
	if (!bb) return -1;
	if (!g_fake) {
		g_tty = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
		if (g_tty < 0) g_tty = open("/dev/tty1", O_RDWR | O_NOCTTY | O_CLOEXEC);
		if (g_tty >= 0) {
			if (ioctl(g_tty, KDGETMODE, &g_kd_saved) < 0) g_kd_saved = KD_TEXT;
			if (ioctl(g_tty, KDSETMODE, KD_GRAPHICS) == 0) g_kd_graphics = 1;
		}
	}
	return 0;
}

static void fb_close(void)
{
	if (fbmap) {
		if (!g_fake) memset(fbmap, 0, fbstride * (size_t)FBH);
		munmap(fbmap, fbmaplen);
		fbmap = NULL;
	}
	if (!g_fake) console_redraw();
	if (fbfd >= 0) close(fbfd);
	fbfd = -1;
}

/* dirty rectangles: merge overlapping ones, collapse into one when the list is full */
struct rect { int x0, y0, x1, y1; };
static struct rect dirty[8];
static int ndirty;

static void dirty_add(int x, int y, int w, int h)
{
	struct rect r = { x, y, x + w, y + h };
	int i;

	if (r.x0 < 0) r.x0 = 0;
	if (r.y0 < 0) r.y0 = 0;
	if (r.x1 > FBW) r.x1 = FBW;
	if (r.y1 > FBH) r.y1 = FBH;
	if (r.x1 <= r.x0 || r.y1 <= r.y0) return;
	for (i = 0; i < ndirty; i++) {
		struct rect *d = &dirty[i];
		if (r.x0 <= d->x1 && r.x1 >= d->x0 && r.y0 <= d->y1 && r.y1 >= d->y0) {
			if (r.x0 < d->x0) d->x0 = r.x0;
			if (r.y0 < d->y0) d->y0 = r.y0;
			if (r.x1 > d->x1) d->x1 = r.x1;
			if (r.y1 > d->y1) d->y1 = r.y1;
			return;
		}
	}
	if (ndirty == 8) {
		for (i = 1; i < ndirty; i++) {
			if (dirty[i].x0 < dirty[0].x0) dirty[0].x0 = dirty[i].x0;
			if (dirty[i].y0 < dirty[0].y0) dirty[0].y0 = dirty[i].y0;
			if (dirty[i].x1 > dirty[0].x1) dirty[0].x1 = dirty[i].x1;
			if (dirty[i].y1 > dirty[0].y1) dirty[0].y1 = dirty[i].y1;
		}
		ndirty = 1;
	}
	dirty[ndirty++] = r;
}

static void flush(void)
{
	int i, y;

	for (i = 0; i < ndirty; i++) {
		const struct rect *d = &dirty[i];
		for (y = d->y0; y < d->y1; y++)
			memcpy(fbmap + (size_t)y * fbstride + (size_t)d->x0 * 2, bb + (size_t)y * FBW + d->x0,
			       (size_t)(d->x1 - d->x0) * 2);
	}
	ndirty = 0;
}

/* ----------------------------------------------------------------- drawing */

#define RGB(r, g, b) ((uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))
static const uint16_t C_BG = RGB(14, 18, 26), C_TEXT = RGB(235, 238, 242), C_DIM = RGB(150, 160, 178),
		      C_PANEL = RGB(26, 32, 44), C_BORDER = RGB(80, 92, 116), C_HINT = RGB(22, 26, 36),
		      C_OK = RGB(150, 230, 150), C_BOX = RGB(30, 38, 54);
/* per system: title bar, selection bar */
static const uint16_t C_TITLE[NSYS] = { RGB(52, 78, 30), RGB(66, 42, 112), RGB(122, 30, 30) };
static const uint16_t C_SEL[NSYS] = { RGB(88, 128, 44), RGB(110, 70, 180), RGB(190, 50, 50) };

static void fill(int x, int y, int w, int h, uint16_t c)
{
	int xx, yy, x1 = x + w, y1 = y + h;

	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x1 > FBW) x1 = FBW;
	if (y1 > FBH) y1 = FBH;
	if (x1 <= x || y1 <= y) return;
	for (yy = y; yy < y1; yy++) {
		uint16_t *p = bb + (size_t)yy * FBW;
		for (xx = x; xx < x1; xx++) p[xx] = c;
	}
	dirty_add(x, y, x1 - x, y1 - y);
}

static void frame(int x, int y, int w, int h, int t, uint16_t c)
{
	fill(x, y, w, t, c); fill(x, y + h - t, w, t, c);
	fill(x, y, t, h, c); fill(x + w - t, y, t, h, c);
}

static int text_w(const unsigned char *s, int sc) { return (int)strlen((const char *)s) * 8 * sc; }

/* Latin-1 text, glyph pixels only (the caller painted the background). */
static void text(int x, int y, int sc, uint16_t fg, const unsigned char *s)
{
	int x0 = x;

	for (; *s; s++, x += 8 * sc) {
		const unsigned char *g;
		int row, col, sx, sy;
		unsigned c = *s;

		if (c < 0x20) c = '?';
		g = font8x16[c - 0x20];
		if (x < 0 || x + 8 * sc > FBW || y < 0 || y + 16 * sc > FBH) continue;
		for (row = 0; row < 16; row++) {
			unsigned bits = g[row];
			if (!bits) continue;
			for (sy = 0; sy < sc; sy++) {
				uint16_t *d = bb + (size_t)(y + row * sc + sy) * FBW + x;
				for (col = 0; col < 8; col++)
					if (bits & (0x80u >> col))
						for (sx = 0; sx < sc; sx++) d[col * sc + sx] = fg;
			}
		}
	}
	dirty_add(x0, y, x - x0, 16 * sc);
}

/* UTF-8 in, fitted to maxch characters, drawn; returns the pixel width */
static int utext(int x, int y, int sc, uint16_t fg, const char *s, int maxch)
{
	unsigned char l1[512];

	utf8_to_l1(s, l1, sizeof l1);
	if (maxch > 0) fit_text(l1, maxch);
	text(x, y, sc, fg, l1);
	return text_w(l1, sc);
}

static void ctext(int cx, int y, int sc, uint16_t fg, const char *s)   /* centred on cx */
{
	unsigned char l1[512];

	utf8_to_l1(s, l1, sizeof l1);
	text(cx - text_w(l1, sc) / 2, y, sc, fg, l1);
}

/* ------------------------------------------------------------------- state */

#define TITLE_H 72
#define HINT_H 56
#define ROW_H 44
#define LIST_X 12
#define LIST_W 664
#define SB_X (LIST_X + LIST_W + 4)
#define PANEL 480

static struct romlist L[NSYS];
static int total_roms, cur_sys, sel[NSYS], topi[NSYS];
static enum { SC_LIST, SC_OPTIONS, SC_ERROR } screen;
static int list_rows, list_top_y, cov_x, cov_y;

static uint16_t covbuf[COVER_MAX * COVER_MAX];
static int cov_w, cov_h;
static enum { CV_BLANK, CV_PENDING, CV_SHOWN, CV_PLACEHOLDER } cov_state = CV_BLANK;
static uint64_t cov_due;

static volatile sig_atomic_t g_quit, g_chld;
static const int exit_code = 0;

static void layout(void)
{
	list_top_y = TITLE_H + 12;
	list_rows = (FBH - HINT_H - 12 - list_top_y) / ROW_H;
	cov_x = FBW - PANEL - 24;
	cov_y = TITLE_H + 24;
}

static struct rom *cur_rom(void)
{
	struct romlist *l = &L[cur_sys];
	return l->n ? &l->v[sel[cur_sys]] : NULL;
}

static void draw_title(void)
{
	char b[64];
	unsigned char l1[96];

	fill(0, 0, FBW, TITLE_H, C_TITLE[cur_sys]);
	snprintf(b, sizeof b, "< %s >", SYS_TITLE[cur_sys]);
	text(24, 12, 3, C_TEXT, (const unsigned char *)b);
	snprintf(b, sizeof b, "%d game%s", L[cur_sys].n, L[cur_sys].n == 1 ? "" : "s");
	utf8_to_l1(b, l1, sizeof l1);
	text(FBW - 24 - text_w(l1, 3), 12, 3, C_TEXT, l1);
}

static void draw_hint(const char *s)
{
	fill(0, FBH - HINT_H, FBW, HINT_H, C_HINT);
	ctext(FBW / 2, FBH - HINT_H + 12, 2, C_DIM, s);
}

static void draw_row(int i)       /* i = row on screen */
{
	int idx = topi[cur_sys] + i, y = list_top_y + i * ROW_H;
	int isel = idx == sel[cur_sys];

	fill(LIST_X, y, LIST_W, ROW_H, isel ? C_SEL[cur_sys] : C_BG);
	if (idx < L[cur_sys].n)
		utext(LIST_X + 14, y + (ROW_H - 32) / 2, 2, C_TEXT, L[cur_sys].v[idx].name, (LIST_W - 28) / 16);
}

static void draw_scrollbar(void)
{
	int n = L[cur_sys].n, h = list_rows * ROW_H, th, ty;

	fill(SB_X, list_top_y, 8, h, C_BG);
	if (n <= list_rows) return;
	fill(SB_X, list_top_y, 8, h, C_PANEL);
	th = h * list_rows / n;
	if (th < 16) th = 16;
	ty = list_top_y + (h - th) * topi[cur_sys] / (n - list_rows);
	fill(SB_X, ty, 8, th, C_DIM);
}

static void draw_list_all(void)
{
	int i;

	fill(0, list_top_y - 12, SB_X + 10, FBH - HINT_H - list_top_y + 12, C_BG);
	if (!L[cur_sys].n) {
		char b[96];
		ctext((LIST_X + LIST_W) / 2, list_top_y + 80, 2, C_DIM, "No games here");
		snprintf(b, sizeof b, "for %s.", SYS_TITLE[cur_sys]);
		ctext((LIST_X + LIST_W) / 2, list_top_y + 120, 2, C_DIM, b);
		snprintf(b, sizeof b, "Put them in roms/%s", SYS_DIR[cur_sys]);
		ctext((LIST_X + LIST_W) / 2, list_top_y + 190, 2, C_DIM, b);
		return;
	}
	for (i = 0; i < list_rows; i++) draw_row(i);
	draw_scrollbar();
}

static void draw_cover_frame(void)
{
	fill(cov_x - 4, cov_y - 4, PANEL + 8, PANEL + 8, C_BORDER);
	fill(cov_x, cov_y, PANEL, PANEL, C_PANEL);
}

static void draw_cover_placeholder(void)
{
	int cx = cov_x + PANEL / 2, cy = cov_y + PANEL / 2;

	draw_cover_frame();
	frame(cx - 90, cy - 120, 180, 190, 6, C_BORDER);               /* a cartridge-ish outline */
	fill(cx - 66, cy - 98, 132, 84, C_BG);
	ctext(cx, cy + 90, 3, C_DIM, SYS_TITLE[cur_sys]);
	ctext(cx, cy + 150, 2, C_BORDER, "no cover");
}

static void draw_cover_image(void)
{
	int y, ox = cov_x + (PANEL - cov_w) / 2, oy = cov_y + (PANEL - cov_h) / 2;

	draw_cover_frame();
	for (y = 0; y < cov_h; y++) memcpy(bb + (size_t)(oy + y) * FBW + ox, covbuf + (size_t)y * cov_w, (size_t)cov_w * 2);
	dirty_add(ox, oy, cov_w, cov_h);
}

static void draw_info(void)
{
	struct rom *r = cur_rom();
	int y = cov_y + PANEL + 20, i, n;
	char lines[3][96], b[96];
	unsigned char l1[512];

	fill(cov_x - 4, y - 8, PANEL + 8, FBH - HINT_H - y, C_BG);
	if (!r) return;
	utf8_to_l1(r->name, l1, sizeof l1);
	n = wrap_text(l1, PANEL / 16, lines, 3);
	for (i = 0; i < n; i++) text(cov_x, y + i * 32, 2, C_TEXT, (unsigned char *)lines[i]);
	snprintf(b, sizeof b, "%d of %d   %s   %lld KB", sel[cur_sys] + 1, L[cur_sys].n,
		 strrchr(r->file, '.') ? strrchr(r->file, '.') + 1 : "", (long long)((r->size + 1023) / 1024));
	utf8_to_l1(b, l1, sizeof l1);
	text(cov_x, y + 3 * 32 + 6, 2, C_DIM, l1);
}

static void cover_invalidate(uint64_t now)
{
	if (cov_state == CV_SHOWN || cov_state == CV_PLACEHOLDER) draw_cover_frame();
	cov_state = CV_PENDING;
	cov_due = now + COVER_REST_MS;
}

static void cover_resolve(void)
{
	struct rom *r = cur_rom();
	char p[4600];

	if (r && cover_path(r, p, sizeof p) == 0 && cover_load(p, covbuf, &cov_w, &cov_h) == 0) {
		draw_cover_image();
		cov_state = CV_SHOWN;
	} else {
		draw_cover_placeholder();
		cov_state = CV_PLACEHOLDER;
	}
}

static void draw_empty_state(void)
{
	int x = 64, y = list_top_y + 40;

	fill(0, list_top_y - 12, FBW, FBH - HINT_H - list_top_y + 12, C_BG);
	text(x, y, 3, C_TEXT, (const unsigned char *)"No games found");
	text(x, y + 80, 2, C_TEXT, (const unsigned char *)"Put your game files in these folders on the SD card");
	text(x, y + 116, 2, C_TEXT, (const unsigned char *)"or on the USB stick:");
	text(x + 32, y + 190, 2, C_DIM, (const unsigned char *)"roms/gb     Game Boy         .gb  .zip");
	text(x + 32, y + 230, 2, C_DIM, (const unsigned char *)"roms/gbc    Game Boy Color   .gbc .zip");
	text(x + 32, y + 270, 2, C_DIM, (const unsigned char *)"roms/nes    NES              .nes .zip");
	text(x, y + 350, 2, C_TEXT, (const unsigned char *)"Then press START and choose Rescan ROMs.");
}

static const char *HINT_LIST = "UP/DOWN move   LEFT/RIGHT system   A play   START options";

static void draw_main(uint64_t now)
{
	fill(0, 0, FBW, FBH, C_BG);
	draw_title();
	if (!total_roms) {
		draw_empty_state();
	} else {
		draw_list_all();
		draw_cover_frame();
		draw_info();
		cov_state = CV_BLANK;
		cover_invalidate(now);
	}
	draw_hint(HINT_LIST);
}

/* selection moved: redraw only what changed */
static void sel_changed(int old, int oldtop, uint64_t now)
{
	if (topi[cur_sys] != oldtop) { draw_list_all(); }
	else {
		draw_row(old - topi[cur_sys]);
		draw_row(sel[cur_sys] - topi[cur_sys]);
		draw_scrollbar();
	}
	draw_info();
	cover_invalidate(now);
}

static void move_sel(int delta, uint64_t now)
{
	struct romlist *l = &L[cur_sys];
	int old = sel[cur_sys], oldtop = topi[cur_sys], s = old + delta;

	if (!l->n) return;
	if (s < 0) s = 0;
	if (s >= l->n) s = l->n - 1;
	if (s == old) return;
	sel[cur_sys] = s;
	if (s < topi[cur_sys]) topi[cur_sys] = s;
	if (s >= topi[cur_sys] + list_rows) topi[cur_sys] = s - list_rows + 1;
	sel_changed(old, oldtop, now);
}

static void switch_sys(int dir, uint64_t now)
{
	if (!total_roms) return;
	cur_sys = (cur_sys + dir + NSYS) % NSYS;
	draw_title();
	draw_list_all();
	draw_info();
	cov_state = CV_BLANK;
	draw_cover_frame();
	cover_invalidate(now);
}

/* -------------------------------------------------------------------- jobs */

enum { JOB_SD, JOB_NET, JOB_COVERS, NJOB };
enum { NET_STATUS, NET_UP, NET_DOWN };
static struct { pid_t pid; int what; } jobs[NJOB];
static int wifi_on = -1;             /* -1 unknown */
static char wifi_ip[48], opt_msg[96], cov_line[96];
static int opt_cur, opt_confirm = -1;    /* confirm: item index awaiting a second A */

static pid_t spawn(const char *out, const char *bin, const char *a1)
{
	pid_t pid = fork();

	if (pid == 0) {
		int fd = open(out ? out : "/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0644);
		int in = open("/dev/null", O_RDONLY);
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		if (in >= 0) dup2(in, 0);
		if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
		execl(bin, bin, a1, (char *)NULL);
		_exit(127);
	}
	return pid;
}

static int file_exec(const char *p) { return access(p, X_OK) == 0; }

static void read_first_line(const char *path, char *out, size_t cap, int last)
{
	char buf[512], *p, *line = NULL;
	FILE *f = fopen(path, "r");
	size_t n;

	out[0] = 0;
	if (!f) return;
	n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	for (p = strtok(buf, "\r\n"); p; p = strtok(NULL, "\r\n")) {
		while (*p == ' ') p++;
		if (!*p) continue;
		line = p;
		if (!last) break;
	}
	if (line) snprintf(out, cap, "%s", line);
}

static void net_status_parse(void)
{
	char buf[512], *p;
	FILE *f = fopen(p_jobout, "r");
	size_t n;

	wifi_ip[0] = 0;
	if (!f) return;
	n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	wifi_on = !strncmp(buf, "connected", 9);
	p = strstr(buf, "ip:");
	if (p && wifi_on) {
		p += 3;
		while (*p == ' ') p++;
		snprintf(wifi_ip, sizeof wifi_ip, "%.*s", (int)strcspn(p, "\r\n"), p);
		if (!strcmp(wifi_ip, "none")) wifi_ip[0] = 0;
	}
}

static void do_rescan(void);
static void draw_options(void);

static void job_done(int k, int status)
{
	(void)status;
	jobs[k].pid = 0;
	if (k == JOB_SD) {
		do_rescan();
		if (screen == SC_LIST) draw_main(now_ms());
	} else if (k == JOB_NET) {
		if (jobs[k].what == NET_STATUS) net_status_parse();
		else { jobs[k].pid = spawn(p_jobout, e_wifi, "status"); jobs[k].what = NET_STATUS; }
	} else if (k == JOB_COVERS) {
		read_first_line(p_covstat, cov_line, sizeof cov_line, 1);
		if (!cov_line[0]) snprintf(cov_line, sizeof cov_line, "download finished");
		if (screen == SC_LIST && cov_state != CV_BLANK) cover_invalidate(now_ms());
	}
	if (screen == SC_OPTIONS) draw_options();
}

static void reap_jobs(void)
{
	int st, k;
	pid_t p;

	g_chld = 0;
	while ((p = waitpid(-1, &st, WNOHANG)) > 0)
		for (k = 0; k < NJOB; k++)
			if (jobs[k].pid == p) { job_done(k, st); break; }
}

/* ------------------------------------------------------------------ options */

#define NOPT 5
static void opt_label(int i, char *b, size_t cap)
{
	switch (i) {
	case 0: snprintf(b, cap, "Rescan ROMs"); break;
	case 1:
		if (jobs[JOB_COVERS].pid) snprintf(b, cap, "Stop cover download");
		else if (!file_exec(e_covers)) snprintf(b, cap, "Download covers (not installed)");
		else snprintf(b, cap, "Download covers");
		break;
	case 2:
		if (jobs[JOB_NET].pid) snprintf(b, cap, "WiFi: working...");
		else if (wifi_on == 1) snprintf(b, cap, "WiFi: ON  - turn off");
		else snprintf(b, cap, "WiFi: OFF - turn on");
		break;
	case 3: snprintf(b, cap, opt_confirm == 3 ? "Power off: press A again" : "Power off"); break;
	default: snprintf(b, cap, opt_confirm == 4 ? "Reboot: press A again" : "Reboot"); break;
	}
}

static void draw_options(void)
{
	int bw = 760, bh = 500, bx = (FBW - bw) / 2, by = (FBH - bh) / 2, i, y;
	char b[160];

	fill(bx - 4, by - 4, bw + 8, bh + 8, C_BORDER);
	fill(bx, by, bw, bh, C_BOX);
	ctext(FBW / 2, by + 14, 3, C_TEXT, "Options");
	for (i = 0; i < NOPT; i++) {
		y = by + 74 + i * 52;
		if (i == opt_cur) fill(bx + 12, y, bw - 24, 46, C_SEL[cur_sys]);
		opt_label(i, b, sizeof b);
		utext(bx + 30, y + 7, 2, C_TEXT, b, (bw - 60) / 16);
	}
	y = by + 74 + NOPT * 52 + 8;
	snprintf(b, sizeof b, "Covers: %s", cov_line[0] ? cov_line : "-");
	utext(bx + 30, y, 2, C_DIM, b, (bw - 60) / 16);
	if (wifi_on == 1) snprintf(b, sizeof b, "WiFi: connected %s", wifi_ip);
	else snprintf(b, sizeof b, "WiFi: %s", wifi_on == 0 ? "not connected" : "unknown");
	utext(bx + 30, y + 34, 2, C_DIM, b, (bw - 60) / 16);
	if (opt_msg[0]) utext(bx + 30, y + 68, 2, C_OK, opt_msg, (bw - 60) / 16);
}

static void read_covers_status(void) { read_first_line(p_covstat, cov_line, sizeof cov_line, 1); }

static void open_options(void)
{
	screen = SC_OPTIONS;
	opt_cur = 0; opt_confirm = -1; opt_msg[0] = 0;
	read_covers_status();
	if (!jobs[JOB_NET].pid) { jobs[JOB_NET].pid = spawn(p_jobout, e_wifi, "status"); jobs[JOB_NET].what = NET_STATUS; }
	draw_hint("UP/DOWN choose   A select   B back");
	draw_options();
}

static void close_options(void)
{
	screen = SC_LIST;
	draw_main(now_ms());
}

static void run_and_wait(const char *bin)
{
	pid_t p = spawn(NULL, bin, NULL);
	int st;

	if (p > 0) while (waitpid(p, &st, 0) < 0 && errno == EINTR) {}
}

static void opt_activate(void)
{
	int i = opt_cur;

	if (i != 3 && i != 4) opt_confirm = -1;
	opt_msg[0] = 0;
	switch (i) {
	case 0:
		do_rescan();
		snprintf(opt_msg, sizeof opt_msg, "Found %d game%s", total_roms, total_roms == 1 ? "" : "s");
		break;
	case 1:
		if (jobs[JOB_COVERS].pid) {
			kill(jobs[JOB_COVERS].pid, SIGTERM);
			snprintf(opt_msg, sizeof opt_msg, "Stopping...");
		} else if (!file_exec(e_covers)) {
			snprintf(opt_msg, sizeof opt_msg, "olpc-covers is not installed");
		} else {
			unlink(p_covstat);
			cov_line[0] = 0;
			jobs[JOB_COVERS].pid = spawn(NULL, e_covers, "fetch");
			snprintf(opt_msg, sizeof opt_msg, "Started");
		}
		break;
	case 2:
		if (jobs[JOB_NET].pid) { snprintf(opt_msg, sizeof opt_msg, "Please wait"); break; }
		jobs[JOB_NET].what = wifi_on == 1 ? NET_DOWN : NET_UP;
		jobs[JOB_NET].pid = spawn(p_jobout, e_wifi, wifi_on == 1 ? "down" : "up");
		snprintf(opt_msg, sizeof opt_msg, wifi_on == 1 ? "Turning WiFi off" : "Connecting (can take a minute)");
		break;
	default:
		if (opt_confirm != i) { opt_confirm = i; break; }
		fill(0, 0, FBW, FBH, C_BG);
		ctext(FBW / 2, FBH / 2 - 24, 3, C_TEXT, i == 3 ? "Powering off..." : "Rebooting...");
		flush();
		run_and_wait(i == 3 ? e_poweroff : e_reboot);
		opt_confirm = -1;
		close_options();
		return;
	}
	draw_options();
}

/* ------------------------------------------------------------ rescan, state */

static char st_file[512];
static int st_sys = -1;

static void remember_selection(void)
{
	struct rom *r = cur_rom();

	if (r) { snprintf(st_file, sizeof st_file, "%s", r->file); st_sys = cur_sys; }
}

static void apply_selection(int sys, const char *file)
{
	int i;

	if (sys < 0 || sys >= NSYS) return;
	cur_sys = sys;
	for (i = 0; file && i < L[sys].n; i++)
		if (!strcmp(L[sys].v[i].file, file)) {
			sel[sys] = i;
			topi[sys] = i >= list_rows ? i - list_rows / 2 : 0;
			if (topi[sys] > L[sys].n - list_rows) topi[sys] = L[sys].n > list_rows ? L[sys].n - list_rows : 0;
			return;
		}
}

static void do_rescan(void)
{
	int i;

	remember_selection();
	rom_free(L);
	total_roms = rom_scan(e_scan, L);
	memset(sel, 0, sizeof sel); memset(topi, 0, sizeof topi);
	if (total_roms && !L[cur_sys].n)
		for (i = 0; i < NSYS; i++) if (L[i].n) { cur_sys = i; break; }
	if (st_sys >= 0) apply_selection(st_sys, st_file);
	if (screen == SC_LIST) {
		cov_state = CV_BLANK;
	}
}

static void save_state(int best_effort_copy)
{
	struct rom *r = cur_rom();

	if (!r) return;
	state_write(p_state, cur_sys, r->file);
	if (best_effort_copy) {
		pid_t p = fork();           /* the stick can be slow: never in the UI process */
		if (p == 0) {
			char path[4700];
			snprintf(path, sizeof path, "%s/../olpc-menu.state", r->romsdir);
			state_write(path, cur_sys, r->file);
			_exit(0);
		}
	}
}

static void load_state(void)
{
	char file[512], path[4700];
	int sys, i, j;

	if (state_read(p_state, &sys, file, sizeof file) == 0) { st_sys = sys; snprintf(st_file, sizeof st_file, "%s", file); return; }
	for (i = 0; i < NSYS; i++)
		for (j = 0; j < L[i].n; j++) {
			snprintf(path, sizeof path, "%s/../olpc-menu.state", L[i].v[j].romsdir);
			if (state_read(path, &sys, file, sizeof file) == 0) { st_sys = sys; snprintf(st_file, sizeof st_file, "%s", file); return; }
			break;      /* one probe per system */
		}
}

/* ------------------------------------------------------------------- input */

static struct input in;
static struct inp_dev sdev;                /* script-injected keys */
static int use_script;
static uint16_t prev_m, blocked, taint;
static int chord_now, g_resynced;
static struct rep rep;
static int ino_dev = -1, ino_run = -1;
static uint64_t sd_due;

#define LONG_BITS (8 * sizeof(unsigned long))

static int read_chord(void)
{
	char c = '0';
	int fd = open(e_chord, O_RDONLY | O_CLOEXEC);

	if (fd >= 0) { if (read(fd, &c, 1) != 1) c = '0'; close(fd); }
	return c == '1';
}

static uint16_t cur_mask(void)
{
	uint16_t m = 0;
	int i;

	chord_now = read_chord();
	for (i = 0; i < in.ndev; i++) {
		const struct inp_dev *d = &in.dev[i];
		uint16_t dm = inp_dev_mask(d);
		if (chord_now && d->cls == IN_BUILTIN) dm &= (uint16_t)~DIRS;
		m |= dm;
	}
	if (use_script) {
		uint16_t dm = inp_dev_mask(&sdev);
		if (chord_now) dm &= (uint16_t)~DIRS;
		m |= dm;
	}
	return m;
}

/* After a game: forget what happened while it ran and read the real button state, so keys
 * still held do not trigger anything (they must be released first). */
static void input_resync(void)
{
	int i, c;

	for (i = 0; i < in.ndev; i++) {
		struct inp_dev *d = &in.dev[i];
		unsigned long keys[(KEY_MAX + LONG_BITS) / LONG_BITS];
		struct input_event ev;
		struct input_absinfo ai;
		static const int abs[] = { ABS_HAT0X, ABS_HAT0Y, ABS_X, ABS_Y };

		inp_dev_drain(d);
		d->btn = d->hat = d->stick = 0;
		d->rotate = 0;
		memset(keys, 0, sizeof keys);
		if (ioctl(d->fd, EVIOCGKEY(sizeof keys), keys) == 0)
			for (c = 0; c <= KEY_MAX; c++)
				if ((keys[c / LONG_BITS] >> (c % LONG_BITS)) & 1) {
					memset(&ev, 0, sizeof ev); ev.type = EV_KEY; ev.code = (uint16_t)c; ev.value = 1;
					inp_dev_event(d, &ev);
				}
		if (d->cls == IN_PAD)
			for (c = 0; c < 4; c++)
				if (ioctl(d->fd, EVIOCGABS(abs[c]), &ai) == 0) {
					memset(&ev, 0, sizeof ev); ev.type = EV_ABS; ev.code = (uint16_t)abs[c]; ev.value = ai.value;
					inp_dev_event(d, &ev);
				}
	}
	blocked = cur_mask();
	taint = 0; prev_m = 0; g_resynced = 1;
	rep_stop(&rep);
}

static void launch(void);
static char err_lines[24][96];
static int err_n, err_rc;

static void on_button(int bit, uint64_t now)
{
	(void)now;
	if (screen == SC_ERROR) { screen = SC_LIST; draw_main(now_ms()); return; }
	if (screen == SC_OPTIONS) {
		if (bit == J_A) opt_activate();
		else if (bit == J_B || bit == J_START) close_options();
		return;
	}
	if (bit == J_A && cur_rom()) launch();
	else if (bit == J_START) open_options();
}

static void input_step(uint64_t now)
{
	uint16_t m = cur_mask(), mm, pressed, released, held_btns;
	int i, n;

	blocked &= m;
	mm = m & ~blocked;
	pressed = mm & ~prev_m;
	released = prev_m & ~mm;
	held_btns = mm & BTNS;
	/* A button counts only when pressed alone and no chord was active: the chords are made of
	 * these same buttons (CIRCLE+CHECK, SQUARE+CROSS).  It fires on release. */
	n = __builtin_popcount(held_btns);
	if (chord_now || n > 1) taint |= held_btns;
	for (i = 0; i < 16; i++) {
		uint16_t b = BIT(i);
		if (!(released & b) || !(BTNS & b)) continue;
		if (taint & b) { taint &= (uint16_t)~b; continue; }
		if (chord_now) continue;
		on_button(i, now);
	}
	if (g_resynced) { g_resynced = 0; return; }       /* a game ran: the state is new */
	taint &= (uint16_t)(held_btns | ~BTNS);
	/* vertical: press, then repeat from the held state */
	if (screen != SC_ERROR) {
		int page = screen == SC_LIST ? list_rows : 1;
		int dir = 0;

		if (pressed & BIT(J_UP)) dir = -1;
		else if (pressed & BIT(J_DOWN)) dir = 1;
		if (dir) {
			rep_start(&rep, dir, now);
			if (screen == SC_LIST) move_sel(dir, now);
			else { opt_cur = (opt_cur + dir + NOPT) % NOPT; opt_confirm = -1; draw_options(); }
		} else if ((rep.dir < 0 && !(mm & BIT(J_UP))) || (rep.dir > 0 && !(mm & BIT(J_DOWN)))) {
			if (mm & BIT(J_UP)) rep_start(&rep, -1, now);
			else if (mm & BIT(J_DOWN)) rep_start(&rep, 1, now);
			else rep_stop(&rep);
		}
		if (screen == SC_LIST) {
			if (pressed & BIT(J_LEFT)) switch_sys(-1, now);
			else if (pressed & BIT(J_RIGHT)) switch_sys(1, now);
		}
		n = rep_tick(&rep, now, page);
		if (n) {
			if (screen == SC_LIST) move_sel(n, now);
			else { opt_cur = (opt_cur + n % NOPT + NOPT) % NOPT; draw_options(); }
		}
	}
	prev_m = mm;
}

/* ------------------------------------------------------------------ launch */

static void load_error_text(void)
{
	char buf[4096];
	FILE *f = fopen(p_runerr, "r");
	size_t n = 0;
	unsigned char l1[4200];
	char *p, *lines[64];
	int nl = 0, i;

	err_n = 0;
	if (f) { n = fread(buf, 1, sizeof buf - 1, f); fclose(f); }
	buf[n] = 0;
	utf8_to_l1(buf, l1, sizeof l1);
	for (p = strtok((char *)l1, "\r\n"); p && nl < 64; p = strtok(NULL, "\r\n")) lines[nl++] = p;
	/* the last 14 lines that fit, long ones wrapped */
	for (i = nl > 14 ? nl - 14 : 0; i < nl && err_n < 24; i++) {
		char w[3][96];
		int k, c = wrap_text((unsigned char *)lines[i], 72, w, 3);
		if (!c) continue;
		for (k = 0; k < c && err_n < 24; k++) snprintf(err_lines[err_n++], 96, "%.95s", w[k]);
	}
}

static void draw_error(void)
{
	char b[96];
	int i, first = err_n > 17 ? err_n - 17 : 0;

	fill(0, 0, FBW, FBH, C_BG);
	fill(0, 0, FBW, TITLE_H, RGB(120, 30, 30));
	snprintf(b, sizeof b, "The game stopped (code %d)", err_rc);
	text(24, 12, 3, C_TEXT, (unsigned char *)b);
	if (!err_n) text(24, 110, 2, C_DIM, (const unsigned char *)"(no message)");
	for (i = first; i < err_n; i++) text(24, 100 + (i - first) * 32, 2, C_TEXT, (unsigned char *)err_lines[i]);
	draw_hint("Press any button");
}

static void launch(void)
{
	struct rom *r = cur_rom();
	char loading[600];
	pid_t pid;
	int st = 0, rc;
	int bx = (FBW - 800) / 2, by = (FBH - 160) / 2;

	save_state(1);
	if (jobs[JOB_COVERS].pid) { kill(jobs[JOB_COVERS].pid, SIGTERM); snprintf(cov_line, sizeof cov_line, "stopped to start a game"); }
	fill(bx - 4, by - 4, 808, 168, C_BORDER);
	fill(bx, by, 800, 160, C_BOX);
	ctext(FBW / 2, by + 20, 3, C_TEXT, "Loading...");
	snprintf(loading, sizeof loading, "%s", r->name);
	{
		unsigned char l1[600];
		utf8_to_l1(loading, l1, sizeof l1);
		fit_text(l1, 48);
		text(FBW / 2 - text_w(l1, 2) / 2, by + 90, 2, C_DIM, l1);
	}
	flush();
	logf_("launch %s %s", SYS_DIR[cur_sys], r->path);
	pid = fork();
	if (pid == 0) {
		sigset_t none;
		int fd = open(p_runerr, O_WRONLY | O_CREAT | O_TRUNC, 0644), z = open("/dev/null", O_RDONLY);
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		if (z >= 0) dup2(z, 0);
		if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
		execl(e_run, e_run, SYS_DIR[cur_sys], r->path, (char *)NULL);
		fprintf(stderr, "cannot run %s: %s\n", e_run, strerror(errno));
		_exit(127);
	}
	if (pid < 0) { rc = 127; goto after; }
	for (;;) {
		pid_t w = waitpid(pid, &st, 0);
		if (w == pid) break;
		if (w < 0 && errno == EINTR) { if (g_quit) kill(pid, SIGTERM); continue; }
		if (w < 0) break;
	}
	rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
after:
	logf_("olpc-run exited with %d", rc);
	input_resync();
	if (rc != 0) {
		err_rc = rc;
		load_error_text();
		screen = SC_ERROR;
		draw_error();
	} else {
		draw_main(now_ms());
	}
}

/* ------------------------------------------------------------------ script */

struct sev { uint64_t at; int kind, code, val; char arg[300]; };
enum { SK_KEY, SK_DUMP, SK_CHORD, SK_END };
static struct sev *sev;
static int nsev, isev;

static const struct { const char *n; int code; } keynames[] = {
	{ "UP", KEY_UP }, { "DOWN", KEY_DOWN }, { "LEFT", KEY_LEFT }, { "RIGHT", KEY_RIGHT },
	{ "A", KEY_END }, { "B", KEY_PAGEDOWN }, { "START", KEY_PAGEUP }, { "SELECT", KEY_HOME },
};

static int script_load(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[512];

	if (!f) { fprintf(stderr, "olpc-menu: --script %s: %s\n", path, strerror(errno)); return -1; }
	while (fgets(line, sizeof line, f)) {
		unsigned long ms;
		char cmd[32], a1[300], a2[32];
		int k, n;
		struct sev e;

		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '#' || !line[0]) continue;
		memset(&e, 0, sizeof e);
		n = sscanf(line, "%lu %31s %299s %31s", &ms, cmd, a1, a2);
		if (n < 2) { fprintf(stderr, "olpc-menu: script: bad line \"%s\"\n", line); fclose(f); return -1; }
		e.at = ms;
		if (!strcmp(cmd, "key") && n == 4) {
			e.kind = SK_KEY; e.code = -1;
			for (k = 0; k < (int)(sizeof keynames / sizeof keynames[0]); k++) if (!strcmp(a1, keynames[k].n)) e.code = keynames[k].code;
			if (e.code < 0) { fprintf(stderr, "olpc-menu: script: unknown key %s\n", a1); fclose(f); return -1; }
			e.val = !strcmp(a2, "down") ? 1 : 0;
			if (strcmp(a2, "down") && strcmp(a2, "up") && strcmp(a2, "tap")) { fprintf(stderr, "olpc-menu: script: bad action %s\n", a2); fclose(f); return -1; }
			if (!strcmp(a2, "tap")) {            /* down now, up 60 ms later */
				sev = realloc(sev, (size_t)(nsev + 2) * sizeof *sev);
				e.val = 1; sev[nsev++] = e; e.val = 0; e.at += 60; sev[nsev++] = e;
				continue;
			}
		} else if (!strcmp(cmd, "dump") && n >= 3) { e.kind = SK_DUMP; snprintf(e.arg, sizeof e.arg, "%s", a1); }
		else if (!strcmp(cmd, "chord") && n >= 3) { e.kind = SK_CHORD; e.val = atoi(a1); }
		else if (!strcmp(cmd, "end")) e.kind = SK_END;
		else { fprintf(stderr, "olpc-menu: script: bad line \"%s\"\n", line); fclose(f); return -1; }
		sev = realloc(sev, (size_t)(nsev + 1) * sizeof *sev);
		if (!sev) abort();
		sev[nsev++] = e;
	}
	fclose(f);
	/* stable sort by time (tap expands out of order) */
	{
		int i, j;
		for (i = 1; i < nsev; i++) {
			struct sev t = sev[i];
			for (j = i - 1; j >= 0 && sev[j].at > t.at; j--) sev[j + 1] = sev[j];
			sev[j + 1] = t;
		}
	}
	return 0;
}

static int sev_barrier(const struct sev *e) { return e->kind != SK_KEY; }

static void script_dump(const char *path)
{
	FILE *f = fopen(path, "wb");

	if (f) { fwrite(bb, 2, (size_t)FBW * FBH, f); fclose(f); }
	else logf_("dump %s: %s", path, strerror(errno));
}

/* ------------------------------------------------------------ main loop */

static void on_sig(int s) { (void)s; g_quit = 1; }
static void on_chld(int s) { (void)s; g_chld = 1; }
static void on_crash(int sig)
{
	static const char m[] = "olpc-menu: fatal signal, restoring the console\n";

	if (write(2, m, sizeof m - 1) < 0) {}
	if (fbmap && !g_fake) memset(fbmap, 0, fbstride * (size_t)FBH);
	console_text();
	_exit(128 + sig);
}

static void install_signals(void)
{
	struct sigaction sa;
	static const int term[] = { SIGINT, SIGTERM, SIGHUP };
	static const int crash[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
	unsigned i;

	memset(&sa, 0, sizeof sa);
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = on_sig;
	for (i = 0; i < sizeof term / sizeof term[0]; i++) sigaction(term[i], &sa, NULL);
	sa.sa_handler = on_chld;
	sigaction(SIGCHLD, &sa, NULL);
	sa.sa_handler = on_crash;
	for (i = 0; i < sizeof crash / sizeof crash[0]; i++) sigaction(crash[i], &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
}

static void handle_inotify(int fd, int is_dev)
{
	char b[4096] __attribute__((aligned(8)));
	ssize_t n = read(fd, b, sizeof b), o = 0;

	while (n > 0 && o < n) {
		struct inotify_event *e = (struct inotify_event *)(b + o);
		if (e->len) {
			if (is_dev && !strncmp(e->name, "mmcblk", 6)) sd_due = now_ms() + 1000;
			if (!is_dev && !strcmp(e->name, "covers.status")) {
				read_covers_status();
				if (screen == SC_OPTIONS) draw_options();
			}
		}
		o += (ssize_t)sizeof *e + e->len;
	}
}

static void usage(void)
{
	fputs("usage: olpc-menu [--script FILE]\n"
	      "  Game launcher.  --script FILE: timed key events for tests (lines: <ms> key <UP|DOWN|LEFT|RIGHT|A|B|START|SELECT>\n"
	      "  <down|up|tap> | <ms> dump FILE | <ms> chord 0|1 | <ms> end).  See the header of olpc-menu.c for env hooks.\n", stderr);
}

int main(int argc, char **argv)
{
	const char *script = NULL;
	struct pollfd pf[IN_MAXDEV + 4];
	sigset_t blockset, oldset;
	uint64_t t_start;
	int i;
	char runerr[300];

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
		else { usage(); return 2; }
	}
	e_scan = envd("OLPC_MENU_SCAN", "/mnt:/media");
	e_rundir = envd("OLPC_MENU_RUNDIR", "/run/olpcboy");
	e_run = envd("OLPC_MENU_RUN", "/usr/local/bin/olpc-run");
	e_covers = envd("OLPC_MENU_COVERS_BIN", "/usr/local/sbin/olpc-covers");
	e_wifi = envd("OLPC_MENU_WIFI_BIN", "/usr/local/sbin/olpc-wifi");
	e_sd = envd("OLPC_MENU_SDMOUNT", "/usr/local/sbin/olpc-sdmount");
	e_poweroff = envd("OLPC_MENU_POWEROFF", "/sbin/poweroff");
	e_reboot = envd("OLPC_MENU_REBOOT", "/sbin/reboot");
	e_inputdir = envd("OLPC_MENU_INPUTDIR", "/dev/input");
	e_devdir = envd("OLPC_MENU_DEVDIR", "/dev");
	snprintf(p_state, sizeof p_state, "%s/menu.state", e_rundir);
	snprintf(p_runerr, sizeof p_runerr, "%s/menu-run.err", e_rundir);
	snprintf(p_covstat, sizeof p_covstat, "%s/covers.status", e_rundir);
	snprintf(p_jobout, sizeof p_jobout, "%s/menu-net.out", e_rundir);
	snprintf(runerr, sizeof runerr, "%s/chord", e_rundir);
	e_chord = envd("OLPC_MENU_CHORD", runerr);
	mkdir(e_rundir, 0755);

	if (script && script_load(script) < 0) return 2;
	use_script = script != NULL;
	layout();
	if (fb_open() < 0) return 3;
	install_signals();

	input_init(&in, e_inputdir, NULL);
	memset(&sdev, 0, sizeof sdev);
	sdev.fd = -1; sdev.cls = IN_BUILTIN;
	ino_dev = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (ino_dev >= 0 && inotify_add_watch(ino_dev, e_devdir, IN_CREATE | IN_DELETE) < 0) { close(ino_dev); ino_dev = -1; }
	ino_run = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (ino_run >= 0 && inotify_add_watch(ino_run, e_rundir, IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO) < 0) { close(ino_run); ino_run = -1; }

	total_roms = rom_scan(e_scan, L);
	load_state();
	if (st_sys >= 0) apply_selection(st_sys, st_file);
	if (total_roms && !L[cur_sys].n)
		for (i = 0; i < NSYS; i++) if (L[i].n) { cur_sys = i; break; }
	logf_("%d games (%d gb, %d gbc, %d nes)", total_roms, L[0].n, L[1].n, L[2].n);
	read_covers_status();
	input_resync();       /* keys held while the menu starts must be released first */

	sigemptyset(&blockset);
	sigaddset(&blockset, SIGCHLD); sigaddset(&blockset, SIGINT); sigaddset(&blockset, SIGTERM); sigaddset(&blockset, SIGHUP);
	sigprocmask(SIG_BLOCK, &blockset, &oldset);

	t_start = now_ms();
	draw_main(t_start);

	while (!g_quit) {
		uint64_t now = now_ms();
		int timeout = -1, np = 0, t, r;
		struct timespec ts;

		if (g_chld) reap_jobs();
		input_poll(&in, now * 1000000ull);        /* drains the pads, rescans on hotplug */
		/* scripted key events (barriers wait until the screen has caught up) */
		while (use_script && isev < nsev && !sev_barrier(&sev[isev]) && t_start + sev[isev].at <= now) {
			struct input_event ev;
			memset(&ev, 0, sizeof ev); ev.type = EV_KEY; ev.code = (uint16_t)sev[isev].code; ev.value = sev[isev].val;
			inp_dev_event(&sdev, &ev);
			isev++;
			input_step(now);
		}
		if (ino_dev >= 0) { struct pollfd p = { ino_dev, POLLIN, 0 }; if (poll(&p, 1, 0) > 0) handle_inotify(ino_dev, 1); }
		if (ino_run >= 0) { struct pollfd p = { ino_run, POLLIN, 0 }; if (poll(&p, 1, 0) > 0) handle_inotify(ino_run, 0); }
		input_step(now);
		if (screen == SC_LIST && total_roms && cov_state == CV_PENDING && now >= cov_due) cover_resolve();
		if (sd_due && now >= sd_due) {
			if (!jobs[JOB_SD].pid && file_exec(e_sd)) { jobs[JOB_SD].pid = spawn(NULL, e_sd, NULL); sd_due = 0; }
			else sd_due = jobs[JOB_SD].pid ? now + 1000 : 0;
		}
		flush();
		/* script barriers: dump / chord / end, once everything due has been drawn */
		if (use_script && isev < nsev && sev_barrier(&sev[isev]) && t_start + sev[isev].at <= now &&
		    !(screen == SC_LIST && total_roms && cov_state == CV_PENDING && now >= cov_due)) {
			struct sev *e = &sev[isev++];
			if (e->kind == SK_DUMP) script_dump(e->arg);
			else if (e->kind == SK_CHORD) {
				FILE *f = fopen(e_chord, "w");
				if (f) { fputs(e->val ? "1\n" : "0\n", f); fclose(f); }
			} else if (e->kind == SK_END) break;
			continue;
		}
		if (use_script && isev >= nsev) break;

		/* sleep: only timers that exist wake us up */
		if (rep.dir) timeout = rep_timeout(&rep, now);
		if (screen == SC_LIST && total_roms && cov_state == CV_PENDING) { t = cov_due > now ? (int)(cov_due - now) : 0; if (timeout < 0 || t < timeout) timeout = t; }
		if (sd_due) { t = sd_due > now ? (int)(sd_due - now) : 0; if (timeout < 0 || t < timeout) timeout = t; }
		if (in.rescan > 0 && (timeout < 0 || timeout > 100)) timeout = 100;
		if (use_script && isev < nsev) { uint64_t at = t_start + sev[isev].at; t = at > now ? (int)(at - now) : 0; if (timeout < 0 || t < timeout) timeout = t; }
		for (i = 0; i < in.ndev; i++) { pf[np].fd = in.dev[i].fd; pf[np].events = POLLIN; np++; }
		if (in.ino >= 0) { pf[np].fd = in.ino; pf[np].events = POLLIN; np++; }
		if (ino_dev >= 0) { pf[np].fd = ino_dev; pf[np].events = POLLIN; np++; }
		if (ino_run >= 0) { pf[np].fd = ino_run; pf[np].events = POLLIN; np++; }
		ts.tv_sec = timeout / 1000; ts.tv_nsec = (long)(timeout % 1000) * 1000000L;
		r = ppoll(pf, (nfds_t)np, timeout < 0 ? NULL : &ts, &oldset);
		(void)r;
	}
	remember_selection();
	if (st_sys >= 0 && cur_rom()) save_state(0);
	for (i = 0; i < NJOB; i++) if (jobs[i].pid && i == JOB_COVERS) kill(jobs[i].pid, SIGTERM);
	fb_close();
	return exit_code;
}
