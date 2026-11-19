#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include <time.h>
#include "menulib.h"

const char *const SYS_DIR[NSYS] = { "gb", "gbc", "nes" };
const char *const SYS_TITLE[NSYS] = { "Game Boy", "Game Boy Color", "NES" };

/* ---------------------------------------------------------------- discovery */

static char *xstrdup(const char *s) { char *p = strdup(s); if (!p) abort(); return p; }

static int ext_ok(const char *f)
{
	const char *e = strrchr(f, '.');

	if (!e || e == f) return 0;
	return !strcasecmp(e, ".gb") || !strcasecmp(e, ".gbc") || !strcasecmp(e, ".nes") || !strcasecmp(e, ".zip");
}

static int cmp_rom(const void *a, const void *b)
{
	const struct rom *x = a, *y = b;
	int c = strcasecmp(x->name, y->name);

	if (!c) c = strcmp(x->name, y->name);
	if (!c) c = strcmp(x->path, y->path);
	return c;
}

static void add_rom(struct romlist *l, const char *romsdir, const char *sysdir, const char *file, off_t size)
{
	struct rom *r;
	char path[4400], *dot;

	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 64;
		l->v = realloc(l->v, (size_t)l->cap * sizeof *l->v);
		if (!l->v) abort();
	}
	snprintf(path, sizeof path, "%s/%s/%s", romsdir, sysdir, file);
	r = &l->v[l->n++];
	r->file = xstrdup(file);
	r->name = xstrdup(file);
	dot = strrchr(r->name, '.');
	if (dot) *dot = 0;
	r->path = xstrdup(path);
	r->romsdir = xstrdup(romsdir);
	r->sysdir = xstrdup(sysdir);
	r->size = size;
}

static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

static void scan_system(const char *romsdir, const char *sysdir, struct romlist *l)
{
	char dir[4400];
	DIR *d;
	struct dirent *e;

	snprintf(dir, sizeof dir, "%s/%s", romsdir, sysdir);
	d = opendir(dir);
	if (!d) return;
	while ((e = readdir(d))) {
		struct stat st;
		char p[4400];

		if (e->d_name[0] == '.' || !ext_ok(e->d_name)) continue;
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
		add_rom(l, romsdir, sysdir, e->d_name, st.st_size);
	}
	closedir(d);
}

static void scan_roms_dir(const char *romsdir, struct romlist lists[NSYS])
{
	DIR *d = opendir(romsdir);
	struct dirent *e;

	if (!d) return;
	while ((e = readdir(d))) {
		int i;
		for (i = 0; i < NSYS; i++)
			if (!strcasecmp(e->d_name, SYS_DIR[i])) {
				char p[4400];
				snprintf(p, sizeof p, "%s/%s", romsdir, e->d_name);
				if (is_dir(p)) scan_system(romsdir, e->d_name, &lists[i]);
			}
	}
	closedir(d);
}

/* look for roms/ directly inside dir */
static void scan_container(const char *dir, struct romlist lists[NSYS])
{
	DIR *d = opendir(dir);
	struct dirent *e;

	if (!d) return;
	while ((e = readdir(d))) {
		char p[4400];
		if (strcasecmp(e->d_name, "roms")) continue;
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		if (is_dir(p)) scan_roms_dir(p, lists);
	}
	closedir(d);
}

static void scan_base(const char *base, struct romlist lists[NSYS])
{
	DIR *d = opendir(base);
	struct dirent *e;
	int deep = strstr(base, "media") != NULL;      /* /media/<user>/<label> */

	if (!d) return;
	while ((e = readdir(d))) {
		char p[4400];
		if (e->d_name[0] == '.') continue;
		snprintf(p, sizeof p, "%s/%s", base, e->d_name);
		if (!is_dir(p)) continue;
		scan_container(p, lists);
		if (deep) {
			DIR *d2 = opendir(p);
			struct dirent *e2;
			if (!d2) continue;
			while ((e2 = readdir(d2))) {
				char q[4400];
				if (e2->d_name[0] == '.') continue;
				snprintf(q, sizeof q, "%s/%s", p, e2->d_name);
				if (is_dir(q)) scan_container(q, lists);
			}
			closedir(d2);
		}
	}
	closedir(d);
}

