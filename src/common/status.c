/* Status parsers shared by olpc-menu and olpc-run (pure: no framebuffer, no evdev). */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include "status.h"

/* ---------------------------------------------------------------- status indicators */

static int tok_int(const char *p, int *out)
{
	char *e;
	long v;

	if (*p != '-' && (*p < '0' || *p > '9')) return -1;
	v = strtol(p, &e, 10);
	if ((*e && *e != ' ' && *e != '\n' && *e != '\r') || e == p || v > 100000 || v < -100000) return -1;
	*out = (int)v;
	return 0;
}

int status_parse(const char *buf, struct sysstat *s)
{
	char line[256], *p, *save = NULL;
	size_t n = strcspn(buf, "\n");
	int nl = buf[n] == '\n';
	struct { const char *k; int *v; } f[] = {
		{ "vol", &s->vol }, { "volmax", &s->volmax }, { "mute", &s->mute },
		{ "bri", &s->bri }, { "brimax", &s->brimax }, { "mono", &s->mono },
		{ "bat", &s->bat }, { "ac", &s->ac }, { "chg", &s->chg },
	};
	size_t i;

	s->vol = s->volmax = s->mute = s->bri = s->brimax = s->mono = s->bat = s->ac = s->chg = -1;
	if (n >= sizeof line) n = sizeof line - 1;
	memcpy(line, buf, n);
	line[n] = 0;
	if (!nl) { p = strrchr(line, ' '); if (p) *p = 0; else line[0] = 0; }      /* truncated: drop the last token */
	for (p = strtok_r(line, " \t\r", &save); p; p = strtok_r(NULL, " \t\r", &save)) {
		char *eq = strchr(p, '=');
		int v;

		if (!eq) continue;
		*eq = 0;
		for (i = 0; i < sizeof f / sizeof f[0]; i++) {
			if (strcmp(p, f[i].k)) continue;
			if (tok_int(eq + 1, &v) == 0 && v >= 0) *f[i].v = v;
			break;
		}
	}
	if (s->volmax == 0) s->volmax = -1;
	if (s->brimax == 0) s->brimax = -1;
	if (s->vol >= 0 && (s->volmax < 0 || s->vol > s->volmax)) s->vol = -1;
	if (s->bri >= 0 && (s->brimax < 0 || s->bri > s->brimax)) s->bri = -1;
	if (s->mute > 1) s->mute = -1;
	if (s->mono > 1) s->mono = -1;
	if (s->bat > 100) s->bat = -1;
	if (s->ac > 1) s->ac = -1;
	if (s->chg > 1) s->chg = -1;
	return (s->vol >= 0) + (s->volmax >= 0) + (s->mute >= 0) + (s->bri >= 0) + (s->brimax >= 0) + (s->mono >= 0) +
	       (s->bat >= 0) + (s->ac >= 0) + (s->chg >= 0);
}

int bat_level(int pct)
{
	return pct < 0 ? BAT_NONE : pct <= 5 ? BAT_CRIT : pct <= 15 ? BAT_LOW : BAT_NORMAL;
}

int pct_of_max(int v, int max)
{
	if (v < 0 || max <= 0) return -1;
	if (v > max) v = max;
	return (v * 100 + max / 2) / max;
}

int wifi_status_parse(const char *buf, struct wifistat *w)
{
	char line[256], *p, *q, *ip;
	size_t n = strcspn(buf, "\n");

	memset(w, 0, sizeof *w);
	w->state = WS_UNKNOWN;
	if (n >= sizeof line) n = sizeof line - 1;
	memcpy(line, buf, n);
	line[n] = 0;
	if (n && line[n - 1] == '\r') line[--n] = 0;
	if (strncmp(line, "state=", 6)) return w->state;
	p = line + 6;
	n = strcspn(p, " ");
	if (n == 3 && !strncmp(p, "off", 3)) w->state = WS_OFF;
	else if (n == 10 && !strncmp(p, "connecting", 10)) w->state = WS_CONNECTING;
	else if (n == 9 && !strncmp(p, "connected", 9)) {
		w->state = WS_CONNECTED;
		/* ssid=... may hold spaces: it runs up to the last " ip=" */
		ip = strstr(p, " ip=");
		for (q = ip; q; q = strstr(q + 1, " ip=")) ip = q;
		q = strstr(p, " ssid=");
		if (q && (!ip || q < ip)) {
			q += 6;
			snprintf(w->ssid, sizeof w->ssid, "%.*s", (int)((ip ? (size_t)(ip - q) : strlen(q))), q);
		}
		if (ip) snprintf(w->ip, sizeof w->ip, "%s", ip + 4);
	}
	return w->state;
}

