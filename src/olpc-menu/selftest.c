/* olpc-menu unit tests (host, no hardware).  Also: selftest --mkcover FILE W H writes a test cover. */
#define _GNU_SOURCE
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
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
	printf("%d checks, %d failed\n", checks, fails);
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", t);
	if (system(cmd)) {}
	return fails != 0;
}
