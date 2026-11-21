/*
 * olpc-covers-match -- map ROM names to entries of a libretro-thumbnails
 * Apache directory index (Named_Boxarts/).
 *
 *   olpc-covers-match index.html "ROM name"   print the best entry (no .png), or nothing
 *   olpc-covers-match -b index.html < names   one ROM name per line; prints
 *                                             rom TAB entry TAB href  (entry/href empty on a miss)
 *
 *   olpc-covers-match --tree2html < tree.json   GitHub git-tree JSON -> index form
 *   olpc-covers-match --tree-sha DIR < tree.json   sha of the tree entry DIR
 *
 * Exit status: 0 match found (single mode) / ok (batch), 1 no match, 2 error.
 *
 * The index is streamed line by line; only the ROM names are kept in memory
 * (plus the best candidate per ROM), so the 4 MB NES index costs a few KB.
 *
 * Rules, in order; the first rule that finds anything wins, a later entry only
 * replaces an earlier one of the same rule with a strictly better score:
 *   1  exact, after replacing the characters  & * / : ` " < > ? \ |  by '_'
 *   2  exact once revision/version/beta/proto tags are dropped from both sides
 *      (Rev A, Rev 1, v1.1, Version x, Beta, Proto, Sample)
 *   3  same base title (text before the first " (") -- best region score
 *   4  same base title after normalisation (case, punctuation, spaces and a
 *      leading article removed, ", The" moved to the front) -- best region score
 * There is no fuzzy matching: every rule is an equality test.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAXN 1024

struct rom {
	char *name;          /* as given, extension removed */
	char sub[MAXN];      /* after character substitution */
	char strip[MAXN];    /* tags removed */
	char base[MAXN];
	char norm[MAXN];
	char grp[MAXN];      /* first parenthetical group */
	int rule, score;
	char best[MAXN], href[MAXN * 3];
};

struct ent {
	char name[MAXN], strip[MAXN], base[MAXN], norm[MAXN], grp[MAXN];
	int ngroups, nremoved, beta;
};

static void subst(char *s)
{
	for (; *s; s++)
		if (strchr("&*/:`\"<>?\\|", *s))
			*s = '_';
}

static int startswith(const char *s, const char *p)
{
	return strncmp(s, p, strlen(p)) == 0;
}

/* Contents of a "(...)" group: is it a revision-like tag? *beta set for Beta/Proto/Sample. */
static int revtag(const char *c, int *beta)
{
	*beta = 0;
	if (startswith(c, "Beta") || startswith(c, "Proto") || startswith(c, "Sample")) {
		*beta = 1;
		return 1;
	}
	if (startswith(c, "Rev ") || startswith(c, "Version ")) return 1;
	if (c[0] == 'v' && isdigit((unsigned char)c[1])) return 1;
	return 0;
}

/* Copy s to out without revision tags (and the space before each one). */
static void strip_tags(const char *s, char *out, int *nremoved, int *beta, int *ngroups)
{
	size_t o = 0;
	*nremoved = 0; *beta = 0; *ngroups = 0;
	while (*s && o < MAXN - 1) {
		if (*s == '(') {
			const char *e = strchr(s, ')');
			if (e) {
				char tag[MAXN];
				size_t n = (size_t)(e - s - 1);
				int b;
				if (n >= MAXN) n = MAXN - 1;
				memcpy(tag, s + 1, n);
				tag[n] = 0;
				(*ngroups)++;
				if (revtag(tag, &b)) {
					(*nremoved)++;
					if (b) *beta = 1;
					if (o && out[o - 1] == ' ') o--;
					s = e + 1;
					continue;
				}
			}
		}
		out[o++] = *s++;
	}
	out[o] = 0;
}

static void get_base(const char *s, char *out)
{
	const char *p = strstr(s, " (");
	size_t n = p ? (size_t)(p - s) : strlen(s);
	if (n >= MAXN) n = MAXN - 1;
	memcpy(out, s, n);
	out[n] = 0;
}

static void get_grp(const char *s, char *out)
{
	const char *p = strstr(s, " (");
	out[0] = 0;
	if (p) {
		const char *e = strchr(p + 2, ')');
		size_t n = e ? (size_t)(e - p - 2) : strlen(p + 2);
		if (n >= MAXN) n = MAXN - 1;
		memcpy(out, p + 2, n);
		out[n] = 0;
	}
}

