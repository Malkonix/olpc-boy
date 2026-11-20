/* olpc-menu: game launcher for the OLPC XO-1.  Draws straight to /dev/fb0 (1200x900 RGB565), reads
 * the built-in buttons and USB pads through the olpc-run input code, starts olpc-run for the chosen
 * game.  Sleeps in ppoll() when nothing changes.
 *
 * Test hooks (all optional): OLPC_MENU_FAKEFB (file standing in for /dev/fb0), OLPC_MENU_SCAN (base
 * dirs, default /mnt:/media), OLPC_MENU_RUNDIR (default /run/olpcboy), OLPC_MENU_CHORD, OLPC_MENU_RUN,
 * OLPC_MENU_LOGDIR (per-game run-NNN.stats/.log; default $BOOTDIR, else /var/log/olpc-current, else the run dir), OLPC_MENU_COVERS_BIN, OLPC_MENU_WIFI_BIN, OLPC_MENU_SDMOUNT, OLPC_MENU_POWEROFF, OLPC_MENU_REBOOT,
 * OLPC_MENU_INPUTDIR, OLPC_MENU_DEVDIR, OLPC_MENU_BACKLIGHT (glob pattern, default the backlight class dir plus a star),
 * OLPC_MENU_WIRELESS (default /proc/net/wireless), OLPC_MENU_CFGDIR (where TZ.CFG lives, default
 * /mnt/olpclog), OLPC_MENU_TZFILE (default /etc/TZ), OLPC_MENU_FAKETIME (epoch seconds standing in
 * for the wall clock, advancing with the monotonic clock), OLPC_MENU_DIRTYLOG (file: every flushed
 * rectangle) and --script FILE (timed key events, see test.sh).
 *
 * Title-bar indicators (WiFi, volume, brightness, clock) read /run/olpcboy/status (olpc-chordd) and
 * /run/olpcboy/wifi.status (olpc-wifi) when inotify reports them, /proc/net/wireless every 30 s while
 * connected, and wake the loop only at minute boundaries.  The time zone comes from TZ.CFG, else
 * /etc/TZ, else AST4 (musl honours only $TZ, so it is set here).
 * Exit codes: 0 ok, 2 usage, 3 no usable framebuffer. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
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
#include "../common/gfx.h"
#include "../olpc-run/input.h"
#include "netui.h"

/* ------------------------------------------------------------------ config */

static const char *e_scan, *e_rundir, *e_chord, *e_run, *e_covers, *e_wifi, *e_sd, *e_poweroff, *e_reboot,
		  *e_inputdir, *e_devdir, *e_backlight, *e_wireless, *e_cfgdir, *e_tzfile;
