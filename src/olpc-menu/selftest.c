/* olpc-menu unit tests (host, no hardware).  Also: selftest --mkcover FILE W H writes a test cover. */
#define _GNU_SOURCE
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include "menulib.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void touch(const char *dir, const char *name, int size)
{
	char p[1024];
	FILE *f;

	snprintf(p, sizeof p, "%s/%s", dir, name);
	f = fopen(p, "w");
	if (f) { while (size-- > 0) fputc('x', f); fclose(f); }
}
static void mk(const char *p) { char c[1100]; snprintf(c, sizeof c, "mkdir -p '%s'", p); if (system(c)) {} }

static void test_discovery(const char *t)
{
	char p[1024], b[1024];
	struct romlist L[NSYS];
	int total, i;

	snprintf(p, sizeof p, "%s/mnt/a/ROMs/GB", t); mk(p);
	touch(p, "zelda.GB", 10); touch(p, "alpha.zip", 20); touch(p, "Beta.gb", 30); touch(p, "notes.txt", 1);
	touch(p, "x.sav", 1); touch(p, ".hidden.gb", 1); touch(p, "noext", 1);
	snprintf(b, sizeof b, "%s/sub.gb", p); mk(b);             /* a directory named like a rom */
	snprintf(p, sizeof p, "%s/mnt/a/Roms/gbc", t); mk(p);
	touch(p, "Hamtaro - Ham-Hams Unite! (Europe) (En,Fr,De,Es,It).zip", 5); touch(p, "a.GBC", 5);
	snprintf(p, sizeof p, "%s/mnt/a/roms/GBA", t); mk(p); touch(p, "ignored.gba", 5); touch(p, "ignored.zip", 5);
	snprintf(p, sizeof p, "%s/mnt/b/roms/NES", t); mk(p); touch(p, "Super.nes", 7); touch(p, "mario.NES", 7);
	snprintf(p, sizeof p, "%s/mnt/c/notroms/gb", t); mk(p); touch(p, "no.gb", 7);
	snprintf(p, sizeof p, "%s/mnt/d/deeper/roms/gb", t); mk(p); touch(p, "deep.gb", 7);        /* only top level */
	snprintf(p, sizeof p, "%s/media/user/label/ROMS/nes", t); mk(p); touch(p, "usb.nes", 7);   /* depth 2 under media */
	snprintf(b, sizeof b, "%s/mnt:%s/media", t, t);
	total = rom_scan(b, L);
	printf("  discovery: gb=%d gbc=%d nes=%d total=%d\n", L[0].n, L[1].n, L[2].n, total);
	CHECK(L[0].n == 3 && L[1].n == 2 && L[2].n == 3 && total == 8);
	CHECK(!strcmp(L[0].v[0].name, "alpha") && !strcmp(L[0].v[1].name, "Beta") && !strcmp(L[0].v[2].name, "zelda"));
	CHECK(!strcmp(L[0].v[2].file, "zelda.GB"));
	CHECK(!strcmp(L[1].v[0].name, "a") && !strncmp(L[1].v[1].name, "Hamtaro", 7));
	CHECK(!strcmp(L[2].v[0].name, "mario") && !strcmp(L[2].v[1].name, "Super") && !strcmp(L[2].v[2].name, "usb"));
	CHECK(L[0].v[0].size == 20);
	for (i = 0; i < 3; i++) CHECK(L[i].n > 0);
	{
		char cp[1200], want[1200];
		cover_path(&L[1].v[0], cp, sizeof cp);
		snprintf(want, sizeof want, "%s/mnt/a/Roms/../covers/gbc/a.obc", t);
		CHECK(!strcmp(cp, want));
		cover_path(&L[2].v[2], cp, sizeof cp);
		snprintf(want, sizeof want, "%s/media/user/label/ROMS/../covers/nes/usb.obc", t);
		CHECK(!strcmp(cp, want));
	}
	rom_free(L);
	CHECK(rom_scan("/nonexistent", L) == 0);
	rom_free(L);
}

