/* In-game indicator strip, see overlay.h. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "overlay.h"

/* Dim and neutral on black: well below the launcher's colours, and the three levels differ in luminance
 * (about 133 / 91 / 46 of 255) so they stay readable when the DCON is in mono mode, where only the
 * luminance of a pixel reaches the panel. */
#define RGB(r, g, b) ((uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))
static const struct ind_pal PAL = { 0, RGB(132, 138, 132), RGB(90, 94, 90), RGB(46, 48, 46), RGB(132, 138, 132) };

/* slot top (inside the strip) and left offset of each slot, in strip coordinates */
static const int SLOT_Y[OS_N] = { 0, 84, 168, 252, 336 };

static uint64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

int ovl_fits(int fbw, int fbh, int gx, int gy, int gw, int gh, int *x)
{
	int border = fbw - (gx + gw);

	(void)gy; (void)gh;
	if (gw <= 0 || border < OVL_W + 2 * OVL_MARGIN || OVL_Y + OVL_H > fbh) return 0;
	if (x) *x = gx + gw + (border - OVL_W) / 2;
	return 1;
}

void ovl_clock_lines(const char *txt, char *l1, char *l2, size_t cap)
{
	const char *sp = strchr(txt, ' ');

	if (!sp) { snprintf(l1, cap, "%s", txt); l2[0] = 0; return; }
	snprintf(l1, cap, "%.*s", (int)(sp - txt), txt);
	snprintf(l2, cap, "%s", sp + 1);
}

void ovl_init(struct overlay *o)
{
	memset(o, 0, sizeof *o);
	o->kstatus.fd = o->kwifi.fd = -1;
	o->period = OVL_PERIOD;
	o->bars = 4;
	o->wifi.state = WS_OFF;
	memset(&o->st, 0xff, sizeof o->st);
}

void ovl_close(struct overlay *o)
{
	if (o->kstatus.fd >= 0) close(o->kstatus.fd);
	if (o->kwifi.fd >= 0) close(o->kwifi.fd);
	o->kstatus.fd = o->kwifi.fd = -1;
}

/* ---- data */

/* One pread on a descriptor that stays open.  A stat() (tmpfs) notices a file that vanished or was replaced. */
static ssize_t kf_read(struct kfile *k, const char *path, char *b, size_t cap)
{
	struct stat s;
	ssize_t n;

	if (!path || stat(path, &s) < 0) {
		if (k->fd >= 0) { close(k->fd); k->fd = -1; }
		return -1;
	}
	if (k->fd >= 0 && ((uint64_t)s.st_ino != k->ino || (uint64_t)s.st_dev != k->dev)) { close(k->fd); k->fd = -1; }
	if (k->fd < 0) {
		k->fd = open(path, O_RDONLY | O_CLOEXEC);
		if (k->fd < 0) return -1;
		k->ino = (uint64_t)s.st_ino; k->dev = (uint64_t)s.st_dev;
	}
	n = pread(k->fd, b, cap - 1, 0);
	if (n < 0) { close(k->fd); k->fd = -1; return -1; }
	b[n] = 0;
	return n;
}

static void wall_now(const struct overlay *o, time_t *t)
{
	if (o->fake_base) {
		*t = o->fake_base + (time_t)((now_ns() - o->fake_t0) / 1000000000ull);
	} else {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		*t = ts.tv_sec;
	}
}

/* Reads everything the strip shows into the current-value fields. */
static void poll_data(struct overlay *o, uint64_t now, char *clk, size_t clkcap)
{
	char b[256];
	struct sysstat s;
	struct wifistat w;
	time_t t;
	struct tm tm;

	if (kf_read(&o->kstatus, o->status_path, b, sizeof b) < 0) memset(&o->st, 0xff, sizeof o->st);
	else if (status_parse(b, &s) > 0) o->st = s;
	if (kf_read(&o->kwifi, o->wifi_path, b, sizeof b) < 0) { memset(&o->wifi, 0, sizeof o->wifi); o->wifi.state = WS_OFF; }
	else if (wifi_status_parse(b, &w) != WS_UNKNOWN) {
		if (w.state == WS_CONNECTED && o->wifi.state != WS_CONNECTED) o->sig_due = 0;
		o->wifi = w;
	}
	if (o->wifi.state == WS_CONNECTED && now >= o->sig_due) {
		char wb[2048];
		int fd = o->wireless_path ? open(o->wireless_path, O_RDONLY | O_CLOEXEC) : -1, dbm;
		ssize_t n = -1;

		if (fd >= 0) { n = read(fd, wb, sizeof wb - 1); close(fd); }
		if (n > 0) wb[n] = 0;
		o->bars = n > 0 && wireless_level(wb, "wlan0", &dbm) == 0 ? signal_bars(dbm) : 4;
		o->sig_due = now + (uint64_t)OVL_SIG_MS * 1000000ull;
	}
	wall_now(o, &t);
	localtime_r(&t, &tm);
	clock_text(&tm, o->h24, clk, clkcap);
}

/* ---- drawing */

static void blit_slot(struct overlay *o, int slot, int h, uint8_t *fb, size_t stride, const struct pixtab *t)
{
	int row, col;
	int sy = SLOT_Y[slot];

	for (row = sy; row < sy + h && row < OVL_H; row++) {
		const uint16_t *src = o->px + (size_t)row * OVL_W;
		uint8_t *dst = fb + (size_t)(o->y + row) * stride + (size_t)o->x * (size_t)t->B;

		if (t->ident16) memcpy(dst, src, OVL_W * 2);
		else if (t->B == 2)
			for (col = 0; col < OVL_W; col++) { uint16_t v = src[col]; ((uint16_t *)dst)[col] = (uint16_t)(t->r[v >> 11] + t->g[(v >> 5) & 63] + t->b[v & 31]); }
		else
			for (col = 0; col < OVL_W; col++) { uint16_t v = src[col]; ((uint32_t *)dst)[col] = t->r[v >> 11] + t->g[(v >> 5) & 63] + t->b[v & 31]; }
	}
}

