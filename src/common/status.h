/* Status parsers shared by olpc-menu and olpc-run: /run/olpcboy/status, wifi.status,
 * /proc/net/wireless, the clock text and the TZ rule.  Pure logic, unit-tested in olpc-menu/selftest.c. */
#ifndef OLPC_STATUS_H
#define OLPC_STATUS_H
#include <stddef.h>

/* /run/olpcboy/status from olpc-chordd: first line "vol=27 volmax=31 mute=0 bri=13 brimax=15 mono=0".
 * Every field is -1 when absent, malformed or out of range (a negative value in the file means
 * "unknown" too).  A buffer that does not end in a newline is truncated: its last token is dropped.
 * Returns the number of valid fields. */
struct sysstat { int vol, volmax, mute, bri, brimax, mono; };
int  status_parse(const char *buf, struct sysstat *s);
/* percent of v within 0..max, rounded; -1 when either is unknown */
int  pct_of_max(int v, int max);

/* /run/olpcboy/wifi.status from olpc-wifi: "state=connected ssid=<text, may hold spaces> ip=<a.b.c.d>",
 * "state=connecting" or "state=off".  Anything else parses as WS_UNKNOWN (ssid/ip empty). */
enum { WS_UNKNOWN, WS_OFF, WS_CONNECTING, WS_CONNECTED };
struct wifistat { int state; char ssid[40]; char ip[48]; };
int  wifi_status_parse(const char *buf, struct wifistat *w);   /* returns w->state */

/* /proc/net/wireless: signal level in dBm of interface ifname.  0 and *dbm set, or -1 when the
 * interface has no line or its level is not a negative (dBm) number. */
int  wireless_level(const char *buf, const char *ifname, int *dbm);
/* >= -55 dBm 4 bars, >= -65 3, >= -75 2, else 1 */
int  signal_bars(int dbm);

/* Clock: ms from (t, msec) to the next minute boundary, plus 30 ms so the wake-up lands inside the
 * new minute; never more than 60030. */
int  ms_to_next_minute(long long t, int msec);
/* "3:07 PM" or (h24) "15:07"; "--:--" while the year is before 2024 (RTC unset, NTP not yet run). */
struct tm;
void clock_text(const struct tm *tm, int h24, char *out, size_t cap);
/* TZ.CFG: first non-empty, non-# line = POSIX TZ string, optional second token "24" or "12".
 * Returns 0 and fills tz (and *h24 only when the token is present, else left alone), -1 when there
 * is no usable TZ token. */
int  tzcfg_parse(const char *buf, char *tz, size_t cap, int *h24);

/* The time-zone rule of the launcher: TZ.CFG (file name matched case-insensitively) in cfgdir, else the
 * first usable line of tzfile, else "AST4".  Sets $TZ (musl reads only $TZ) and calls tzset(), unless
 * keep_env is nonzero and $TZ is already set and non-empty: that one is kept.  The 12/24-hour flag
 * always comes from the same files (*h24 = 0 unless a file says 24).  tz_out (cap bytes) gets the zone
 * in effect.  Reads files: call it before the real-time loop, never inside it. */
void tz_apply(const char *cfgdir, const char *tzfile, int keep_env, char *tz_out, size_t cap, int *h24);
#endif
