/* netui: WiFi network screen, see netui.h.
 *
 * Data: the saved networks come from `olpc-wifi list`, the visible ones from /run/olpcboy/wifi.scan (written by
 * `olpc-wifi scan`, picked up through inotify), the link state from wifi.status.  Every olpc-wifi call is a child
 * process whose output goes to a file in the run dir; jobs run one at a time from a small queue, so the UI never
 * waits.  A chain (add -> connect) stops at its first failure and the reason from the RESULT line is shown.
 * The typed key lives in the keyboard state and in one queue entry; it goes to `olpc-wifi add` through a pipe
 * on stdin and is wiped from memory afterwards. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/wait.h>
#include "menulib.h"
#include "netui.h"
#include "osk.h"

static const struct netui_host *H;
static const struct netui_host *nd_h;

const struct netui_host *nd_host(void) { return nd_h; }
void netui_init(const struct netui_host *h) { H = nd_h = h; }

/* -------------------------------------------------------------------- drawing helpers */

void nd_fill(int x, int y, int w, int h, uint16_t c)
{
	int x1 = x + w, y1 = y + h;

	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x1 > H->g->w) x1 = H->g->w;
	if (y1 > H->g->h) y1 = H->g->h;
	if (x1 <= x || y1 <= y) return;
	gfx_fill(H->g, x, y, x1 - x, y1 - y, c);
	H->dirty(x, y, x1 - x, y1 - y);
}

void nd_frame(int x, int y, int w, int h, int t, uint16_t c)
{
	nd_fill(x, y, w, t, c); nd_fill(x, y + h - t, w, t, c);
	nd_fill(x, y, t, h, c); nd_fill(x + w - t, y, t, h, c);
}

static int to_l1(const char *s, int sc, int maxch, unsigned char *l1, size_t cap)
{
	(void)sc;
	utf8_to_l1(s, l1, cap);
	if (maxch > 0) fit_text(l1, maxch);
	return gfx_text_w(l1, sc);
}

int nd_utext(int x, int y, int sc, uint16_t fg, const char *s, int maxch)
{
	unsigned char l1[512];
	int w = to_l1(s, sc, maxch, l1, sizeof l1);

	gfx_text(H->g, x, y, sc, fg, l1);
	H->dirty(x, y, w, 16 * sc);
	return w;
}

void nd_ctext(int cx, int y, int sc, uint16_t fg, const char *s)
{
	unsigned char l1[512];
	int w = to_l1(s, sc, 0, l1, sizeof l1);

	gfx_text(H->g, cx - w / 2, y, sc, fg, l1);
	H->dirty(cx - w / 2, y, w, 16 * sc);
}

void nd_rtext(int rx, int y, int sc, uint16_t fg, const char *s)
{
	unsigned char l1[512];
	int w = to_l1(s, sc, 0, l1, sizeof l1);

	gfx_text(H->g, rx - w, y, sc, fg, l1);
	H->dirty(rx - w, y, w, 16 * sc);
}

void nd_hint(const char *s) { H->hint(s); }

/* ------------------------------------------------------------------------------ state */

#define MAXNET 64
#define SSID_CAP 160
#define BIT(n) ((uint16_t)(1u << (n)))
enum { SEC_OPEN, SEC_WEP, SEC_WPA, SEC_WPA2 };
static const char *const SEC_NAME[4] = { "OPEN", "WEP", "WPA", "WPA2" };

struct net { char ssid[SSID_CAP]; int dbm, sec, saved, vis, conn; };
struct saved { char ssid[SSID_CAP]; int sec; };
struct vis { char ssid[SSID_CAP]; int dbm, sec, conn; };
static struct saved sv[MAXNET]; static int nsv;
static struct vis vs[MAXNET]; static int nvs;
static struct net nets[2 * MAXNET]; static int nnets;
enum { R_NET, R_SCAN, R_OTHER, R_POWER, R_AUTO };
struct row { int kind, idx; };
static struct row rows[2 * MAXNET + 4]; static int nrows;
static int sel, top, moved;           /* moved: the user has moved the selection since the screen opened */
static int active, auto_on = 1, confirm_row = -1;
static uint16_t SEL;
static enum { NS_LIST, NS_OSK, NS_SEC } nstate;
static struct osk osk;
static enum { OP_KEY, OP_SSID } osk_purpose;
static char pend_ssid[SSID_CAP], pend_type[8];
static int sec_cur;
static char msg[200]; static uint16_t msg_col;
static char last_fail_ssid[SSID_CAP], last_fail_code[24];
static int ov;                                  /* 0 none, 1 working, 2 failure */
static int ov_hidden;
static char ov_title[200], ov_text[200];
static int spin_frame; static uint64_t spin_due;

#define LX 60
#define LW 1080
#define RH 48
#define STAT_Y 82
#define LIST_Y 134
#define MSG_H 44
#define HINT_H 56

static int list_rows(void) { return (H->g->h - HINT_H - MSG_H - LIST_Y) / RH; }