int rom_scan(const char *bases, struct romlist lists[NSYS])
{
	char *copy = xstrdup(bases), *save = NULL, *b;
	int i, total = 0;

	memset(lists, 0, NSYS * sizeof lists[0]);
	for (b = strtok_r(copy, ":", &save); b; b = strtok_r(NULL, ":", &save)) scan_base(b, lists);
	free(copy);
	for (i = 0; i < NSYS; i++) {
		if (lists[i].n > 1) qsort(lists[i].v, (size_t)lists[i].n, sizeof lists[i].v[0], cmp_rom);
		total += lists[i].n;
	}
	return total;
}

void rom_free(struct romlist lists[NSYS])
{
	int i, j;

	for (i = 0; i < NSYS; i++) {
		for (j = 0; j < lists[i].n; j++) {
			free(lists[i].v[j].name); free(lists[i].v[j].file); free(lists[i].v[j].path);
			free(lists[i].v[j].romsdir); free(lists[i].v[j].sysdir);
		}
		free(lists[i].v);
	}
	memset(lists, 0, NSYS * sizeof lists[0]);
}

int cover_path(const struct rom *r, char *out, size_t cap)
{
	char sys[32];
	size_t i;
	int n;

	for (i = 0; r->sysdir[i] && i < sizeof sys - 1; i++) sys[i] = (char)tolower((unsigned char)r->sysdir[i]);
	sys[i] = 0;
	n = snprintf(out, cap, "%s/../covers/%s/%s.obc", r->romsdir, sys, r->name);
	return n > 0 && (size_t)n < cap ? 0 : -1;
}

int cover_load(const char *path, uint16_t *dst, int *w, int *h)
{
	unsigned char hd[16];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	size_t want, got = 0;
	int cw, ch;

	if (fd < 0) return -1;
	if (read(fd, hd, 16) != 16 || memcmp(hd, "OBC1", 4)) { close(fd); return -1; }
	cw = hd[4] | hd[5] << 8;
	ch = hd[6] | hd[7] << 8;
	if (cw < 1 || ch < 1 || cw > COVER_MAX || ch > COVER_MAX) { close(fd); return -1; }
	want = (size_t)cw * (size_t)ch * 2;
	while (got < want) {
		ssize_t n = read(fd, (char *)dst + got, want - got);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) break;
		got += (size_t)n;
	}
	close(fd);
	if (got != want) return -1;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	{ size_t i; for (i = 0; i < want / 2; i++) dst[i] = (uint16_t)(dst[i] << 8 | dst[i] >> 8); }
#endif
	*w = cw; *h = ch;
	return 0;
}

/* ------------------------------------------------------------------- repeat */

void rep_start(struct rep *r, int dir, uint64_t now) { r->dir = dir; r->t0 = now; r->next = now + REP_DELAY_MS; }
void rep_stop(struct rep *r) { r->dir = 0; }

int rep_tick(struct rep *r, uint64_t now, int page)
{
	int steps = 0, k = 0;

	if (!r->dir || now < r->next) return 0;
	while (r->next <= now && k < 4) {
		if (r->next - r->t0 >= REP_PAGE_AFTER) { steps += page; r->next += REP_PAGE_MS; }
		else { steps += 1; r->next += REP_FAST_MS; }
		k++;
	}
	if (r->next <= now) r->next = now + 1;      /* far behind: drop the backlog */
	return steps * r->dir;
}

int rep_timeout(const struct rep *r, uint64_t now)
{
	if (!r->dir) return -1;
	return r->next > now ? (int)(r->next - now) : 0;
}

/* -------------------------------------------------------------------- state */

int state_write_opt(const char *path, int sys, const char *file, int overlay)
{
	char tmp[4200];
	FILE *f;

	if (sys < 0 || sys >= NSYS) return -1;
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	f = fopen(tmp, "w");
	if (!f) return -1;
	fprintf(f, "system=%s\nfile=%s\n", SYS_DIR[sys], file ? file : "");
	if (overlay == 0 || overlay == 1) fprintf(f, "overlay=%d\n", overlay);
	if (fclose(f) != 0) { unlink(tmp); return -1; }
	if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
	return 0;
}

int state_write(const char *path, int sys, const char *file) { return state_write_opt(path, sys, file, -1); }

int state_read_opt(const char *path, int *sys, char *file, size_t cap, int *overlay)
{
	char line[4200];
	FILE *f = fopen(path, "r");
	int got = 0, i;

	if (!f) return -1;
	file[0] = 0;
	*sys = -1;
	if (overlay) *overlay = -1;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "system=", 7)) {
			for (i = 0; i < NSYS; i++) if (!strcmp(line + 7, SYS_DIR[i])) { *sys = i; got |= 1; }
		} else if (!strncmp(line, "file=", 5)) {
			snprintf(file, cap, "%s", line + 5);
			got |= 2;
		} else if (overlay && (!strcmp(line, "overlay=0") || !strcmp(line, "overlay=1"))) {
			*overlay = line[8] - '0';
		}
	}
	fclose(f);
	return got == 3 ? 0 : -1;
}

