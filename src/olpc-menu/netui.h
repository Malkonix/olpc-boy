/* netui: the WiFi network screen of the launcher (saved/visible networks, connect, forget, manual SSID)
 * and its drawing helpers.  The launcher owns the framebuffer, the title bar and the input loop; this
 * module draws into the launcher's back buffer through the host callbacks, runs olpc-wifi as a child
 * process (never blocking the UI) and keeps its own key-repeat state.  The on-screen keyboard is in osk.c. */
#ifndef OLPC_NETUI_H
#define OLPC_NETUI_H
#include <stdint.h>
#include <sys/types.h>
#include "../common/gfx.h"

/* Button / direction bit numbers: the same as J_* in olpc-menu.c */
#define NJ_B 0
#define NJ_SELECT 2
#define NJ_START 3
#define NJ_UP 4
#define NJ_DOWN 5
#define NJ_LEFT 6
#define NJ_RIGHT 7
#define NJ_A 8

struct netui_host {
	const struct gfx *g;                      /* the launcher's back buffer */
	void (*dirty)(int x, int y, int w, int h);
	void (*title)(const char *s);             /* paints the title bar (with the indicators) */
	void (*hint)(const char *s);              /* paints the hint bar */
	const struct wifistat *wifi;              /* live /run/olpcboy/wifi.status */
	const char *wifi_bin;                     /* olpc-wifi */
	const char *rundir;                       /* /run/olpcboy */
};

void netui_init(const struct netui_host *h);
/* sel = selection colour of the current system */
void netui_open(uint16_t sel, uint64_t now);
/* A/B/START/SELECT released (NJ_* bit).  Returns 1 when the screen wants to be closed. */
int  netui_button(int bit, uint64_t now);
/* held directions (mm) and the ones pressed now: selection moves and their key repeat */
void netui_dirs(uint16_t mm, uint16_t pressed, uint64_t now);
void netui_tick(uint64_t now);                /* spinner */
int  netui_timeout(uint64_t now);             /* ms to the next timer, -1 = none (idle) */
int  netui_child(pid_t pid, int status);      /* 1 = that child was ours */
void netui_notify(const char *name);          /* inotify event on a file of the run dir */
void netui_redraw(void);
void netui_autostart(void);                   /* once after the launcher is up: olpc-wifi autostart, nice 19 */
void netui_busy_leave(void);                  /* the screen was left */

/* ---- drawing helpers shared with osk.c (clipped, they report dirty rectangles) ---------------- */
#define NRGB(r, g, b) ((uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))
#define NC_BG NRGB(14, 18, 26)
#define NC_TEXT NRGB(235, 238, 242)
#define NC_DIM NRGB(150, 160, 178)
#define NC_PANEL NRGB(26, 32, 44)
#define NC_BORDER NRGB(80, 92, 116)
#define NC_HINT NRGB(22, 26, 36)
#define NC_OK NRGB(150, 230, 150)
#define NC_BOX NRGB(30, 38, 54)
#define NC_WARN NRGB(255, 170, 120)
void nd_fill(int x, int y, int w, int h, uint16_t c);
void nd_frame(int x, int y, int w, int h, int t, uint16_t c);
int  nd_utext(int x, int y, int sc, uint16_t fg, const char *s, int maxch);    /* UTF-8, returns pixel width */
void nd_ctext(int cx, int y, int sc, uint16_t fg, const char *s);              /* centred on cx */
void nd_rtext(int rx, int y, int sc, uint16_t fg, const char *s);              /* right-aligned to rx */
void nd_hint(const char *s);
const struct netui_host *nd_host(void);
#endif