static void set_msg(uint16_t col, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_msg(uint16_t col, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
	msg_col = col;
}

static void scpy(char *d, size_t cap, const char *s)
{
	size_t n = strlen(s);

	if (n >= cap) n = cap - 1;
	memcpy(d, s, n);
	d[n] = 0;
}

static void wipe_mem(void *p, size_t n)
{
	volatile unsigned char *v = p;

	while (n--) *v++ = 0;
}

/* ------------------------------------------------------------------------------- jobs */

enum { K_LIST, K_AUTO, K_SCAN, K_ACT };
struct job {
	int kind, chain, overlay;
	char arg[4][SSID_CAP]; int n;
	char input[80]; int has_input;
	char okmsg[SSID_CAP + 40];
	char what[SSID_CAP + 40];                    /* overlay title */
};
#define MAXQ 10
static struct job q[MAXQ]; static int qn;
static struct { pid_t pid; struct job j; } cur;
static pid_t auto_pid;

static void out_path(char *b, size_t cap) { snprintf(b, cap, "%s/net-job.out", H->rundir); }

static void job_start(void);

static struct job *job_new(int kind)
{
	struct job *j;

	if (qn >= MAXQ) return NULL;
	j = &q[qn++];
	memset(j, 0, sizeof *j);
	j->kind = kind;
	return j;
}

static void job_arg(struct job *j, const char *a)
{
	if (j->n < 4) snprintf(j->arg[j->n++], SSID_CAP, "%s", a);
}

static void push_list(void) { struct job *j = job_new(K_LIST); if (j) job_arg(j, "list"); }
static void push_auto(void) { struct job *j = job_new(K_AUTO); if (j) { job_arg(j, "autoconnect"); job_arg(j, "status"); } }
static void push_scan(void) { struct job *j = job_new(K_SCAN); if (j) job_arg(j, "scan"); }
static void push_refresh(void) { push_list(); }

static pid_t spawn_job(struct job *j)
{
	int pfd[2] = { -1, -1 };
	pid_t pid;
	char out[300], *argv[8];
	int i;

	if (j->has_input && pipe(pfd) < 0) return -1;
	out_path(out, sizeof out);
	argv[0] = (char *)H->wifi_bin;
	for (i = 0; i < j->n; i++) argv[i + 1] = j->arg[i];
	argv[j->n + 1] = NULL;
	pid = fork();
	if (pid == 0) {
		sigset_t none;
		int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600), in = pfd[0] >= 0 ? pfd[0] : open("/dev/null", O_RDONLY);

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		if (in >= 0) dup2(in, 0);
		if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
		if (pfd[0] >= 0) close(pfd[0]);
		if (pfd[1] >= 0) close(pfd[1]);
		execv(argv[0], argv);
		_exit(127);
	}
	if (pfd[0] >= 0) close(pfd[0]);
	if (pid > 0 && pfd[1] >= 0) {
		/* a key is at most 64 characters: the write fits in the pipe and cannot block */
		size_t l = strlen(j->input);
		char nl = '\n';

		if (write(pfd[1], j->input, l) < 0 || write(pfd[1], &nl, 1) < 0) {}
	}
	if (pfd[1] >= 0) close(pfd[1]);
	wipe_mem(j->input, sizeof j->input);
	j->has_input = 0;
	return pid;
}

static void draw_all(void);
static void draw_status(void);
static void draw_msg(void);
static void draw_overlay(void);
static void draw_list(void);
static void rebuild_rows(void);

static void job_start(void)
{
	if (cur.pid || !qn) return;
	cur.j = q[0];
	memmove(&q[0], &q[1], (size_t)(qn - 1) * sizeof q[0]);
	qn--;
	wipe_mem(&q[qn], sizeof q[qn]);
	if (cur.j.kind == K_ACT && cur.j.overlay && !ov_hidden) {
		ov = 1;
		snprintf(ov_title, sizeof ov_title, "%s", cur.j.what);
		ov_text[0] = 0;
	}
	cur.pid = spawn_job(&cur.j);
	if (cur.pid <= 0) { cur.pid = 0; wipe_mem(&cur.j, sizeof cur.j); job_start(); return; }
	spin_due = 0;
	if (active) { draw_status(); if (ov) draw_overlay(); }
}

/* ------------------------------------------------------------------- parsing results */

static ssize_t slurp_path(const char *path, char *b, size_t cap)
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

static int sec_of(const char *s)
{
	int i;

	for (i = 0; i < 4; i++) if (!strcasecmp(s, SEC_NAME[i])) return i;
	return SEC_WPA2;
}

static void load_list(const char *buf)
{
	char *copy = strdup(buf), *p, *save = NULL;

	nsv = 0;
	if (!copy) return;
	for (p = strtok_r(copy, "\r\n", &save); p && nsv < MAXNET; p = strtok_r(NULL, "\r\n", &save)) {
		char *t = strchr(p, '\t');

		if (!t) continue;                                  /* RESULT lines and noise */
		*t++ = 0;
		if (!*p) continue;
		snprintf(sv[nsv].ssid, SSID_CAP, "%s", p);
		sv[nsv].sec = sec_of(t);
		nsv++;
	}
	free(copy);
}