static void test_sort(void)
{
	char t[] = "/tmp/olpcmenu-sort-XXXXXX", p[1100];
	struct romlist L[NSYS];
	static const char *names[] = { "b.gb", "B2.gb", "a.gb", "A1.gb", "Zed.gb", "_x.gb", "10.gb", "9.gb" };
	static const char *want[] = { "10", "9", "_x", "a", "A1", "b", "B2", "Zed" };
	int i;

	if (!mkdtemp(t)) return;
	snprintf(p, sizeof p, "%s/m/roms/gb", t); mk(p);
	for (i = 0; i < 8; i++) touch(p, names[i], 1);
	snprintf(p, sizeof p, "%s/m", t);
	rom_scan(t, L);
	CHECK(L[0].n == 8);
	for (i = 0; i < 8 && i < L[0].n; i++) CHECK(!strcmp(L[0].v[i].name, want[i]));
	rom_free(L);
	snprintf(p, sizeof p, "rm -rf '%s'", t); if (system(p)) {}
}

static void test_repeat(void)
{
	struct rep r = { 0 };
	uint64_t t;
	int steps, s;

	CHECK(rep_tick(&r, 100, 17) == 0 && rep_timeout(&r, 0) == -1);
	rep_start(&r, 1, 1000);
	CHECK(rep_timeout(&r, 1000) == REP_DELAY_MS);
	CHECK(rep_tick(&r, 1299, 17) == 0);
	CHECK(rep_tick(&r, 1300, 17) == 1);
	/* held 0.3 s .. 1.5 s: about 12 per second */
	steps = 1;
	for (t = 1301; t < 2500; t++) { s = rep_tick(&r, t, 17); if (s == 1) steps++; CHECK(s == 0 || s == 1); }
	printf("  repeat: %d single steps between 300 ms and 1500 ms (expect ~15)\n", steps);
	CHECK(steps >= 14 && steps <= 16);
	/* after 1.5 s: page steps */
	steps = 0;
	for (t = 2500; t < 3500; t++) { s = rep_tick(&r, t, 17); if (s) { CHECK(s == 17); steps++; } }
	printf("  repeat: %d page steps in the next second (expect 5)\n", steps);
	CHECK(steps >= 4 && steps <= 6);
	rep_stop(&r);
	CHECK(rep_tick(&r, 9000, 17) == 0 && rep_timeout(&r, 9000) == -1);
	/* upward, and a late tick catches up but is bounded */
	rep_start(&r, -1, 0);
	CHECK(rep_tick(&r, 300, 17) == -1);
	s = rep_tick(&r, 800, 17);
	CHECK(s <= -1 && s >= -4);
	CHECK(rep_timeout(&r, 800) > 0);
}

static void test_state(const char *t)
{
	char p[1024], f[600];
	int sys;

	snprintf(p, sizeof p, "%s/state", t);
	CHECK(state_read(p, &sys, f, sizeof f) < 0);
	CHECK(state_write(p, 2, "Super Mario Bros. 3 (USA).zip") == 0);
	CHECK(state_read(p, &sys, f, sizeof f) == 0 && sys == 2 && !strcmp(f, "Super Mario Bros. 3 (USA).zip"));
	CHECK(state_write(p, 1, "x y.gbc") == 0 && state_read(p, &sys, f, sizeof f) == 0 && sys == 1 && !strcmp(f, "x y.gbc"));
	{
		char tmp[1100];
		snprintf(tmp, sizeof tmp, "%s.tmp", p);
		CHECK(access(tmp, F_OK) != 0);                  /* atomic: no temp file left */
	}
	{
		FILE *fp = fopen(p, "w"); fputs("system=bogus\nfile=a\n", fp); fclose(fp);
		CHECK(state_read(p, &sys, f, sizeof f) < 0);
	}
	CHECK(state_write(p, 9, "a") < 0);
	{	/* the In-game indicators switch rides along in the state file */
		int ov = 5;
		CHECK(state_write_opt(p, 1, "g.gbc", 0) == 0 && state_read_opt(p, &sys, f, sizeof f, &ov) == 0 && sys == 1 && !strcmp(f, "g.gbc") && ov == 0);
		CHECK(state_write_opt(p, 1, "g.gbc", 1) == 0 && state_read_opt(p, &sys, f, sizeof f, &ov) == 0 && ov == 1);
		CHECK(state_write(p, 1, "g.gbc") == 0 && state_read_opt(p, &sys, f, sizeof f, &ov) == 0 && ov == -1);
		CHECK(state_write_opt(p, 1, "g.gbc", 1) == 0 && state_read(p, &sys, f, sizeof f) == 0 && sys == 1);
	}
}

