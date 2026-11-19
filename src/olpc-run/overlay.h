/* In-game indicators: WiFi, volume, brightness, DCON mono/colour and the clock, drawn as a vertical strip
 * in the RIGHT border of the screen, outside the game rectangle.  The scaler never touches the border, so
 * the strip costs nothing per frame: it is painted once and then only the slot whose value changed is
 * repainted.  The strip is rendered in RAM (common/gfx.c, the launcher's icons) and the changed slot is
 * copied to the framebuffer (write-only: the framebuffer is never read).  Nothing here forks or touches
 * storage: the data come from tmpfs, procfs and the clock, polled about once a second.
 *
 * The caller (olpc-run.c) gives the screen and game rectangle to ovl_place() (after every clear of the
 * screen), calls ovl_frame() once per emulated frame, and reads the counters at exit. */
#ifndef OLPC_RUN_OVERLAY_H
#define OLPC_RUN_OVERLAY_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "scale.h"
#include "../common/gfx.h"

#define OVL_W 96                 /* strip width */
#define OVL_MARGIN 4             /* minimum free pixels between the strip and the screen edge / game */
#define OVL_Y 16                 /* top of the strip */
#define OVL_H 452                /* strip height: clock, WiFi, volume, brightness, battery, mode */
#define OVL_PERIOD 60            /* frames between polls (about 1 s) */
#define OVL_SIG_MS 30000         /* /proc/net/wireless: at most this often, only while connected */

enum { OS_CLOCK, OS_WIFI, OS_VOL, OS_BRI, OS_BAT, OS_MODE, OS_N };

/* A file kept open and read with one pread; reopened when it disappears or is replaced (olpc-wifi
 * replaces its status file atomically; olpc-chordd rewrites its in place). */
struct kfile { int fd; uint64_t dev, ino; };

struct overlay {
	/* configuration */
	const char *status_path, *wifi_path, *wireless_path;
	int period;                  /* frames between polls */
	int h24;                     /* 24-hour clock */
	time_t fake_base;            /* test hook: wall clock stand-in (0 = real) */
	uint64_t fake_t0;
	/* geometry: 0 = disabled (no room, or switched off) */
	int on;
	int x, y;                    /* strip rectangle on screen: x, y, OVL_W, OVL_H */
	/* data */
	struct kfile kstatus, kwifi;
	struct sysstat st;
	struct wifistat wifi;
	int bars;
	uint64_t sig_due;            /* monotonic ns */
	int countdown;
	/* what each slot shows now (-2 = never drawn) */
	int sh_wifi, sh_vol, sh_mute, sh_bri, sh_mono, sh_bat;
	char sh_clk[16];
	int dirty[OS_N];
	/* strip pixels */
	uint16_t px[OVL_W * OVL_H];
	/* counters for the statistics */
	unsigned long redraws, full_paints, ticks;
	uint64_t redraw_ns, redraw_ns_max, tick_ns, tick_ns_max;
};

void ovl_init(struct overlay *o);
/* Decide whether the strip fits and where, mark everything for repaint and paint it now (call it after
 * every clear of the screen).  Returns 1 when the strip is shown, 0 when the border is too narrow. */
int  ovl_place(struct overlay *o, int fbw, int fbh, int gx, int gy, int gw, int gh,
	       uint8_t *fb, size_t stride, const struct pixtab *t);
/* once per frame: a counter compare, except every period-th frame, when the data are polled and the
 * slots that changed are repainted.  defer: do nothing this frame (a blit is being skipped). */
void ovl_frame(struct overlay *o, uint8_t *fb, size_t stride, const struct pixtab *t, int defer);
void ovl_close(struct overlay *o);

/* pure helpers (unit-tested) */
/* Fits the strip in the right border?  Fills *x with its left edge. */
int  ovl_fits(int fbw, int fbh, int gx, int gy, int gw, int gh, int *x);
/* Clock text split in lines: "3:07 PM" -> "3:07" / "PM"; one line otherwise (l2 empty) */
void ovl_clock_lines(const char *txt, char *l1, char *l2, size_t cap);
#endif