static void draw_slot(struct overlay *o, int slot)
{
	struct gfx g = { o->px, OVL_W, OVL_W, OVL_H };
	int sy = SLOT_Y[slot];
	char l1[16], l2[16];
	unsigned char t1[16], t2[16];

	switch (slot) {
	case OS_CLOCK: {
		int dim = o->sh_clk[0] == '-';
		gfx_fill(&g, 0, sy, OVL_W, IND_SLOT_H, PAL.bg);
		ovl_clock_lines(o->sh_clk, l1, l2, sizeof l1);
		memcpy(t1, l1, sizeof t1); memcpy(t2, l2, sizeof t2);
		if (l2[0]) {
			gfx_text(&g, (OVL_W - gfx_text_w(t1, 2)) / 2, sy, 2, PAL.on, t1);
			gfx_text(&g, (OVL_W - gfx_text_w(t2, 2)) / 2, sy + 32, 2, PAL.on, t2);
		} else {
			gfx_text(&g, (OVL_W - gfx_text_w(t1, 2)) / 2, sy + 16, 2, dim ? PAL.dim : PAL.on, t1);
		}
		break;
	}
	case OS_WIFI:
		ind_wifi(&g, (OVL_W - IND_W_WIFI) / 2, sy, &PAL, o->wifi.state, o->bars);
		break;
	case OS_VOL:
		ind_vol(&g, (OVL_W - IND_W_VOL) / 2, sy, &PAL, pct_of_max(o->st.vol, o->st.volmax), o->st.mute == 1);
		break;
	case OS_BRI:
		ind_bri(&g, (OVL_W - IND_W_BRI) / 2, sy, &PAL, pct_of_max(o->st.bri, o->st.brimax));
		break;
	default: {
		const char *s = o->st.mono == 1 ? "MONO" : o->st.mono == 0 ? "COLOR" : "--";

		gfx_fill(&g, 0, sy, OVL_W, 32, PAL.bg);
		snprintf((char *)t1, sizeof t1, "%s", s);
		gfx_text(&g, (OVL_W - gfx_text_w(t1, 2)) / 2, sy, 2, o->st.mono < 0 ? PAL.faint : PAL.on, t1);
		break;
	}
	}
}

/* Repaint the slots whose value differs from what they show (all of them with force). */
static void update_slots(struct overlay *o, const char *clk, int force, uint8_t *fb, size_t stride, const struct pixtab *t)
{
	int want_wifi = o->wifi.state * 16 + (o->wifi.state == WS_CONNECTED ? o->bars : 0);
	int vol = pct_of_max(o->st.vol, o->st.volmax), mute = o->st.mute == 1, bri = pct_of_max(o->st.bri, o->st.brimax);
	int s;

	o->dirty[OS_WIFI] = force || want_wifi != o->sh_wifi;
	o->dirty[OS_VOL] = force || vol != o->sh_vol || mute != o->sh_mute;
	o->dirty[OS_BRI] = force || bri != o->sh_bri;
	o->dirty[OS_MODE] = force || o->st.mono != o->sh_mono;
	o->dirty[OS_CLOCK] = force || strcmp(clk, o->sh_clk) != 0;
	o->sh_wifi = want_wifi; o->sh_vol = vol; o->sh_mute = mute; o->sh_bri = bri; o->sh_mono = o->st.mono;
	snprintf(o->sh_clk, sizeof o->sh_clk, "%s", clk);
	for (s = 0; s < OS_N; s++) {
		uint64_t t0;
		uint64_t dt;

		if (!o->dirty[s]) continue;
		t0 = now_ns();
		draw_slot(o, s);
		blit_slot(o, s, s == OS_MODE ? 32 : IND_SLOT_H, fb, stride, t);
		dt = now_ns() - t0;
		o->redraws++; o->redraw_ns += dt;
		if (dt > o->redraw_ns_max) o->redraw_ns_max = dt;
		o->dirty[s] = 0;
	}
}

int ovl_place(struct overlay *o, int fbw, int fbh, int gx, int gy, int gw, int gh,
	      uint8_t *fb, size_t stride, const struct pixtab *t)
{
	int x;
	char clk[16];

	o->on = ovl_fits(fbw, fbh, gx, gy, gw, gh, &x);
	if (!o->on) return 0;
	o->x = x; o->y = OVL_Y;
	poll_data(o, now_ns(), clk, sizeof clk);
	update_slots(o, clk, 1, fb, stride, t);
	o->full_paints++;
	o->countdown = o->period;
	return 1;
}

void ovl_frame(struct overlay *o, uint8_t *fb, size_t stride, const struct pixtab *t, int defer)
{
	uint64_t t0, now;
	char clk[16];

	if (--o->countdown > 0) return;
	if (defer) { o->countdown = 1; return; }
	o->countdown = o->period;
	if (!o->on) return;
	t0 = now = now_ns();
	poll_data(o, now, clk, sizeof clk);
	update_slots(o, clk, 0, fb, stride, t);
	t0 = now_ns() - t0;
	o->ticks++; o->tick_ns += t0;
	if (t0 > o->tick_ns_max) o->tick_ns_max = t0;
}