static void load_scan_text(const char *buf)
{
	char *copy = strdup(buf), *p, *save = NULL;

	nvs = 0;
	if (!copy) return;
	for (p = strtok_r(copy, "\r\n", &save); p && nvs < MAXNET; p = strtok_r(NULL, "\r\n", &save)) {
		char *f[5] = { 0 };
		int k = 0;

		f[k++] = p;
		while (k < 5 && (p = strchr(p, '\t'))) { *p++ = 0; f[k++] = p; }
		if (k < 3 || !f[0][0]) continue;
		snprintf(vs[nvs].ssid, SSID_CAP, "%s", f[0]);
		vs[nvs].dbm = atoi(f[1]);
		vs[nvs].sec = sec_of(f[2]);
		vs[nvs].conn = k >= 5 && f[4][0] == '1';
		nvs++;
	}
	free(copy);
}

static void load_scan_file(void)
{
	char path[300], buf[8192];

	snprintf(path, sizeof path, "%s/wifi.scan", H->rundir);
	if (slurp_path(path, buf, sizeof buf) >= 0) load_scan_text(buf);
}

/* "RESULT <code> <text>": the last such line of the job output */
static int parse_result(const char *buf, char *code, size_t cc, char *text, size_t ct)
{
	const char *p = buf, *last = NULL;

	while ((p = strstr(p, "RESULT "))) {
		if (p == buf || p[-1] == '\n') last = p;
		p += 7;
	}
	if (!last) return -1;
	last += 7;
	{
		size_t n = strcspn(last, " \r\n");
		const char *t;

		snprintf(code, cc, "%.*s", (int)n, last);
		t = last + n;
		while (*t == ' ') t++;
		snprintf(text, ct, "%.*s", (int)strcspn(t, "\r\n"), t);
	}
	return 0;
}

static const char *reason_text(const char *code, const char *text)
{
	if (text[0]) return text;
	if (!strcmp(code, "wrongkey")) return "Wrong password";
	if (!strcmp(code, "notfound")) return "Network not found";
	if (!strcmp(code, "timeout")) return "No answer from the network";
	if (!strcmp(code, "dhcp")) return "Connected, but no IP address was given";
	if (!strcmp(code, "nostore")) return "Cannot save: the WiFi settings storage is read-only or missing";
	if (!strcmp(code, "busy")) return "WiFi is busy, try again";
	return "Connection failed";
}

/* -------------------------------------------------------------------------- rows model */

static void rebuild_rows(void)
{
	char keep[SSID_CAP] = "";
	int keep_kind = -1, i, j, k;

	if (moved && sel < nrows) {
		keep_kind = rows[sel].kind;
		if (rows[sel].kind == R_NET) snprintf(keep, sizeof keep, "%s", nets[rows[sel].idx].ssid);
	}
	nnets = 0;
	for (i = 0; i < nsv && nnets < 2 * MAXNET; i++) {
		struct net *n = &nets[nnets++];
		memset(n, 0, sizeof *n);
		scpy(n->ssid, sizeof n->ssid, sv[i].ssid);
		n->saved = 1; n->sec = sv[i].sec; n->dbm = -100;
		for (j = 0; j < nvs; j++)
			if (!strcmp(vs[j].ssid, sv[i].ssid)) { n->vis = 1; n->dbm = vs[j].dbm; n->sec = vs[j].sec; break; }
		n->conn = H->wifi->state == WS_CONNECTED && !strcmp(H->wifi->ssid, n->ssid);
	}
	for (j = 0; j < nvs && nnets < 2 * MAXNET; j++) {
		struct net *n;
		int dup = 0;

		for (i = 0; i < nsv; i++) if (!strcmp(vs[j].ssid, sv[i].ssid)) dup = 1;
		if (dup) continue;
		n = &nets[nnets++];
		memset(n, 0, sizeof *n);
		scpy(n->ssid, sizeof n->ssid, vs[j].ssid);
		n->vis = 1; n->dbm = vs[j].dbm; n->sec = vs[j].sec;
		n->conn = H->wifi->state == WS_CONNECTED && (!strcmp(H->wifi->ssid, n->ssid) || (!H->wifi->ssid[0] && vs[j].conn));
	}
	nrows = 0;
	for (k = 0; k < nnets; k++) { rows[nrows].kind = R_NET; rows[nrows++].idx = k; }
	rows[nrows].kind = R_SCAN; rows[nrows++].idx = 0;
	rows[nrows].kind = R_OTHER; rows[nrows++].idx = 0;
	rows[nrows].kind = R_POWER; rows[nrows++].idx = 0;
	rows[nrows].kind = R_AUTO; rows[nrows++].idx = 0;
	/* keep the selection on the same entry */
	if (keep_kind == R_NET) {
		for (i = 0; i < nrows; i++) if (rows[i].kind == R_NET && !strcmp(nets[rows[i].idx].ssid, keep)) { sel = i; break; }
	} else if (keep_kind > R_NET) {
		for (i = 0; i < nrows; i++) if (rows[i].kind == keep_kind) { sel = i; break; }
	}
	if (sel >= nrows) sel = nrows - 1;
	if (sel < 0) sel = 0;
	if (top > sel) top = sel;
	if (sel >= top + list_rows()) top = sel - list_rows() + 1;
	if (top < 0) top = 0;
	confirm_row = -1;
}

/* ---------------------------------------------------------------------------- drawing */

static const int SPIN[8][2] = { { 0, -13 }, { 9, -9 }, { 13, 0 }, { 9, 9 }, { 0, 13 }, { -9, 9 }, { -13, 0 }, { -9, -9 } };