/* ", The" / ", A" / ", An" (before end or a space) moves to the front. */
static void normalise(const char *base, char *out)
{
	static const char *art[] = { ", The", ", An", ", A" };
	char tmp[MAXN * 2], *p;
	size_t i, o = 0;
	int k;

	strncpy(tmp, base, MAXN - 1);
	tmp[MAXN - 1] = 0;
	for (k = 0; k < 3; k++) {
		size_t al = strlen(art[k]);
		p = strstr(tmp, art[k]);
		if (p && (p[al] == 0 || p[al] == ' ')) {
			char head[MAXN * 2];
			size_t pre = (size_t)(p - tmp);
			snprintf(head, sizeof head, "%s %.*s%s", art[k] + 2, (int)pre, tmp, p + al);
			strcpy(tmp, head);
			break;
		}
	}
	/* drop a leading article */
	if (strncasecmp(tmp, "the ", 4) == 0) p = tmp + 4;
	else if (strncasecmp(tmp, "an ", 3) == 0) p = tmp + 3;
	else if (strncasecmp(tmp, "a ", 2) == 0) p = tmp + 2;
	else p = tmp;
	for (i = 0; p[i] && o < MAXN - 1; i++) {
		unsigned char c = (unsigned char)p[i];
		if (c >= 128) out[o++] = (char)c;
		else if (isalnum(c)) out[o++] = (char)tolower(c);
	}
	out[o] = 0;
}

static int has_token(const char *grp, const char *tok)
{
	size_t tl = strlen(tok);
	const char *p = grp;
	while ((p = strstr(p, tok))) {
		int l = (p == grp) || p[-1] == ' ' || p[-1] == ',';
		int r = p[tl] == 0 || p[tl] == ',' || p[tl] == ' ';
		if (l && r) return 1;
		p += tl;
	}
	return 0;
}

/* How good is entry e's region for ROM r (higher is better). */
static int region_score(const struct rom *r, const struct ent *e)
{
	static const char *pref[] = { "World", "USA", "Europe", "Japan" };
	static const int val[] = { 80, 70, 60, 50 };
	int s = 10, i;
	char tok[MAXN], *t;

	if (r->grp[0] && strcmp(r->grp, e->grp) == 0) s = 100;
	else {
		int shared = 0;
		strcpy(tok, r->grp);
		for (t = strtok(tok, ","); t; t = strtok(NULL, ",")) {
			while (*t == ' ') t++;
			if (*t && has_token(e->grp, t)) shared = 1;
		}
		if (shared) s = 90;
		else
			for (i = 0; i < 4; i++)
				if (has_token(e->grp, pref[i])) { s = val[i]; break; }
	}
	if (e->beta) s -= 50;
	return s - e->ngroups;
}

static void offer(struct rom *r, int rule, int score, const struct ent *e, const char *href)
{
	if (r->rule && (rule > r->rule || (rule == r->rule && score <= r->score)))
		return;
	r->rule = rule;
	r->score = score;
	strcpy(r->best, e->name);
	strncpy(r->href, href, sizeof r->href - 1);
	r->href[sizeof r->href - 1] = 0;
}

static void entry(struct rom *roms, int n, const char *name, const char *href)
{
	struct ent e;
	int i, dummy;
	size_t l = strlen(name);

	if (l >= MAXN) return;
	strcpy(e.name, name);
	strip_tags(name, e.strip, &e.nremoved, &e.beta, &e.ngroups);
	get_base(name, e.base);
	get_grp(name, e.grp);
	normalise(e.base, e.norm);
	(void)dummy;
	for (i = 0; i < n; i++) {
		struct rom *r = &roms[i];
		if (r->rule == 1) continue;
		if (strcmp(e.name, r->sub) == 0) { offer(r, 1, 0, &e, href); continue; }
		if (strcmp(e.strip, r->strip) == 0)
			offer(r, 2, 100 - 10 * e.nremoved - (e.beta ? 50 : 0), &e, href);
		else if (r->rule >= 2 || !r->rule) {
			if (strcmp(e.base, r->base) == 0)
				offer(r, 3, region_score(r, &e), &e, href);
			else if (e.norm[0] && strcmp(e.norm, r->norm) == 0)
				offer(r, 4, region_score(r, &e), &e, href);
		}
	}
}