static void test_text(void)
{
	unsigned char b[256];
	char lines[4][96];
	int n;

	utf8_to_l1("Pok\xc3\xa9mon - Edici\xc3\xb3n", b, sizeof b);
	CHECK(!strcmp((char *)b, "Pok\xe9mon - Edici\xf3n"));
	utf8_to_l1("Caf\xe9 raw latin1", b, sizeof b);          /* invalid UTF-8: taken as Latin-1 */
	CHECK(!strcmp((char *)b, "Caf\xe9 raw latin1"));
	utf8_to_l1("a\xe2\x80\x99s \xe2\x80\x93 b\xe2\x80\xa6 \xe6\x97\xa5", b, sizeof b);
	CHECK(!strcmp((char *)b, "a's - b... ?"));
	utf8_to_l1("abcdefghij", b, sizeof b); fit_text(b, 10); CHECK(!strcmp((char *)b, "abcdefghij"));
	utf8_to_l1("abcdefghijk", b, sizeof b); fit_text(b, 10); CHECK(!strcmp((char *)b, "abcdefg..."));
	utf8_to_l1("Hamtaro - Ham-Hams Unite! (Europe) (En,Fr,De,Es,It)", b, sizeof b);
	n = wrap_text(b, 30, lines, 3);
	printf("  wrap: %d lines: \"%s\" / \"%s\"\n", n, lines[0], n > 1 ? lines[1] : "");
	CHECK(n == 2 && strlen(lines[0]) <= 30 && strlen(lines[1]) <= 30);
	utf8_to_l1("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", b, sizeof b);
	n = wrap_text(b, 20, lines, 3);
	CHECK(n == 3 && strlen(lines[2]) <= 20 && !strcmp(lines[2] + strlen(lines[2]) - 3, "..."));
}

static void test_cover(const char *t)
{
	static uint16_t buf[COVER_MAX * COVER_MAX];
	char p[1024];
	int w, h;
	FILE *f;
	unsigned char hd[16] = { 'O', 'B', 'C', '1', 4, 0, 2, 0 };
	uint16_t px[8] = { 1, 2, 3, 4, 5, 6, 7, 0xf800 };

	snprintf(p, sizeof p, "%s/c.obc", t);
	CHECK(cover_load(p, buf, &w, &h) < 0);
	f = fopen(p, "wb"); fwrite(hd, 1, 16, f); fwrite(px, 2, 8, f); fclose(f);
	CHECK(cover_load(p, buf, &w, &h) == 0 && w == 4 && h == 2 && buf[7] == 0xf800);
	f = fopen(p, "wb"); fwrite(hd, 1, 16, f); fwrite(px, 2, 7, f); fclose(f);        /* short */
	CHECK(cover_load(p, buf, &w, &h) < 0);
	hd[0] = 'X'; f = fopen(p, "wb"); fwrite(hd, 1, 16, f); fwrite(px, 2, 8, f); fclose(f);
	CHECK(cover_load(p, buf, &w, &h) < 0);
	hd[0] = 'O'; hd[4] = 0xe1; hd[5] = 0x01; f = fopen(p, "wb"); fwrite(hd, 1, 16, f); fclose(f);   /* 481 wide */
	CHECK(cover_load(p, buf, &w, &h) < 0);
	hd[4] = 0; hd[5] = 0; f = fopen(p, "wb"); fwrite(hd, 1, 16, f); fclose(f);           /* zero width */
	CHECK(cover_load(p, buf, &w, &h) < 0);
}