static void spinner(int cx, int cy, int scale, uint16_t bg)
{
	int i;

	nd_fill(cx - 16 * scale, cy - 16 * scale, 32 * scale, 32 * scale, bg);
	for (i = 0; i < 8; i++) {
		int age = (spin_frame - i + 16) % 8;
		uint16_t c = age == 0 ? NC_TEXT : age == 1 ? NC_DIM : age < 4 ? NC_BORDER : NC_PANEL;
		nd_fill(cx + SPIN[i][0] * scale - 3 * scale, cy + SPIN[i][1] * scale - 3 * scale, 6 * scale, 6 * scale, c);
	}
}

static void draw_status(void)
{
	char b[160];
	const struct wifistat *w = H->wifi;
	const char *what = "";

	nd_fill(0, STAT_Y - 6, H->g->w, 48, NC_BG);
	if (w->state == WS_CONNECTED) snprintf(b, sizeof b, "Connected to %s   %s", w->ssid, w->ip);
	else if (w->state == WS_CONNECTING) snprintf(b, sizeof b, "Connecting...");
	else snprintf(b, sizeof b, "WiFi is off");
	nd_utext(LX, STAT_Y, 2, w->state == WS_CONNECTED ? NC_OK : NC_TEXT, b, 52);
	if (cur.pid) {
		what = cur.j.kind == K_SCAN ? "Scanning..." : cur.j.kind == K_ACT ? "Working..." : "Reading...";
		nd_rtext(H->g->w - 24 - 52, STAT_Y, 2, NC_DIM, what);
		spinner(H->g->w - 24 - 20, STAT_Y + 14, 1, NC_BG);
	}
}

static void draw_msg(void)
{
	nd_fill(0, H->g->h - HINT_H - MSG_H, H->g->w, MSG_H, NC_BG);
	if (msg[0]) nd_utext(LX, H->g->h - HINT_H - MSG_H + 8, 2, msg_col, msg, (H->g->w - 2 * LX) / 16);
}

static void lock_glyph(int x, int y, uint16_t c)       /* 22 x 26 */
{
	nd_fill(x, y + 12, 22, 14, c);
	nd_fill(x + 4, y, 4, 14, c); nd_fill(x + 14, y, 4, 14, c); nd_fill(x + 4, y, 14, 4, c);
}

static void bars(int x, int y, int n, int lit_ok, uint16_t on, uint16_t off)   /* 4 bars, 13 px pitch, base at y */
{
	int i;

	for (i = 0; i < 4; i++) nd_fill(x + i * 13, y - 8 * (i + 1), 9, 8 * (i + 1), lit_ok && i < n ? on : off);
}

static void check_mark(int x, int y, uint16_t c)       /* 18 x 16 */
{
	int i;

	for (i = 0; i < 6; i++) nd_fill(x + i, y + 7 + i, 4, 4, c);
	for (i = 0; i < 13; i++) nd_fill(x + 5 + i, y + 12 - i, 4, 4, c);
}

static void saved_mark(int cx, int cy, uint16_t c)
{
	int j;

	for (j = -5; j <= 5; j++) {
		int half = 5 - (j < 0 ? -j : j);
		nd_fill(cx - half, cy + j, 2 * half + 1, 1, c);
	}
}

static const char *power_label(void)
{
	return H->wifi->state == WS_OFF || H->wifi->state == WS_UNKNOWN ? "WiFi: turn on" : "WiFi: turn off";
}

static void draw_row(int i)       /* i = row on screen */
{
	int idx = top + i, y = LIST_Y + i * RH, on = idx == sel;
	uint16_t bg = on ? SEL : NC_BG;
	char b[200];

	nd_fill(LX, y, LW, RH, bg);
	if (idx >= nrows) return;
	if (rows[idx].kind == R_NET) {
		const struct net *n = &nets[rows[idx].idx];
		uint16_t fg = n->conn ? NC_OK : n->saved && !n->vis ? NC_DIM : NC_TEXT;
		int rx = LX + LW - 18;

		if (n->conn) check_mark(LX + 16, y + 14, NC_OK);
		else if (n->saved) saved_mark(LX + 24, y + RH / 2, n->vis ? NC_TEXT : NC_DIM);
		nd_utext(LX + 56, y + 8, 2, fg, n->ssid, 46);
		if (confirm_row == idx) {
			nd_rtext(rx, y + 8, 2, NC_WARN, "press SELECT again to forget");
			return;
		}
		if (!n->vis) { nd_rtext(rx, y + 8, 2, NC_DIM, "not in range"); return; }
		bars(rx - 52, y + 40, signal_bars(n->dbm), 1, on ? NC_TEXT : NC_OK, on ? gfx_blend(SEL, NC_TEXT, 90) : NC_BORDER);
		if (n->sec != SEC_OPEN) lock_glyph(rx - 52 - 18 - 22, y + 11, on ? NC_TEXT : NC_DIM);
		return;
	}
	if (rows[idx].kind == R_SCAN) nd_fill(LX + 14, y, LW - 28, 2, NC_BORDER);
	switch (rows[idx].kind) {
	case R_SCAN: snprintf(b, sizeof b, "%s", cur.pid && cur.j.kind == K_SCAN ? "Scanning..." : "Scan again"); break;
	case R_OTHER: snprintf(b, sizeof b, "Other network..."); break;
	case R_POWER: snprintf(b, sizeof b, "%s", power_label()); break;
	default: snprintf(b, sizeof b, "Connect at start: %s", auto_on ? "ON" : "OFF"); break;
	}
	nd_utext(LX + 56, y + 8, 2, NC_TEXT, b, 46);
}