static char p_state[300], p_runerr[300], p_covstat[300], p_jobout[300], p_status[300], p_wifist[300];

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
static struct gfx G;                       /* the back buffer as a drawing surface */
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
	G.px = bb; G.stride = FBW; G.w = FBW; G.h = FBH;
	if (!g_fake) {
		g_tty = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
		if (g_tty < 0) g_tty = open("/dev/tty1", O_RDWR | O_NOCTTY | O_CLOEXEC);
		if (g_tty >= 0) {
			g_kd_saved = KD_TEXT;   /* the boot splash leaves tty1 in KD_GRAPHICS: never "restore" to that */
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

static FILE *dirtylog;

static void flush(void)
{
	int i, y;

	for (i = 0; i < ndirty; i++) {
		const struct rect *d = &dirty[i];
		if (dirtylog) { fprintf(dirtylog, "D %d %d %d %d\n", d->x0, d->y0, d->x1, d->y1); fflush(dirtylog); }
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
	int x1 = x + w, y1 = y + h;

	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x1 > FBW) x1 = FBW;
	if (y1 > FBH) y1 = FBH;
	if (x1 <= x || y1 <= y) return;
	gfx_fill(&G, x, y, x1 - x, y1 - y, c);
	dirty_add(x, y, x1 - x, y1 - y);
}

static void frame(int x, int y, int w, int h, int t, uint16_t c)
{
	fill(x, y, w, t, c); fill(x, y + h - t, w, t, c);
	fill(x, y, t, h, c); fill(x + w - t, y, t, h, c);
}

static int text_w(const unsigned char *s, int sc) { return gfx_text_w(s, sc); }

/* Latin-1 text, glyph pixels only (the caller painted the background). */
static void text(int x, int y, int sc, uint16_t fg, const unsigned char *s)
{
	gfx_text(&G, x, y, sc, fg, s);
	dirty_add(x, y, gfx_text_w(s, sc), 16 * sc);
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
static enum { SC_LIST, SC_OPTIONS, SC_ERROR, SC_NET } screen;
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

/* ------------------------------------------------------------ title-bar indicators */

/* Right-aligned slots, left to right: WiFi, volume, brightness, battery, clock.  Fixed widths, so a value
 * change repaints only its own slot. */
#define IND_Y 4
#define IND_H (TITLE_H - 8)           /* = IND_SLOT_H */
#define IND_GAP 20
#define W_WIFI IND_W_WIFI
#define W_VOL IND_W_VOL
#define W_BRI IND_W_BRI
#define W_BAT IND_W_BAT
#define W_CLOCK 192
static int ix_clock(void) { return FBW - 24 - W_CLOCK; }
static int ix_bat(void) { return ix_clock() - IND_GAP - W_BAT; }
static int ix_bri(void) { return ix_bat() - IND_GAP - W_BRI; }
static int ix_vol(void) { return ix_bri() - IND_GAP - W_VOL; }
static int ix_wifi(void) { return ix_vol() - IND_GAP - W_WIFI; }

static struct sysstat g_st = SYSSTAT_UNKNOWN;      /* /run/olpcboy/status */
static struct wifistat g_wifi = { WS_OFF, "", "" };           /* /run/olpcboy/wifi.status */
static int g_bars = 4, g_sys_bri = -1, g_sys_brimax = -1, g_h24;
static uint64_t sig_due, sysfs_due;
static int fake_on;
static time_t fake_base;
static uint64_t fake_t0;
static struct { int wifi, vol, mute, bri, bat, plug, low; char clk[16]; } shown;

static ssize_t slurp(const char *path, char *b, size_t cap)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0) return -1;
	n = read(fd, b, cap - 1);
	close(fd);
	if (n < 0) n = 0;
	b[n] = 0;
	return n;
}

static void read_status(void)
{
	char b[256];
	struct sysstat s;

	if (slurp(p_status, b, sizeof b) < 0) { memset(&g_st, 0xff, sizeof g_st); return; }    /* chordd not running: unknown */
	if (status_parse(b, &s) > 0) g_st = s;               /* empty or garbage (caught mid-write): keep what we had */
}

static void read_wifi(void)
{
	char b[256];
	struct wifistat w;

	if (slurp(p_wifist, b, sizeof b) < 0) { memset(&g_wifi, 0, sizeof g_wifi); g_wifi.state = WS_OFF; return; }
	if (wifi_status_parse(b, &w) == WS_UNKNOWN) return;
	if (w.state == WS_CONNECTED && g_wifi.state != WS_CONNECTED) sig_due = 0;
	g_wifi = w;
}

static void read_signal(uint64_t now)
{
	char b[2048];
	int dbm;

	g_bars = slurp(e_wireless, b, sizeof b) > 0 && wireless_level(b, "wlan0", &dbm) == 0 ? signal_bars(dbm) : 4;
	sig_due = now + 30000;
}

static void read_sysfs_bri(uint64_t now)
{
	glob_t g;
	size_t i;
	char p[400], b[32];

	sysfs_due = now + 30000;
	g_sys_bri = g_sys_brimax = -1;
	if (glob(e_backlight, GLOB_NOSORT, NULL, &g) == 0) {
		for (i = 0; i < g.gl_pathc && g_sys_brimax < 0; i++) {
			snprintf(p, sizeof p, "%s/max_brightness", g.gl_pathv[i]);
			if (slurp(p, b, sizeof b) <= 0 || atoi(b) <= 0) continue;
			g_sys_brimax = atoi(b);
			snprintf(p, sizeof p, "%s/brightness", g.gl_pathv[i]);
			g_sys_bri = slurp(p, b, sizeof b) > 0 ? atoi(b) : -1;
		}
	}
	globfree(&g);
}

static void wall_now(time_t *t, int *msec)
{
	if (fake_on) {
		uint64_t d = now_ms() - fake_t0;
		*t = fake_base + (time_t)(d / 1000); *msec = (int)(d % 1000);
	} else {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		*t = ts.tv_sec; *msec = (int)(ts.tv_nsec / 1000000);
	}
}

static void clock_now(char *out, size_t cap)
{
	time_t t;
	int ms;
	struct tm tm;

	wall_now(&t, &ms);
	localtime_r(&t, &tm);
	clock_text(&tm, g_h24, out, cap);
}

/* The zone must be set here: musl reads only $TZ (no /etc/TZ), and the image has no tzdata.  The rule
 * (TZ.CFG, then /etc/TZ, then AST4) is shared with olpc-run. */
static void tz_setup(void)
{
	char tz[64];
	int h24 = 0;

	tz_apply(e_cfgdir, e_tzfile, 0, tz, sizeof tz, &h24);
	g_h24 = h24;
	logf_("time zone %s, %s-hour clock", tz, h24 ? "24" : "12");
}

static uint16_t faint(void) { return gfx_blend(C_TITLE[cur_sys], C_TEXT, 70); }

static int bri_pct(void)
{
	if (g_st.bri >= 0 && g_st.brimax > 0) return pct_of_max(g_st.bri, g_st.brimax);
	return pct_of_max(g_sys_bri, g_sys_brimax);
}

static struct ind_pal ind_pal(uint16_t bg)
{
	struct ind_pal p = { bg, C_TEXT, C_DIM, faint(), C_TEXT };

	return p;
}

static void ind_wifi_(int x, uint16_t bg)
{
	struct ind_pal p = ind_pal(bg);

	ind_wifi(&G, x, IND_Y, &p, g_wifi.state, g_bars);
	dirty_add(x, IND_Y, IND_W_WIFI, IND_SLOT_H);
}

static void ind_vol_(int x, uint16_t bg)
{
	struct ind_pal p = ind_pal(bg);

	ind_vol(&G, x, IND_Y, &p, pct_of_max(g_st.vol, g_st.volmax), g_st.mute == 1);
	dirty_add(x, IND_Y, IND_W_VOL, IND_SLOT_H);
}

static void ind_bri_(int x, uint16_t bg)
{
	struct ind_pal p = ind_pal(bg);

	ind_bri(&G, x, IND_Y, &p, bri_pct());
	dirty_add(x, IND_Y, IND_W_BRI, IND_SLOT_H);
}

static void ind_bat_(int x, uint16_t bg)
{
	struct ind_pal p = ind_pal(bg);

	ind_bat(&G, x, IND_Y, &p, g_st.bat, g_st.ac, g_st.chg);
	dirty_add(x, IND_Y, IND_W_BAT, IND_SLOT_H);
}

static void draw_hint(const char *s);
static const char *list_hint(void);
/* discharging and nearly empty: the list hint turns into a warning */
static int bat_warn(void) { return g_st.bat >= 0 && g_st.bat <= 5 && g_st.ac != 1 && g_st.chg != 1; }

static void ind_clock(int x, uint16_t bg)
{
	unsigned char l1[16];

	fill(x, IND_Y, W_CLOCK, IND_H, bg);
	utf8_to_l1(shown.clk, l1, sizeof l1);
	text(x + W_CLOCK - text_w(l1, 3), 12, 3, shown.clk[0] == '-' ? C_DIM : C_TEXT, l1);
}

/* repaint only the slots whose value changed since they were last drawn */
static void ind_update(int force)
{
	int w = g_wifi.state * 16 + (g_wifi.state == WS_CONNECTED ? g_bars : 0);
	int v = pct_of_max(g_st.vol, g_st.volmax), m = g_st.mute == 1, b = bri_pct();
	char clk[16];
	uint16_t bg = C_TITLE[cur_sys];

	clock_now(clk, sizeof clk);
	if (force || w != shown.wifi) { shown.wifi = w; ind_wifi_(ix_wifi(), bg); }
	if (force || v != shown.vol || m != shown.mute) { shown.vol = v; shown.mute = m; ind_vol_(ix_vol(), bg); }
	if (force || b != shown.bri) { shown.bri = b; ind_bri_(ix_bri(), bg); }
	if (force || g_st.bat != shown.bat || (g_st.ac == 1 || g_st.chg == 1) != shown.plug) {
		shown.bat = g_st.bat; shown.plug = g_st.ac == 1 || g_st.chg == 1; ind_bat_(ix_bat(), bg);
	}
	if (bat_warn() != shown.low) { shown.low = bat_warn(); if (!force && screen == SC_LIST) draw_hint(list_hint()); }
	if (force || strcmp(clk, shown.clk)) { snprintf(shown.clk, sizeof shown.clk, "%s", clk); ind_clock(ix_clock(), bg); }
}

static void draw_title(void)
{
	char b[64];
	unsigned char l1[96];
	int nx;

	fill(0, 0, FBW, TITLE_H, C_TITLE[cur_sys]);
	snprintf(b, sizeof b, "< %s >", SYS_TITLE[cur_sys]);
	text(24, 12, 3, C_TEXT, (const unsigned char *)b);
	nx = 24 + (int)strlen(b) * 24 + 24;
	snprintf(b, sizeof b, "%d game%s", L[cur_sys].n, L[cur_sys].n == 1 ? "" : "s");
	utf8_to_l1(b, l1, sizeof l1);
	if (nx + text_w(l1, 2) <= ix_wifi() - 20) text(nx, 25, 2, C_DIM, l1);
	ind_update(1);
}

static void draw_hint(const char *s)
{
	fill(0, FBH - HINT_H, FBW, HINT_H, C_HINT);
	ctext(FBW / 2, FBH - HINT_H + 12, 2, C_DIM, s);
}

/* the WiFi network screen (netui.c) draws into the back buffer through these */
static void net_title(const char *s)
{
	fill(0, 0, FBW, TITLE_H, C_TITLE[cur_sys]);
	text(24, 12, 3, C_TEXT, (const unsigned char *)s);
	ind_update(1);
}
static struct netui_host net_host;

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
static const char *list_hint(void) { return bat_warn() ? "Battery low - plug in the charger" : HINT_LIST; }

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
	draw_hint(list_hint());
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
enum { NET_UP, NET_DOWN };
static struct { pid_t pid; int what; } jobs[NJOB];
static char opt_msg[96], cov_line[96];
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
		read_wifi();                 /* olpc-wifi wrote its final state; the inotify event may not be served yet */
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
	while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
		if (netui_child(p, st)) continue;
		for (k = 0; k < NJOB; k++)
			if (jobs[k].pid == p) { job_done(k, st); break; }
	}
}