int wireless_level(const char *buf, const char *ifname, int *dbm)
{
	size_t il = strlen(ifname);
	const char *p = buf;

	while (*p) {
		const char *e = strchr(p, '\n'), *q = p;

		while (*q == ' ' || *q == '\t') q++;
		if (!strncmp(q, ifname, il) && q[il] == ':') {
			char *end;
			long lev;
			int skip;

			/* "wlan0: 0000   70.  -40.  -256 ..." : status, link, level (dBm when negative) */
			q += il + 1;
			for (skip = 0; skip < 2; skip++) {          /* status, link */
				while (*q == ' ') q++;
				while (*q && *q != ' ' && *q != '\n') q++;
			}
			while (*q == ' ') q++;
			lev = strtol(q, &end, 10);
			if (end == q || lev >= 0 || lev < -200) return -1;
			*dbm = (int)lev;
			return 0;
		}
		if (!e) break;
		p = e + 1;
	}
	return -1;
}

int signal_bars(int dbm)
{
	return dbm >= -55 ? 4 : dbm >= -65 ? 3 : dbm >= -75 ? 2 : 1;
}

int ms_to_next_minute(long long t, int msec)
{
	long long s = 60 - (t % 60 + 60) % 60;
	long long ms = s * 1000 - (msec < 0 ? 0 : msec > 999 ? 999 : msec) + 30;

	return ms > 60030 ? 60030 : (int)ms;
}

void clock_text(const struct tm *tm, int h24, char *out, size_t cap)
{
	if (tm->tm_year + 1900 < 2024) { snprintf(out, cap, "--:--"); return; }
	if (h24) snprintf(out, cap, "%02d:%02d", tm->tm_hour, tm->tm_min);
	else snprintf(out, cap, "%d:%02d %s", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12, tm->tm_min, tm->tm_hour >= 12 ? "PM" : "AM");
}

int tzcfg_parse(const char *buf, char *tz, size_t cap, int *h24)
{
	const char *p = buf;

	while (*p) {
		char line[160], *a, *b, *save = NULL;
		size_t n = strcspn(p, "\n");

		if (n >= sizeof line) n = sizeof line - 1;
		memcpy(line, p, n);
		line[n] = 0;
		p += strcspn(p, "\n");
		if (*p) p++;
		a = strtok_r(line, " \t\r", &save);
		if (!a || *a == '#') continue;
		if (!((*a >= 'A' && *a <= 'Z') || (*a >= 'a' && *a <= 'z') || *a == '<' || *a == ':') || strlen(a) >= cap)
			return -1;
		for (b = a; *b; b++) if ((unsigned char)*b < 0x20 || (unsigned char)*b > 0x7e) return -1;
		snprintf(tz, cap, "%s", a);
		b = strtok_r(NULL, " \t\r", &save);
		if (b && !strcmp(b, "24")) *h24 = 1;
		else if (b && !strcmp(b, "12")) *h24 = 0;
		return 0;
	}
	return -1;
}

static ssize_t slurp_(const char *path, char *b, size_t cap)
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

void tz_apply(const char *cfgdir, const char *tzfile, int keep_env, char *tz_out, size_t cap, int *h24)
{
	char b[256], tz[64] = "AST4", path[400];
	int h = 0, got = 0;
	DIR *d = opendir(cfgdir);
	struct dirent *e;
	const char *env = getenv("TZ");

	while (d && (e = readdir(d))) {
		if (strcasecmp(e->d_name, "tz.cfg")) continue;
		snprintf(path, sizeof path, "%s/%s", cfgdir, e->d_name);
		if (slurp_(path, b, sizeof b) > 0 && tzcfg_parse(b, tz, sizeof tz, &h) == 0) got = 1;
		break;
	}
	if (d) closedir(d);
	if (!got && slurp_(tzfile, b, sizeof b) > 0) tzcfg_parse(b, tz, sizeof tz, &h);
	if (keep_env && env && *env) snprintf(tz, sizeof tz, "%s", env);
	else setenv("TZ", tz, 1);
	tzset();
	if (h24) *h24 = h;
	if (tz_out) snprintf(tz_out, cap, "%s", tz);
}