static void draw_scrollbar(void)
{
	int h = list_rows() * RH, th, ty;

	nd_fill(LX + LW + 6, LIST_Y, 8, h, NC_BG);
	if (nrows <= list_rows()) return;
	nd_fill(LX + LW + 6, LIST_Y, 8, h, NC_PANEL);
	th = h * list_rows() / nrows;
	if (th < 16) th = 16;
	ty = LIST_Y + (h - th) * top / (nrows - list_rows());
	nd_fill(LX + LW + 6, ty, 8, th, NC_DIM);
}

static void draw_list(void)
{
	int i;

	nd_fill(0, LIST_Y - 4, H->g->w, list_rows() * RH + 8, NC_BG);
	for (i = 0; i < list_rows(); i++) draw_row(i);
	draw_scrollbar();
}

static void set_hint(void)
{
	if (ov == 2) nd_hint("A or B close");
	else if (ov == 1) nd_hint("B hide");
	else if (nstate == NS_SEC) nd_hint("UP/DOWN choose   A select   B back");
	else if (nstate == NS_OSK) nd_hint("D-pad move   A type   B delete   SELECT layer   START ok");
	else nd_hint("UP/DOWN move   A select   SELECT forget   START rescan   B back");
}

static void draw_box(int bx, int by, int bw, int bh)
{
	nd_fill(bx - 4, by - 4, bw + 8, bh + 8, NC_BORDER);
	nd_fill(bx, by, bw, bh, NC_BOX);
}

static void draw_overlay(void)
{
	int bw = 860, bh = 260, bx = (H->g->w - bw) / 2, by = (H->g->h - bh) / 2;

	if (!ov) return;
	draw_box(bx, by, bw, bh);
	if (ov == 1) {
		nd_utext(bx + 40, by + 36, 3, NC_TEXT, ov_title, 34);
		spinner(bx + 80, by + 170, 2, NC_BOX);
		nd_utext(bx + 160, by + 150, 2, NC_DIM, "This can take up to a minute", 40);
	} else {
		char lines[3][96];
		unsigned char l1[400];
		int n, i;

		nd_utext(bx + 40, by + 30, 3, NC_WARN, ov_title, 34);
		utf8_to_l1(ov_text, l1, sizeof l1);
		n = wrap_text(l1, (bw - 80) / 16, lines, 3);
		for (i = 0; i < n; i++) nd_utext(bx + 40, by + 100 + i * 36, 2, NC_TEXT, lines[i], 0);
	}
}

static void draw_sec(void)
{
	static const char *const L[4] = { "WPA2 (most networks)", "WPA", "WEP", "Open (no password)" };
	int bw = 760, bh = 400, bx = (H->g->w - bw) / 2, by = (H->g->h - bh) / 2, i;
	char b[SSID_CAP + 20];

	draw_box(bx, by, bw, bh);
	nd_ctext(H->g->w / 2, by + 14, 3, NC_TEXT, "Security");
	snprintf(b, sizeof b, "%s", pend_ssid);
	nd_ctext(H->g->w / 2, by + 62, 2, NC_DIM, b);
	for (i = 0; i < 4; i++) {
		int y = by + 110 + i * 62;

		if (i == sec_cur) nd_fill(bx + 12, y, bw - 24, 54, SEL);
		nd_utext(bx + 36, y + 11, 2, NC_TEXT, L[i], 40);
	}
}

static void draw_all(void)
{
	nd_fill(0, 0, H->g->w, H->g->h, NC_BG);
	H->title("WiFi");
	if (nstate == NS_OSK) {
		osk_draw(&osk, SEL);
	} else {
		draw_status();
		draw_list();
		draw_msg();
		if (nstate == NS_SEC) draw_sec();
		draw_overlay();
	}
	set_hint();
}

void netui_redraw(void) { if (active) draw_all(); }

/* ---------------------------------------------------------------------------- actions */

static void begin_chain(void) { ov_hidden = 0; }

static struct job *act_job(const char *what, const char *a0, const char *a1, const char *a2, int overlay)
{
	struct job *j = job_new(K_ACT);

	if (!j) return NULL;
	j->chain = 1; j->overlay = overlay;
	job_arg(j, a0);
	if (a1) job_arg(j, a1);
	if (a2) job_arg(j, a2);
	if (what) snprintf(j->what, sizeof j->what, "%s", what);
	return j;
}

static void do_connect_chain(const char *ssid, const char *add_type, const char *key)
{
	struct job *j;
	char what[SSID_CAP + 40];

	begin_chain();
	snprintf(what, sizeof what, "Connecting to %s", ssid);
	if (add_type) {
		j = act_job(what, "add", ssid, add_type, 1);
		if (j && key) { snprintf(j->input, sizeof j->input, "%s", key); j->has_input = 1; }
	}
	j = act_job(what, "connect", ssid, NULL, 1);
	if (j) snprintf(j->okmsg, sizeof j->okmsg, "Connected to %s", ssid);
	push_refresh();
	job_start();
}