static int mkcover(const char *path, int w, int h)
{
	FILE *f = fopen(path, "wb");
	unsigned char hd[16] = { 'O', 'B', 'C', '1' };
	int x, y;

	if (!f) return 1;
	hd[4] = w & 255; hd[5] = w >> 8; hd[6] = h & 255; hd[7] = h >> 8;
	fwrite(hd, 1, 16, f);
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			int r = 40 + 200 * x / w, g = 40 + 200 * y / h, bl = 200 - 150 * x / w;
			int edge = x < 6 || y < 6 || x >= w - 6 || y >= h - 6;
			uint16_t v = edge ? 0xffff : (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (bl >> 3));
			if (((x / 32) + (y / 32)) % 2 && !edge) v = (uint16_t)(v & 0xf7de) >> 0;
			fputc(v & 255, f); fputc(v >> 8, f);
		}
	fclose(f);
	return 0;
}

static void test_indicators(void)
{
	struct sysstat s;
	struct wifistat w;
	struct tm tm;
	char tz[64], b[32];
	int h24, dbm;
	static const char wl[] =
		"Inter-| sta-|   Quality        |   Discarded packets               | Missed | WE\n"
		" face | tus | link level noise |  nwid  crypt   frag  retry   misc | beacon | 22\n"
		"    lo: 0000    0.    0.    0.        0      0      0      0      0        0\n"
		" wlan0: 0000   70.  -52.  -256        0      0      0      0      0        0\n";

	/* status file */
	CHECK(status_parse("vol=27 volmax=31 mute=0 bri=13 brimax=15 mono=1\n", &s) == 6);
	CHECK(s.vol == 27 && s.volmax == 31 && s.mute == 0 && s.bri == 13 && s.brimax == 15 && s.mono == 1);
	CHECK(pct_of_max(s.vol, s.volmax) == 87 && pct_of_max(0, 15) == 0 && pct_of_max(15, 15) == 100 && pct_of_max(-1, 15) == -1);
	CHECK(status_parse("", &s) == 0 && s.vol == -1 && s.bri == -1);
	CHECK(status_parse("vol=27 volmax=31 mute=0 bri=1", &s) == 3 && s.bri == -1 && s.volmax == 31);   /* truncated: last token dropped */
	CHECK(status_parse("vol=27 volmax=3", &s) == 0 && s.vol == -1);                                  /* no newline, no consistent pair */
	CHECK(status_parse("vol=-1 volmax=0 mute=0 bri=7 brimax=15 mono=0\n", &s) == 4 && s.vol == -1 && s.volmax == -1 && s.bri == 7);
	CHECK(status_parse("vol=40 volmax=31 mute=7 bri=3 brimax=15\n", &s) == 3 && s.vol == -1 && s.mute == -1 && s.bri == 3);
	CHECK(status_parse("garbage\x01\xff=== vol=abc volmax=,, \n", &s) == 0 && s.vol == -1);
	CHECK(status_parse("vol=5x volmax=31\n", &s) == 1 && s.vol == -1);
	CHECK(status_parse("\n\n", &s) == 0);
	/* battery fields (olpc-chordd >= the battery build); older lines simply lack them */
	CHECK(status_parse("vol=27 volmax=31 mute=0 bri=13 brimax=15 mono=1 bat=93 ac=1 chg=0\n", &s) == 9 && s.bat == 93 && s.ac == 1 && s.chg == 0 && s.vol == 27);
	CHECK(status_parse("vol=27 volmax=31 mute=0 bri=13 brimax=15 mono=1\n", &s) == 6 && s.bat == -1 && s.ac == -1 && s.chg == -1);   /* old format */
	CHECK(status_parse("vol=-1 volmax=-1 mute=0 bri=-1 brimax=-1 mono=-1 bat=-1 ac=-1 chg=0\n", &s) == 2 && s.bat == -1 && s.ac == -1 && s.chg == 0);
	CHECK(status_parse("bat=100 ac=0 chg=1\n", &s) == 3 && s.bat == 100 && s.ac == 0 && s.chg == 1);
	CHECK(status_parse("bat=0 ac=0 chg=0\n", &s) == 3 && s.bat == 0);
	CHECK(status_parse("bat=101 ac=2 chg=5\n", &s) == 0 && s.bat == -1 && s.ac == -1 && s.chg == -1);          /* out of range */
	CHECK(status_parse("bat=9x ac=1 chg=0\n", &s) == 2 && s.bat == -1 && s.ac == 1);
	CHECK(status_parse("vol=5 volmax=31 mute=0 bri=7 brimax=15 mono=0 bat=93 ac=1 ch", &s) == 8 && s.bat == 93 && s.chg == -1);   /* truncated: last token dropped */
	CHECK(status_parse("vol=5 volmax=31 mute=0 bri=7 brimax=15 mono=0 bat=9", &s) == 6 && s.bat == -1);
	CHECK(bat_level(-1) == BAT_NONE && bat_level(0) == BAT_CRIT && bat_level(5) == BAT_CRIT && bat_level(6) == BAT_LOW &&
	      bat_level(15) == BAT_LOW && bat_level(16) == BAT_NORMAL && bat_level(100) == BAT_NORMAL);

	/* wifi status */
	CHECK(wifi_status_parse("state=off\n", &w) == WS_OFF);
	CHECK(wifi_status_parse("state=connecting\n", &w) == WS_CONNECTING);
	CHECK(wifi_status_parse("state=connected ssid=Casa 5G ip=192.168.1.20\n", &w) == WS_CONNECTED);
	CHECK(!strcmp(w.ssid, "Casa 5G") && !strcmp(w.ip, "192.168.1.20"));
	CHECK(wifi_status_parse("state=connected ssid=a ip=b ssid=c ip=10.0.0.2\n", &w) == WS_CONNECTED && !strcmp(w.ssid, "a ip=b ssid=c") && !strcmp(w.ip, "10.0.0.2"));
	CHECK(wifi_status_parse("state=connected\n", &w) == WS_CONNECTED && !w.ssid[0] && !w.ip[0]);
	CHECK(wifi_status_parse("", &w) == WS_UNKNOWN);
	CHECK(wifi_status_parse("state=conn", &w) == WS_UNKNOWN);
	CHECK(wifi_status_parse("state=offline\n", &w) == WS_UNKNOWN);
	CHECK(wifi_status_parse("\x01\x02 junk", &w) == WS_UNKNOWN);

	/* /proc/net/wireless */
	CHECK(wireless_level(wl, "wlan0", &dbm) == 0 && dbm == -52);
	CHECK(wireless_level(wl, "wlan1", &dbm) == -1);
	CHECK(wireless_level("Inter-| x\n face | y\n", "wlan0", &dbm) == -1);
	CHECK(wireless_level(" wlan0: 0000   70.   180.  -256\n", "wlan0", &dbm) == -1);          /* not dBm */
	CHECK(wireless_level(" wlan0: 0000   70.  -80  -256", "wlan0", &dbm) == 0 && dbm == -80);  /* no dot, no newline */
	CHECK(wireless_level(" wlan0:", "wlan0", &dbm) == -1);
	CHECK(wireless_level("", "wlan0", &dbm) == -1);
	CHECK(signal_bars(-40) == 4 && signal_bars(-55) == 4 && signal_bars(-56) == 3 && signal_bars(-65) == 3 && signal_bars(-66) == 2 &&
	      signal_bars(-75) == 2 && signal_bars(-76) == 1 && signal_bars(-95) == 1);

	/* minute boundary */
	CHECK(ms_to_next_minute(0, 0) == 60030);
	CHECK(ms_to_next_minute(59, 0) == 1030);
	CHECK(ms_to_next_minute(59, 999) == 31);
	CHECK(ms_to_next_minute(1700000010LL, 250) == (60 - 1700000010LL % 60) * 1000 - 250 + 30);
	CHECK(ms_to_next_minute(61, 500) == 58530);

	/* clock text */
	memset(&tm, 0, sizeof tm);
	tm.tm_year = 2026 - 1900; tm.tm_hour = 15; tm.tm_min = 7;
	clock_text(&tm, 0, b, sizeof b); CHECK(!strcmp(b, "3:07 PM"));
	clock_text(&tm, 1, b, sizeof b); CHECK(!strcmp(b, "15:07"));
	tm.tm_hour = 0; clock_text(&tm, 0, b, sizeof b); CHECK(!strcmp(b, "12:07 AM"));
	tm.tm_hour = 12; clock_text(&tm, 0, b, sizeof b); CHECK(!strcmp(b, "12:07 PM"));
	tm.tm_hour = 9; tm.tm_min = 5; clock_text(&tm, 0, b, sizeof b); CHECK(!strcmp(b, "9:05 AM"));
	tm.tm_hour = 9; clock_text(&tm, 1, b, sizeof b); CHECK(!strcmp(b, "09:05"));
	tm.tm_year = 1970 - 1900; clock_text(&tm, 0, b, sizeof b); CHECK(!strcmp(b, "--:--"));
	tm.tm_year = 2023 - 1900; clock_text(&tm, 1, b, sizeof b); CHECK(!strcmp(b, "--:--"));

	/* TZ.CFG */
	h24 = 0;
	CHECK(tzcfg_parse("AST4\n", tz, sizeof tz, &h24) == 0 && !strcmp(tz, "AST4") && h24 == 0);
	CHECK(tzcfg_parse("# my zone\r\n\r\n  EST5EDT,M3.2.0,M11.1.0   24  \r\nignored\n", tz, sizeof tz, &h24) == 0 &&
	      !strcmp(tz, "EST5EDT,M3.2.0,M11.1.0") && h24 == 1);
	CHECK(tzcfg_parse("AST4 12\n", tz, sizeof tz, &h24) == 0 && h24 == 0);
	CHECK(tzcfg_parse("<-03>3 24", tz, sizeof tz, &h24) == 0 && !strcmp(tz, "<-03>3") && h24 == 1);
	h24 = 1;
	CHECK(tzcfg_parse("CET-1CEST\n", tz, sizeof tz, &h24) == 0 && h24 == 1);          /* no token: unchanged */
	CHECK(tzcfg_parse("AST4 bogus\n", tz, sizeof tz, &h24) == 0 && h24 == 1);
	CHECK(tzcfg_parse("", tz, sizeof tz, &h24) == -1);
	CHECK(tzcfg_parse("# only a comment\n\n", tz, sizeof tz, &h24) == -1);
	CHECK(tzcfg_parse("24\n", tz, sizeof tz, &h24) == -1);
	CHECK(tzcfg_parse("AST\x01" "4\n", tz, sizeof tz, &h24) == -1);
	CHECK(tzcfg_parse("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA4\n", tz, sizeof tz, &h24) == -1);
}

int main(int argc, char **argv)
{
	char t[] = "/tmp/olpcmenu-test-XXXXXX", cmd[1100];

	if (argc == 5 && !strcmp(argv[1], "--mkcover")) return mkcover(argv[2], atoi(argv[3]), atoi(argv[4]));
	if (!mkdtemp(t)) return 1;
	printf("== menulib\n");
	test_discovery(t);
	test_sort();
	test_repeat();
	test_state(t);
	test_text();
	test_cover(t);
	test_indicators();
	printf("%d checks, %d failed\n", checks, fails);
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", t);
	if (system(cmd)) {}
	return fails != 0;
}