int state_read(const char *path, int *sys, char *file, size_t cap) { return state_read_opt(path, sys, file, cap, NULL); }

/* --------------------------------------------------------------------- text */

static unsigned fold(unsigned cp, const char **ascii)
{
	*ascii = NULL;
	switch (cp) {
	case 0x2018: case 0x2019: case 0x2032: *ascii = "'"; break;
	case 0x201c: case 0x201d: *ascii = "\""; break;
	case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: *ascii = "-"; break;
	case 0x2026: *ascii = "..."; break;
	case 0x0152: *ascii = "OE"; break;
	case 0x0153: *ascii = "oe"; break;
	case 0x0160: *ascii = "S"; break;
	case 0x0161: *ascii = "s"; break;
	case 0x017d: *ascii = "Z"; break;
	case 0x017e: *ascii = "z"; break;
	case 0x20ac: *ascii = "E"; break;
	default: *ascii = "?"; break;
	}
	return 0;
}

size_t utf8_to_l1(const char *s, unsigned char *out, size_t cap)
{
	const unsigned char *p = (const unsigned char *)s;
	size_t n = 0;

	while (*p && n + 4 < cap) {
		unsigned c = *p, cp;
		int len = 0;

		if (c < 0x80) { out[n++] = (unsigned char)c; p++; continue; }
		if (c >= 0xc2 && c <= 0xdf) { len = 1; cp = c & 0x1f; }
		else if (c >= 0xe0 && c <= 0xef) { len = 2; cp = c & 0x0f; }
		else if (c >= 0xf0 && c <= 0xf4) { len = 3; cp = c & 0x07; }
		else { out[n++] = (unsigned char)c; p++; continue; }     /* lone high byte: Latin-1 */
		{
			int i, ok = 1;
			for (i = 1; i <= len; i++) {
				if ((p[i] & 0xc0) != 0x80) { ok = 0; break; }
				cp = cp << 6 | (p[i] & 0x3f);
			}
			if (!ok) { out[n++] = (unsigned char)c; p++; continue; }
		}
		p += len + 1;
		if (cp <= 0xff) out[n++] = (unsigned char)cp;
		else {
			const char *a, *q;
			fold(cp, &a);
			for (q = a; *q; q++) out[n++] = (unsigned char)*q;
		}
	}
	out[n] = 0;
	return n;
}

void fit_text(unsigned char *s, int maxch)
{
	size_t n = strlen((char *)s);

	if ((int)n <= maxch) return;
	if (maxch < 4) { s[maxch < 0 ? 0 : maxch] = 0; return; }
	memcpy(s + maxch - 3, "...", 4);
}

int wrap_text(const unsigned char *s, int maxch, char out[][96], int maxlines)
{
	size_t len = strlen((const char *)s), pos = 0;
	int nl = 0;

	if (maxch > 90) maxch = 90;
	while (pos < len && nl < maxlines) {
		size_t rem = len - pos, take;
		while (pos < len && s[pos] == ' ') pos++;
		rem = len - pos;
		if (!rem) break;
		if (rem <= (size_t)maxch) take = rem;
		else {
			size_t i = (size_t)maxch;      /* last break opportunity within the width */
			take = 0;
			for (; i > 0; i--) {
				if (s[pos + i] == ' ') { take = i; break; }
				if (i < (size_t)maxch && (s[pos + i - 1] == '-' || s[pos + i - 1] == ',')) { take = i; break; }
			}
			if (!take) take = (size_t)maxch;
		}
		memcpy(out[nl], s + pos, take);
		out[nl][take] = 0;
		while (take && out[nl][take - 1] == ' ') out[nl][--take] = 0;
		pos += take;
		nl++;
	}
	while (pos < len && s[pos] == ' ') pos++;
	if (pos < len && nl > 0) {                        /* more text than lines: cut the last one */
		char tmp[96];
		snprintf(tmp, sizeof tmp, "%s", out[nl - 1]);
		if ((int)strlen(tmp) > maxch - 3) tmp[maxch - 3] = 0;
		snprintf(out[nl - 1], 96, "%.90s...", tmp);
	}
	return nl;
}