static void open_osk(enum osk_mode mode, const char *prompt, int purpose)
{
	osk_init(&osk, mode, prompt);
	osk_purpose = purpose;
	nstate = NS_OSK;
}

static struct rep rv, rh;

static void stop_reps(void) { rep_stop(&rv); rep_stop(&rh); }

static void start_key_entry(const char *ssid, int sec)
{
	char p[SSID_CAP + 40];

	if (ssid != pend_ssid) scpy(pend_ssid, sizeof pend_ssid, ssid);
	snprintf(pend_type, sizeof pend_type, "%s", SEC_NAME[sec]);
	snprintf(p, sizeof p, "Password for %s", ssid);
	open_osk(sec == SEC_WEP ? OSK_KEY_WEP : OSK_KEY_WPA, p, OP_KEY);
	stop_reps();
	draw_all();
}

static void activate_row(void)
{
	const struct row *r;

	if (sel >= nrows) return;
	r = &rows[sel];
	msg[0] = 0;
	if (r->kind == R_NET) {
		const struct net *n = &nets[r->idx];

		if (n->conn) { set_msg(NC_DIM, "Already connected to %s", n->ssid); draw_msg(); return; }
		if (n->saved && !(n->sec != SEC_OPEN && !strcmp(last_fail_code, "wrongkey") && !strcmp(last_fail_ssid, n->ssid))) {
			do_connect_chain(n->ssid, NULL, NULL);
		} else if (n->sec == SEC_OPEN) {
			do_connect_chain(n->ssid, "OPEN", NULL);
		} else {
			start_key_entry(n->ssid, n->sec);
		}
	} else if (r->kind == R_SCAN) {
		if (!(cur.pid && cur.j.kind == K_SCAN)) { push_scan(); job_start(); }
	} else if (r->kind == R_OTHER) {
		pend_ssid[0] = 0;
		open_osk(OSK_SSID, "Network name (SSID)", OP_SSID);
		stop_reps();
		draw_all();
	} else if (r->kind == R_POWER) {
		struct job *j;
		int on = H->wifi->state == WS_CONNECTED || H->wifi->state == WS_CONNECTING;

		begin_chain();
		if (on) {
			j = act_job("Turning WiFi off", "down", NULL, NULL, 0);
			if (j) snprintf(j->okmsg, sizeof j->okmsg, "WiFi turned off");
		} else {
			j = act_job("Turning WiFi on", "up", NULL, NULL, 1);
			if (j) snprintf(j->okmsg, sizeof j->okmsg, "WiFi is on");
		}
		job_start();
	} else {
		struct job *j = act_job(NULL, "autoconnect", auto_on ? "off" : "on", NULL, 0);

		if (j) snprintf(j->okmsg, sizeof j->okmsg, "Connect at start is %s", auto_on ? "OFF" : "ON");
		push_auto();
		job_start();
	}
	if (nstate == NS_LIST) { draw_msg(); draw_list(); draw_status(); if (ov) draw_overlay(); }
}

static void forget_row(void)
{
	const struct net *n;
	struct job *j;

	if (sel >= nrows || rows[sel].kind != R_NET) { confirm_row = -1; return; }
	n = &nets[rows[sel].idx];
	if (!n->saved) { set_msg(NC_DIM, "%s is not a saved network", n->ssid); draw_msg(); return; }
	if (confirm_row != sel) {
		confirm_row = sel;
		set_msg(NC_WARN, "Forget %s? Press SELECT again, any other key cancels", n->ssid);
		draw_row(sel - top); draw_msg();
		return;
	}
	confirm_row = -1;
	begin_chain();
	j = act_job("Forgetting", "forget", n->ssid, NULL, 0);
	if (j) snprintf(j->okmsg, sizeof j->okmsg, "Forgot %s", n->ssid);
	push_refresh();
	job_start();
	draw_list(); draw_msg();
}

static void osk_leave(void)
{
	osk_wipe(&osk);
	nstate = NS_LIST;
	stop_reps();
	draw_all();
}

static void osk_submit(void)
{
	if (osk_purpose == OP_SSID) {
		snprintf(pend_ssid, sizeof pend_ssid, "%s", osk.text);
		osk_wipe(&osk);
		nstate = NS_SEC; sec_cur = 0;
		stop_reps();
		draw_all();
		return;
	}
	{
		char key[OSK_TEXT_MAX];

		snprintf(key, sizeof key, "%s", osk.text);
		osk_wipe(&osk);
		nstate = NS_LIST;
		stop_reps();
		do_connect_chain(pend_ssid, pend_type, key);
		wipe_mem(key, sizeof key);
		last_fail_code[0] = 0;
		draw_all();
	}
}

