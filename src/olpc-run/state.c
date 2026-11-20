/* Save states, see state.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "state.h"
#include "saver.h"
#include "../common/status.h"

void st_path(char *out, size_t cap, const char *dir, const char *base, int slot)
{
	snprintf(out, cap, "%s/%s.st%d", dir, base, slot);
}

int st_save(const struct core_ser *c, const char *dir, const char *path)
{
	size_t n = c->size ? c->size() : 0;
	uint8_t *b;
	int r;

	if (!n || !c->ser) return ST_UNSUPPORTED;
	if (mkdir(dir, 0755) < 0 && errno != EEXIST) return ST_IO;
	b = malloc(n);
	if (!b) return ST_IO;
	r = c->ser(b, n) && sv_write_atomic(path, b, n) == 0 ? ST_OK : ST_IO;
	free(b);
	return r;
}

int st_load(const struct core_ser *c, const char *path)
{
	size_t want = c->size ? c->size() : 0, got;
	uint8_t *b;
	FILE *f;
	int r;

	if (!want || !c->unser) return ST_UNSUPPORTED;
	f = fopen(path, "rb");
	if (!f) return errno == ENOENT ? ST_EMPTY : ST_IO;
	b = malloc(want + 1);
	if (!b) { fclose(f); return ST_IO; }
	got = fread(b, 1, want + 1, f);                  /* one byte more than wanted: a longer file is refused */
	fclose(f);
	if (got != want) r = ST_BADSIZE;
	else r = c->unser(b, want) ? ST_OK : ST_REFUSED;
	free(b);
	return r;
}

time_t st_stamp(const char *path)
{
	struct stat s;

	return stat(path, &s) == 0 ? s.st_mtim.tv_sec : (time_t)-1;
}

void st_when(char *out, size_t cap, time_t t, int h24)
{
	struct tm tm;
	char clk[16], day[16];

	localtime_r(&t, &tm);
	clock_text(&tm, h24, clk, sizeof clk);
	strftime(day, sizeof day, "%b %d", &tm);
	snprintf(out, cap, "%s %s", clk, day);
}

int st_slot_read(const char *dir, const char *base)
{
	char p[PATH_MAX];
	FILE *f;
	int s = 1;

	snprintf(p, sizeof p, "%s/%s.slot", dir, base);
	f = fopen(p, "r");
	if (f) { if (fscanf(f, "%d", &s) != 1) s = 1; fclose(f); }
	return s >= 1 && s <= ST_SLOTS ? s : 1;
}

void st_slot_write(const char *dir, const char *base, int slot)
{
	char p[PATH_MAX], v[4];

	if (mkdir(dir, 0755) < 0 && errno != EEXIST) return;
	snprintf(p, sizeof p, "%s/%s.slot", dir, base);
	snprintf(v, sizeof v, "%d\n", slot);
	sv_write_atomic(p, (const uint8_t *)v, strlen(v));
}