/* ------------------------------------------------------------------ options */

#define NOPT 6
static int g_overlay = 1;       /* "In-game indicators": passed to olpc-run as --no-overlay when 0; kept in the state file */
static void save_state(int best_effort_copy);
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
		snprintf(b, cap, "WiFi networks...");
		break;
	case 3: snprintf(b, cap, opt_confirm == 3 ? "Power off: press A again" : "Power off"); break;
	case 4: snprintf(b, cap, opt_confirm == 4 ? "Reboot: press A again" : "Reboot"); break;
	default: snprintf(b, cap, "In-game indicators: %s", g_overlay ? "ON" : "OFF"); break;
	}
}

static void draw_options(void)
{
	int bw = 760, bh = 540, bx = (FBW - bw) / 2, by = (FBH - bh) / 2, i, y;
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
	if (g_wifi.state == WS_CONNECTED) snprintf(b, sizeof b, "WiFi: connected %s %s", g_wifi.ssid, g_wifi.ip);
	else snprintf(b, sizeof b, "WiFi: %s", g_wifi.state == WS_CONNECTING ? "connecting..." : "not connected");
	utext(bx + 30, y + 34, 2, C_DIM, b, (bw - 60) / 16);
	if (opt_msg[0]) utext(bx + 30, y + 68, 2, C_OK, opt_msg, (bw - 60) / 16);
}