int netui_button(int bit, uint64_t now)
{
	(void)now;
	if (ov == 2) {                              /* failure box */
		if (bit == NJ_A || bit == NJ_B) { ov = 0; draw_all(); }
		return 0;
	}
	if (ov == 1) {                              /* working: B hides the box, the job goes on */
		if (bit == NJ_B) { ov = 0; ov_hidden = 1; draw_all(); }
		return 0;
	}
	if (nstate == NS_OSK) {
		enum osk_result r = OSK_NONE;
		int layer = osk.layer;

		if (bit == NJ_A) r = osk_press(&osk);
		else if (bit == NJ_B) r = osk_back(&osk);
		else if (bit == NJ_SELECT) osk_next_layer(&osk);
		else if (bit == NJ_START) r = osk_ok(&osk, NULL);
		if (r == OSK_CANCEL) { osk_leave(); return 0; }
		if (r == OSK_OK) { osk_submit(); return 0; }
		if (osk.layer != layer || bit == NJ_SELECT) osk_draw(&osk, SEL);
		else {
			osk_draw_field(&osk);
			if (osk.row == 4) osk_draw_cell(&osk, SEL, 4, osk.col);
		}
		return 0;
	}
	if (nstate == NS_SEC) {
		if (bit == NJ_B) { nstate = NS_LIST; draw_all(); }
		else if (bit == NJ_A) {
			if (sec_cur == 3) { nstate = NS_LIST; do_connect_chain(pend_ssid, "OPEN", NULL); draw_all(); }
			else { start_key_entry(pend_ssid, sec_cur == 0 ? SEC_WPA2 : sec_cur == 1 ? SEC_WPA : SEC_WEP); }
		}
		return 0;
	}
	if (bit != NJ_SELECT && confirm_row >= 0) { int c = confirm_row; confirm_row = -1; msg[0] = 0; if (c >= top && c < top + list_rows()) draw_row(c - top); draw_msg(); }
	switch (bit) {
	case NJ_B: return 1;
	case NJ_A: activate_row(); break;
	case NJ_START: if (!(cur.pid && cur.j.kind == K_SCAN)) { push_scan(); job_start(); msg[0] = 0; draw_msg(); draw_list(); } break;
	case NJ_SELECT: forget_row(); break;
	default: break;
	}
	return 0;
}

/* -------------------------------------------------------------------------------- input */

static int axis(struct rep *r, uint16_t mm, uint16_t pressed, int bminus, int bplus, uint64_t now, int page)
{
	int dir = 0;

	if (pressed & BIT(bminus)) dir = -1;
	else if (pressed & BIT(bplus)) dir = 1;
	if (dir) { rep_start(r, dir, now); return dir; }
	if ((r->dir < 0 && !(mm & BIT(bminus))) || (r->dir > 0 && !(mm & BIT(bplus)))) {
		if (mm & BIT(bminus)) rep_start(r, -1, now);
		else if (mm & BIT(bplus)) rep_start(r, 1, now);
		else rep_stop(r);
	}
	return rep_tick(r, now, page);
}

static void move_list(int n)
{
	int old = sel, oldtop = top, m = n % nrows;

	if (!nrows || !m) return;
	moved = 1;
	sel = ((sel + m) % nrows + nrows) % nrows;
	if (sel < top) top = sel;
	if (sel >= top + list_rows()) top = sel - list_rows() + 1;
	if (sel == 0) top = 0;
	if (confirm_row >= 0) { confirm_row = -1; msg[0] = 0; draw_msg(); }
	if (top != oldtop) { draw_list(); return; }
	draw_row(old - top); draw_row(sel - top);
}

void netui_dirs(uint16_t mm, uint16_t pressed, uint64_t now)
{
	int v, h = 0;

	if (ov) { stop_reps(); return; }
	if (nstate == NS_OSK) {
		int ov_row = osk.row, ov_col = osk.col, ol = osk.layer;

		v = axis(&rv, mm, pressed, NJ_UP, NJ_DOWN, now, 1);
		h = axis(&rh, mm, pressed, NJ_LEFT, NJ_RIGHT, now, 1);
		if (!v && !h) return;
		while (h) { osk_move(&osk, h > 0 ? 1 : -1, 0); h += h > 0 ? -1 : 1; }
		while (v) { osk_move(&osk, 0, v > 0 ? 1 : -1); v += v > 0 ? -1 : 1; }
		if (osk.layer != ol) { osk_draw(&osk, SEL); return; }
		osk_draw_cell(&osk, SEL, ov_row, ov_col);
		osk_draw_cell(&osk, SEL, osk.row, osk.col);
		if (osk.warn[0] == 0) osk_draw_field(&osk);
		return;
	}
	v = axis(&rv, mm, pressed, NJ_UP, NJ_DOWN, now, nstate == NS_LIST ? list_rows() : 1);
	rep_stop(&rh);
	if (!v) return;
	if (nstate == NS_SEC) {
		sec_cur = (sec_cur + v % 4 + 4) % 4;
		draw_sec();
		return;
	}
	move_list(v);
}

/* ------------------------------------------------------------------- timers, children */

static int spin_wanted(void) { return active && (cur.pid || ov == 1); }

void netui_tick(uint64_t now)
{
	if (!spin_wanted()) return;
	if (spin_due && now < spin_due) return;
	spin_due = now + 120;
	spin_frame = (spin_frame + 1) % 8;
	if (nstate != NS_OSK) {
		if (cur.pid) { spinner(H->g->w - 24 - 20, STAT_Y + 14, 1, NC_BG); }
		if (ov == 1) {
			int bw = 860, bh = 260, bx = (H->g->w - bw) / 2, by = (H->g->h - bh) / 2;
			spinner(bx + 80, by + 170, 2, NC_BOX);
		}
	}
}