static int hexv(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Parse one index line: every href="....png" becomes an entry. */
static void parse_line(struct rom *roms, int n, char *line)
{
	char *p = line;
	while ((p = strstr(p, "href=\""))) {
		char *s = p + 6, *e = strchr(s, '"'), href[MAXN * 3], name[MAXN];
		size_t hl, i, o = 0;

		if (!e) break;
		p = e + 1;
		hl = (size_t)(e - s);
		if (hl < 5 || hl >= sizeof href || s[0] == '?' || s[0] == '/' ||
		    strncmp(s + hl - 4, ".png", 4) != 0)
			continue;
		/* HTML entities, keeping the %XX escapes: that is the usable URL path */
		for (i = 0; i < hl;) {
			if (s[i] == '&') {
				if (!strncmp(s + i, "&amp;", 5)) { href[o++] = '&'; i += 5; continue; }
				if (!strncmp(s + i, "&lt;", 4)) { href[o++] = '<'; i += 4; continue; }
				if (!strncmp(s + i, "&gt;", 4)) { href[o++] = '>'; i += 4; continue; }
				if (!strncmp(s + i, "&quot;", 6)) { href[o++] = '"'; i += 6; continue; }
				if (!strncmp(s + i, "&#39;", 5)) { href[o++] = '\''; i += 5; continue; }
			}
			href[o++] = s[i++];
		}
		href[o] = 0;
		/* percent-decode the name, drop ".png" */
		for (i = 0, o = 0; href[i] && o < MAXN - 1; i++) {
			if (href[i] == '%' && hexv(href[i + 1]) >= 0 && hexv(href[i + 2]) >= 0) {
				name[o++] = (char)(hexv(href[i + 1]) * 16 + hexv(href[i + 2]));
				i += 2;
			} else
				name[o++] = href[i];
		}
		name[o] = 0;
		if (o > 4) name[o - 4] = 0;
		entry(roms, n, name, href);
	}
}

static void rom_init(struct rom *r, const char *raw)
{
	static const char *ext[] = { ".zip", ".gb", ".gbc", ".nes", ".7z" };
	char tmp[MAXN];
	size_t l, k;
	int d1, d2, d3;

	memset(r, 0, sizeof *r);
	strncpy(tmp, raw, MAXN - 1);
	tmp[MAXN - 1] = 0;
	l = strlen(tmp);
	while (l && (tmp[l - 1] == '\n' || tmp[l - 1] == '\r')) tmp[--l] = 0;
	for (k = 0; k < sizeof ext / sizeof *ext; k++) {
		size_t el = strlen(ext[k]);
		if (l > el && strcasecmp(tmp + l - el, ext[k]) == 0) { tmp[l - el] = 0; break; }
	}
	r->name = strdup(tmp);
	subst(tmp);
	strcpy(r->sub, tmp);
	strip_tags(tmp, r->strip, &d1, &d2, &d3);
	get_base(tmp, r->base);
	get_grp(tmp, r->grp);
	normalise(r->base, r->norm);
}

/* ---- GitHub tree JSON -> Apache-like index (streamed, constant memory).
 * Byte-identical to the awk implementation it replaced (kept as the reference
 * in test.sh).  Only lines  <spaces>"path": "<value>"[,]  are looked at. */
static const char SAFE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._~()!,-";

static void put_pct(char **o, unsigned b)
{
	static const char hx[] = "0123456789ABCDEF";
	*(*o)++ = '%'; *(*o)++ = hx[(b >> 4) & 15]; *(*o)++ = hx[b & 15];
}

/* one literal character: safe set as is, other ASCII %XX, bytes >= 128 raw */
static void put_chr(char **o, unsigned char c)
{
	if (c && strchr(SAFE, c)) *(*o)++ = (char)c;
	else if (c < 128) put_pct(o, c);
	else *(*o)++ = (char)c;
}

static void put_utf8(char **o, unsigned long cp)
{
	if (cp == 0) return;		/* awk sprintf("%c", 0) is empty */
	else if (cp < 128) put_chr(o, (unsigned char)cp);
	else if (cp < 0x800) { put_pct(o, 0xC0 + cp / 64); put_pct(o, 0x80 + cp % 64); }
	else if (cp < 0x10000) {
		put_pct(o, 0xE0 + cp / 4096); put_pct(o, 0x80 + cp / 64 % 64); put_pct(o, 0x80 + cp % 64);
	} else {
		put_pct(o, 0xF0 + cp / 262144); put_pct(o, 0x80 + cp / 4096 % 64);
		put_pct(o, 0x80 + cp / 64 % 64); put_pct(o, 0x80 + cp % 64);
	}
}

/* 4 hex digits at s (n bytes available) -> value, or -1 */
static long hex4(const char *s, size_t n)
{
	long v = 0;
	int i;
	if (n < 4) return -1;
	for (i = 0; i < 4; i++) {
		int c = (unsigned char)s[i], d;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		else return -1;
		v = v * 16 + d;
	}
	return v;
}

/* decode JSON escapes of s[0..n) and percent-encode; o must hold 12*n+1 */
static size_t jenc(const char *s, size_t n, char *out)
{
	char *o = out;
	size_t i = 0;
	while (i < n) {
		unsigned char c = (unsigned char)s[i];
		long v, w;
		if (c != '\\') { put_chr(&o, c); i++; continue; }
		if (i + 1 < n && s[i + 1] == 'u' && (v = hex4(s + i + 2, n - i - 2)) >= 0) {
			i += 6;
			if (v >= 0xD800 && v < 0xDC00 && i + 1 < n && s[i] == '\\' && s[i + 1] == 'u' &&
			    (w = hex4(s + i + 2, n - i - 2)) >= 0xDC00 && w < 0xE000) {
				v = 0x10000 + (v - 0xD800) * 1024 + (w - 0xDC00); i += 6;
			}
			put_utf8(&o, (unsigned long)v);
			continue;
		}
		if (i + 1 >= n) break;		/* lone trailing backslash: dropped */
		c = (unsigned char)s[i + 1]; i += 2;
		switch (c) {
		case 'b': put_pct(&o, 8); break;
		case 'f': put_pct(&o, 12); break;
		case 'n': put_pct(&o, 10); break;
		case 'r': put_pct(&o, 13); break;
		case 't': put_pct(&o, 9); break;
		default: put_chr(&o, c);	/* \" \\ \/ and unknown escapes */
		}
	}
	*o = 0;
	return (size_t)(o - out);
}

/* start of the value of a  ^ *"path": "  line, or NULL */
static const char *path_value(const char *l)
{
	while (*l == ' ') l++;
	return startswith(l, "\"path\": \"") ? l + 9 : NULL;
}

static int tree2html(void)
{
	char *line = NULL, *out = NULL;
	size_t lcap = 0, ocap = 0, n = 0;
	ssize_t len;
	while ((len = getline(&line, &lcap, stdin)) > 0) {
		const char *v;
		const char *q, *e;
		size_t vl, ol;
		if (line[len - 1] == '\n') line[--len] = 0;
		if (!(v = path_value(line))) continue;
		vl = strlen(v);
		/* drop  ",?[ \r]*$  starting at the last quote */
		q = strrchr(v, '"');
		if (q) {
			e = q + 1;
			if (*e == ',') e++;
			while (*e == ' ' || *e == '\r') e++;
			if (!*e) vl = (size_t)(q - v);
		}
		if (ocap < vl * 12 + 1) {
			ocap = vl * 12 + 1;
			out = realloc(out, ocap);
			if (!out) return 2;
		}
		ol = jenc(v, vl, out);
		if (ol < 4 || strcmp(out + ol - 4, ".png")) continue;
		if (!n++) fputs("<table>\n", stdout);
		printf("<a href=\"%s\">x</a>\n", out);
	}
	if (n) fputs("</table>\n", stdout);
	return ferror(stdout) ? 2 : 0;
}

/* sha of the entry DIR: the first "sha" line after its "path" line */
static int tree_sha(const char *dir)
{
	char *line = NULL, *pat;
	size_t lcap = 0;
	int found = 0;
	if (asprintf(&pat, "\"path\": \"%s\"", dir) < 0) return 2;
	while (getline(&line, &lcap, stdin) > 0) {
		if (!found) { found = strstr(line, pat) != NULL; continue; }
		if (strstr(line, "\"sha\":")) {
			char *p = line;
			int k;
			for (k = 0; k < 3 && p; k++) { p = strchr(p, '"'); if (p) p++; }
			if (p) { char *e = strchr(p, '"'); if (e) *e = 0; else p[strcspn(p, "\n")] = 0; puts(p); }
			else puts("");
			return 0;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	int batch;
	const char *index;
	if (argc == 2 && !strcmp(argv[1], "--tree2html")) return tree2html();
	if (argc == 3 && !strcmp(argv[1], "--tree-sha")) return tree_sha(argv[2]);
	batch = argc > 1 && strcmp(argv[1], "-b") == 0;
	index = argc > 1 + batch ? argv[1 + batch] : NULL;
	struct rom *roms = NULL;
	int n = 0, cap = 0, i;
	char *line = NULL;
	size_t lcap = 0;
	FILE *f;

	if (!index || (!batch && argc != 3) || (batch && argc != 3)) {
		fprintf(stderr, "usage: olpc-covers-match index.html \"ROM name\"\n"
				"       olpc-covers-match -b index.html < names\n");
		return 2;
	}
	if (batch) {
		while (getline(&line, &lcap, stdin) > 0) {
			if (line[0] == '\n' || line[0] == '\r') continue;
			if (n == cap) {
				cap = cap ? cap * 2 : 64;
				roms = realloc(roms, (size_t)cap * sizeof *roms);
				if (!roms) return 2;
			}
			rom_init(&roms[n++], line);
		}
	} else {
		roms = malloc(sizeof *roms);
		if (!roms) return 2;
		rom_init(&roms[n++], argv[2]);
	}
	f = fopen(index, "r");
	if (!f) { perror(index); return 2; }
	while (getline(&line, &lcap, f) > 0)
		parse_line(roms, n, line);
	fclose(f);

	if (batch) {
		for (i = 0; i < n; i++)
			printf("%s\t%s\t%s\n", roms[i].name, roms[i].rule ? roms[i].best : "",
			       roms[i].rule ? roms[i].href : "");
		return 0;
	}
	if (!roms[0].rule) return 1;
	puts(roms[0].best);
	return 0;
}