static void read_covers_status(void) { read_first_line(p_covstat, cov_line, sizeof cov_line, 1); }

static void open_options(void)
{
	screen = SC_OPTIONS;
	opt_cur = 0; opt_confirm = -1; opt_msg[0] = 0;
	read_covers_status();
	read_wifi();
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
		screen = SC_NET;                 /* the network screen (netui.c) has its own WiFi on/off entry */
		netui_open(C_SEL[cur_sys], now_ms());
		return;
	case 5:
		g_overlay = !g_overlay;
		save_state(1);
		snprintf(opt_msg, sizeof opt_msg, g_overlay ? "Indicators shown during games" : "Indicators hidden during games");
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

	if (!r) { state_write_opt(p_state, cur_sys, st_file, g_overlay); return; }
	state_write_opt(p_state, cur_sys, r->file, g_overlay);
	if (best_effort_copy) {
		pid_t p = fork();           /* the stick can be slow: never in the UI process */
		if (p == 0) {
			char path[4700];
			snprintf(path, sizeof path, "%s/../olpc-menu.state", r->romsdir);
			state_write_opt(path, cur_sys, r->file, g_overlay);
			_exit(0);
		}
	}
}

static void load_state(void)
{
	char file[512], path[4700];
	int sys, i, j, ov;

	if (state_read_opt(p_state, &sys, file, sizeof file, &ov) == 0) { st_sys = sys; snprintf(st_file, sizeof st_file, "%s", file); if (ov >= 0) g_overlay = ov; return; }
	for (i = 0; i < NSYS; i++)
		for (j = 0; j < L[i].n; j++) {
			snprintf(path, sizeof path, "%s/../olpc-menu.state", L[i].v[j].romsdir);
			if (state_read_opt(path, &sys, file, sizeof file, &ov) == 0) { st_sys = sys; snprintf(st_file, sizeof st_file, "%s", file); if (ov >= 0) g_overlay = ov; return; }
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
	if (screen == SC_NET) {
		if (netui_button(bit, now)) {
			/* the network screen is larger than the options box: repaint the launcher under it first */
			netui_busy_leave();
			screen = SC_LIST;
			draw_main(now_ms());
			open_options();
		}
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
	if (screen == SC_NET) { netui_dirs(mm, pressed, now); prev_m = mm; return; }
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

/* ---- per-game statistics ----------------------------------------------------------------
 * olpc-run is started with --stats <logdir>/run-NNN.stats (written once, after the game, so the
 * emulation loop never touches the stick) and its output is copied to run-NNN.log.  NNN counts
 * the games of this boot from 001 (continued across a menu respawn by looking at what is in the
 * directory); only the newest STATS_KEEP games are kept. */
#define STATS_KEEP 30

static char p_logdir[300];

static void logdir_init(void)
{
	struct stat sb;
	const char *d = getenv("OLPC_MENU_LOGDIR");

	if (!d || !*d) d = envd("BOOTDIR", "/var/log/olpc-current");
	if (stat(d, &sb) != 0 || !S_ISDIR(sb.st_mode)) d = e_rundir;
	snprintf(p_logdir, sizeof p_logdir, "%s", d);
}

/* Number of a "run-NNN.stats" / "run-NNN.log" entry (digits only), else -1. */
static int run_entry_no(const char *name)
{
	char *end;
	long v;

	if (strncmp(name, "run-", 4) || name[4] < '0' || name[4] > '9') return -1;
	v = strtol(name + 4, &end, 10);
	if (v > 100000 || (strcmp(end, ".stats") && strcmp(end, ".log"))) return -1;
	return (int)v;
}

/* Next game number: one above the highest run-NNN file of this boot, 1 for the first. */
static int next_run_no(void)
{
	DIR *d = opendir(p_logdir);
	struct dirent *de;
	int hi = 0, n;

	if (!d) return 1;
	while ((de = readdir(d)))
		if ((n = run_entry_no(de->d_name)) > hi) hi = n;
	closedir(d);
	return hi + 1;
}

/* Remove run-NNN.* older than the newest STATS_KEEP games (cur is the newest). */
static void prune_runs(int cur)
{
	DIR *d = opendir(p_logdir);
	struct dirent *de;
	char p[600];
	int n;

	if (!d) return;
	while ((de = readdir(d))) {
		if ((n = run_entry_no(de->d_name)) < 0 || n > cur - STATS_KEEP) continue;
		snprintf(p, sizeof p, "%s/%s", p_logdir, de->d_name);
		unlink(p);
	}
	closedir(d);
}

static void copy_file(const char *from, const char *to)
{
	char buf[4096];
	size_t n, tot = 0;
	FILE *i = fopen(from, "r"), *o;

	if (!i) return;
	if (!(o = fopen(to, "w"))) { fclose(i); return; }
	while (tot < 65536 && (n = fread(buf, 1, sizeof buf, i)) > 0) { fwrite(buf, 1, n, o); tot += n; }
	fclose(i);
	fclose(o);
}

/* One summary line in the menu log from a run-NNN.stats file. */
static void log_stats(const char *path, int no)
{
	static const char *const keys[] = { "status", "frames_run", "fps_effective", "underruns", "frame_ms_max",
					    "overlay_redraws", "overlay_redraw_us_total", "overlay_redraw_us_max" };
	char val[8][48], line[256];
	FILE *f = fopen(path, "r");
	size_t i, kl;
	int any = 0;

	if (!f) { logf_("stats run-%03d: none (olpc-run wrote no statistics)", no); return; }
	for (i = 0; i < 8; i++) strcpy(val[i], "?");
	while (fgets(line, sizeof line, f)) {
		char *nl = strchr(line, '\n');
		if (nl) *nl = 0;
		for (i = 0; i < 8; i++) {
			kl = strlen(keys[i]);
			if (!strncmp(line, keys[i], kl) && line[kl] == '=') { snprintf(val[i], sizeof val[i], "%s", line + kl + 1); any = 1; }
		}
	}
	fclose(f);
	if (!any) { logf_("stats run-%03d: unreadable", no); return; }
	logf_("stats run-%03d: status=%s frames=%s fps=%s underruns=%s frame_ms_max=%s overlay_redraws=%s redraw_us_total=%s redraw_us_max=%s",
	      no, val[0], val[1], val[2], val[3], val[4], val[5], val[6], val[7]);
}

static void launch(void)
{
	struct rom *r = cur_rom();
	char loading[600];
	pid_t pid;
	int st = 0, rc, run_no = next_run_no();
	char stats_path[400], log_path[400];
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
	snprintf(stats_path, sizeof stats_path, "%s/run-%03d.stats", p_logdir, run_no);
	snprintf(log_path, sizeof log_path, "%s/run-%03d.log", p_logdir, run_no);
	logf_("launch %s %s (stats %s)", SYS_DIR[cur_sys], r->path, stats_path);
	pid = fork();
	if (pid == 0) {
		sigset_t none;
		int fd = open(p_runerr, O_WRONLY | O_CREAT | O_TRUNC, 0644), z = open("/dev/null", O_RDONLY);
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		if (z >= 0) dup2(z, 0);
		if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
		if (g_overlay) execl(e_run, e_run, "--stats", stats_path, SYS_DIR[cur_sys], r->path, (char *)NULL);
		else execl(e_run, e_run, "--stats", stats_path, "--no-overlay", SYS_DIR[cur_sys], r->path, (char *)NULL);
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
	copy_file(p_runerr, log_path);
	log_stats(stats_path, run_no);
	prune_runs(run_no);
	/* take the screen back explicitly: the child may have changed the VT mode */
	if (g_tty >= 0 && !g_fake) {
		int m = -1;
		ioctl(g_tty, KDGETMODE, &m);
		logf_("console mode after game: %d (1 = graphics)", m);
		if (ioctl(g_tty, KDSETMODE, KD_GRAPHICS) == 0) g_kd_graphics = 1;
	}
	input_resync();
	read_status(); read_wifi(); read_sysfs_bri(now_ms());
	if (rc != 0) {
		err_rc = rc;
		load_error_text();
		screen = SC_ERROR;
		draw_error();
	} else {
		draw_main(now_ms());
	}
	/* Repaint twice: on hardware the first repaint after a game was lost (black
	 * screen until the next change), so push the whole frame again shortly after. */
	flush();
	{ struct timespec ts = { 0, 200000000 }; nanosleep(&ts, NULL); }
	dirty_add(0, 0, FBW, FBH);
	flush();
}

/* ------------------------------------------------------------------ script */

struct sev { uint64_t at; int kind, code, val; char arg[300]; };
enum { SK_KEY, SK_DUMP, SK_CHORD, SK_END, SK_TIME, SK_SH, SK_MARK };
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
		else if (!strcmp(cmd, "time") && n >= 3) { e.kind = SK_TIME; snprintf(e.arg, sizeof e.arg, "%s", a1); }
		else if (!strcmp(cmd, "sh") && n >= 3) { e.kind = SK_SH; snprintf(e.arg, sizeof e.arg, "%s", a1); }
		else if (!strcmp(cmd, "mark")) e.kind = SK_MARK;
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
			if (!is_dev && !strcmp(e->name, "status")) read_status();
			if (!is_dev && !strcmp(e->name, "wifi.status")) {
				read_wifi();
				if (screen == SC_OPTIONS) draw_options();
				if (screen == SC_NET) netui_notify("wifi.status");
			}
			if (!is_dev && !strcmp(e->name, "wifi.scan") && screen == SC_NET) netui_notify("wifi.scan");
		}
		o += (ssize_t)sizeof *e + e->len;
	}
}

static void usage(void)
{
	fputs("usage: olpc-menu [--script FILE]\n"
	      "  Game launcher.  --script FILE: timed key events for tests (lines: <ms> key <UP|DOWN|LEFT|RIGHT|A|B|START|SELECT>\n"
	      "  <down|up|tap> | <ms> dump FILE | <ms> chord 0|1 | <ms> time EPOCH | <ms> sh FILE | <ms> mark | <ms> end).  See the header of olpc-menu.c for env hooks.\n", stderr);
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
	e_backlight = envd("OLPC_MENU_BACKLIGHT", "/sys/class/backlight/*");
	e_wireless = envd("OLPC_MENU_WIRELESS", "/proc/net/wireless");
	e_cfgdir = envd("OLPC_MENU_CFGDIR", "/mnt/olpclog");
	e_tzfile = envd("OLPC_MENU_TZFILE", "/etc/TZ");
	snprintf(p_status, sizeof p_status, "%s/status", e_rundir);
	snprintf(p_wifist, sizeof p_wifist, "%s/wifi.status", e_rundir);
	if (getenv("OLPC_MENU_DIRTYLOG")) dirtylog = fopen(getenv("OLPC_MENU_DIRTYLOG"), "w");
	if (getenv("OLPC_MENU_FAKETIME")) { fake_on = 1; fake_base = (time_t)atoll(getenv("OLPC_MENU_FAKETIME")); fake_t0 = now_ms(); }
	snprintf(p_state, sizeof p_state, "%s/menu.state", e_rundir);
	snprintf(p_runerr, sizeof p_runerr, "%s/menu-run.err", e_rundir);
	snprintf(p_covstat, sizeof p_covstat, "%s/covers.status", e_rundir);
	snprintf(p_jobout, sizeof p_jobout, "%s/menu-net.out", e_rundir);
	snprintf(runerr, sizeof runerr, "%s/chord", e_rundir);
	e_chord = envd("OLPC_MENU_CHORD", runerr);
	mkdir(e_rundir, 0755);
	logdir_init();

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
	tz_setup();
	read_status(); read_wifi(); read_sysfs_bri(now_ms());
	input_resync();       /* keys held while the menu starts must be released first */

	sigemptyset(&blockset);
	sigaddset(&blockset, SIGCHLD); sigaddset(&blockset, SIGINT); sigaddset(&blockset, SIGTERM); sigaddset(&blockset, SIGHUP);
	sigprocmask(SIG_BLOCK, &blockset, &oldset);

	net_host.g = &G; net_host.dirty = dirty_add; net_host.title = net_title; net_host.hint = draw_hint;
	net_host.wifi = &g_wifi; net_host.wifi_bin = e_wifi; net_host.rundir = e_rundir;
	netui_init(&net_host);
	t_start = now_ms();
	draw_main(t_start);
	netui_autostart();            /* connect to a saved network in the background (olpc-wifi decides) */

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
		if (g_wifi.state == WS_CONNECTED && now >= sig_due) read_signal(now);
		if (now >= sysfs_due) read_sysfs_bri(now);
		if (screen != SC_ERROR) ind_update(0);
		if (screen == SC_NET) netui_tick(now);
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
			} else if (e->kind == SK_TIME) { fake_on = 1; fake_base = (time_t)atoll(e->arg); fake_t0 = now_ms(); }
			else if (e->kind == SK_SH) { if (system(e->arg)) {} }
			else if (e->kind == SK_MARK) { if (dirtylog) { fputs("M\n", dirtylog); fflush(dirtylog); } }
			else if (e->kind == SK_END) break;
			continue;
		}
		if (use_script && isev >= nsev) break;

		/* sleep: only timers that exist wake us up */
		if (rep.dir) timeout = rep_timeout(&rep, now);
		if (screen == SC_LIST && total_roms && cov_state == CV_PENDING) { t = cov_due > now ? (int)(cov_due - now) : 0; if (timeout < 0 || t < timeout) timeout = t; }
		if (sd_due) { t = sd_due > now ? (int)(sd_due - now) : 0; if (timeout < 0 || t < timeout) timeout = t; }
		if (in.rescan > 0 && (timeout < 0 || timeout > 100)) timeout = 100;
		if (screen == SC_NET) { t = netui_timeout(now); if (t >= 0 && (timeout < 0 || t < timeout)) timeout = t; }
		{       /* clock: next minute boundary; WiFi signal: only while connected */
			time_t wt;
			int wms;
			wall_now(&wt, &wms);
			t = ms_to_next_minute((long long)wt, wms);
			if (timeout < 0 || t < timeout) timeout = t;
			if (g_wifi.state == WS_CONNECTED) { t = sig_due > now ? (int)(sig_due - now) : 0; if (t < timeout) timeout = t; }
		}
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