int netui_timeout(uint64_t now)
{
	int t = -1, a;

	if (spin_wanted()) t = !spin_due || spin_due <= now ? 0 : (int)(spin_due - now);
	a = rep_timeout(&rv, now);
	if (a >= 0 && (t < 0 || a < t)) t = a;
	a = rep_timeout(&rh, now);
	if (a >= 0 && (t < 0 || a < t)) t = a;
	return t;
}

static void job_finished(int status)
{
	char buf[4096] = "", out[300], code[24] = "", text[200] = "";
	struct job j = cur.j;
	int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status), have;

	cur.pid = 0;
	wipe_mem(&cur.j, sizeof cur.j);
	out_path(out, sizeof out);
	slurp_path(out, buf, sizeof buf);
	have = parse_result(buf, code, sizeof code, text, sizeof text) == 0;
	switch (j.kind) {
	case K_LIST: load_list(buf); rebuild_rows(); break;
	case K_AUTO: {
		char *p = buf;

		while (*p == ' ' || *p == '\n') p++;
		if (!strncmp(p, "on", 2)) auto_on = 1;
		else if (!strncmp(p, "off", 3)) auto_on = 0;
		break;
	}
	case K_SCAN:
		load_scan_file();
		rebuild_rows();
		if (rc != 0) set_msg(NC_WARN, "Scan failed: %s", have ? reason_text(code, text) : "olpc-wifi did not answer");
		else if (!nvs) set_msg(NC_DIM, "No networks found - try again");
		else msg[0] = 0;
		break;
	default: {
		int ok = have ? !strcmp(code, "ok") : rc == 0;

		if (ok) {
			if (j.okmsg[0]) set_msg(NC_OK, "%s", j.okmsg);
			last_fail_code[0] = 0;
			if (!(qn && q[0].kind == K_ACT)) ov = 0;
		} else {
			int k;

			if (!have) snprintf(code, sizeof code, "failed");
			snprintf(ov_title, sizeof ov_title, "%s", !strcmp(j.arg[0], "connect") || !strcmp(j.arg[0], "up") ? "Could not connect" : "That did not work");
			snprintf(ov_text, sizeof ov_text, "%s", have ? reason_text(code, text) :
				 rc == 127 ? "olpc-wifi is not installed" : "olpc-wifi failed");
			if (!strcmp(j.arg[0], "connect") || !strcmp(j.arg[0], "up")) {
				snprintf(last_fail_ssid, sizeof last_fail_ssid, "%s", j.n > 1 ? j.arg[1] : "");
				snprintf(last_fail_code, sizeof last_fail_code, "%s", code);
			}
			set_msg(NC_WARN, "%s", ov_text);
			ov = 2;
			for (k = 0; k < qn; ) {                         /* the rest of the chain makes no sense now */
				if (q[k].kind == K_ACT && q[k].chain) {
					wipe_mem(&q[k], sizeof q[k]);
					memmove(&q[k], &q[k + 1], (size_t)(qn - k - 1) * sizeof q[0]);
					qn--;
				} else k++;
			}
		}
		break;
	}
	}
	wipe_mem(&j, sizeof j);
	job_start();
	if (active) {
		if (nstate == NS_OSK) return;
		draw_all();
	}
}

int netui_child(pid_t pid, int status)
{
	if (pid == auto_pid && pid > 0) { auto_pid = 0; return 1; }
	if (pid != cur.pid || !pid) return 0;
	job_finished(status);
	return 1;
}

void netui_notify(const char *name)
{
	if (!active) return;
	if (!strcmp(name, "wifi.scan")) {
		load_scan_file();
		rebuild_rows();
		if (nstate != NS_OSK) { draw_list(); }
	} else if (!strcmp(name, "wifi.status")) {
		rebuild_rows();
		if (nstate != NS_OSK) { draw_status(); draw_list(); }
	}
}

void netui_open(uint16_t sel_col, uint64_t now)
{
	(void)now;
	SEL = sel_col;
	active = 1;
	nstate = NS_LIST;
	ov = 0; ov_hidden = 0; msg[0] = 0; confirm_row = -1;
	stop_reps();
	sel = 0; top = 0; moved = 0;
	load_scan_file();
	rebuild_rows();
	if (!cur.pid && !qn) { push_list(); push_auto(); push_scan(); }
	draw_all();
	job_start();
}

void netui_busy_leave(void)
{
	active = 0;
	osk_wipe(&osk);
	confirm_row = -1;
	stop_reps();
}

void netui_autostart(void)
{
	pid_t p = fork();

	if (p == 0) {
		sigset_t none;
		int z = open("/dev/null", O_RDWR);

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		setsid();
		if (nice(19) < 0) {}
		if (z >= 0) { dup2(z, 0); dup2(z, 1); dup2(z, 2); }
		execl(H->wifi_bin, H->wifi_bin, "autostart", (char *)NULL);
		_exit(127);
	}
	if (p > 0) auto_pid = p;
}
