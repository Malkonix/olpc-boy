/* fakenand: a file-backed fake NAND that stands in for nanddump / nandwrite / flash_erase in
 * nandtest/test.sh (invoked through links named like the real tools).  Test code only.
 *
 * Backing file FAKE_NAND: raw pages, (WRITE+OOB) bytes each, in device order.
 * Env: FAKE_ERASE FAKE_WRITE FAKE_OOB (geometry)  FAKE_PRE FAKE_TEST (partition sizes in bytes;
 *      mtd0 = [0,PRE) read-only, mtd1 = [PRE,PRE+TEST) writeable, mtd2 = rest, read-only;
 *      "mtdall" = everything, read-only)  FAKE_BAD="n n n" (bad blocks: skipped by nandwrite and
 *      flash_erase, still readable)  FAKE_ECC_OFF (3 ECC bytes per 512 data bytes, Hamming-like)
 *      FAKE_CM_OFF (cleanmarker offset in the OOB)  FAKE_SYS (fake sysfs dir; counters updated)
 *      FAKE_LOG (one line per write/erase call)  FAKE_STORE (removed by every erase)
 * Fault injection: FAKE_FAIL_WRITE=n (nth nandwrite call fails)  FAKE_FAIL_ERASE=n  FAKE_WRITE_BAD=1
 *      (stored data gets 2 flipped bits in one 512-byte chunk: uncorrectable)  FAKE_NO_CM=1 (erase -j
 *      writes no cleanmarker)  FAKE_COUNT (file holding the call counters)
 * Extra mode: fakenand mkimage -- creates FAKE_NAND, see main().  */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>

static unsigned ERASE = 131072, WRITE = 2048, OOB = 64, ECC_OFF = 0, CM_OFF = 14;
static uint64_t PRE, TEST, TOTAL;
static FILE *nand;
static unsigned char *page;          /* WRITE + OOB */

static uint64_t envu(const char *n, uint64_t d) { const char *v = getenv(n); return v && *v ? strtoull(v, NULL, 0) : d; }
static int is_bad(uint64_t blk) {
	const char *v = getenv("FAKE_BAD"); char *c, *s;
	if (!v) return 0;
	c = strdup(v);
	for (s = strtok(c, " "); s; s = strtok(NULL, " ")) if ((uint64_t)atoll(s) == blk) { free(c); return 1; }
	free(c); return 0;
}
static void logline(const char *fmt, const char *dev, uint64_t a) {
	const char *l = getenv("FAKE_LOG"); FILE *f;
	if (!l || !(f = fopen(l, "a"))) return;
	fprintf(f, fmt, dev, (unsigned long long)a); fputc('\n', f); fclose(f);
}
static int callno(const char *tool) {   /* persistent per-tool call counter */
	const char *c = getenv("FAKE_COUNT"); char path[512]; FILE *f; int n = 0;
	if (!c) return 0;
	snprintf(path, sizeof path, "%s.%s", c, tool);
	if ((f = fopen(path, "r"))) { if (fscanf(f, "%d", &n) != 1) n = 0; fclose(f); }
	n++;
	if ((f = fopen(path, "w"))) { fprintf(f, "%d\n", n); fclose(f); }
	return n;
}

/* ---- ECC: per 512-byte chunk: xor of all bytes, 9 row parities ------------------------------ */
static void ecc_calc(const unsigned char *d, unsigned char *e) {
	unsigned x = 0, p = 0; int i, j;
	for (j = 0; j < 512; j++) x ^= d[j];
	for (i = 0; i < 9; i++) { unsigned t = 0; for (j = 0; j < 512; j++) if ((j >> i) & 1) t ^= d[j]; p |= (__builtin_popcount(t) & 1) << i; }
	e[0] = x; e[1] = p & 0xff; e[2] = p >> 8;
}
static void ecc_fill(unsigned char *pg) { unsigned c; for (c = 0; c < WRITE / 512; c++) ecc_calc(pg + c * 512, pg + WRITE + ECC_OFF + c * 3); }
/* returns corrected bits, or -1 if uncorrectable; page erased (ECC bytes FF) -> 0 */
static int ecc_check(unsigned char *pg) {
	unsigned c; int fixed = 0;
	for (c = 0; c < WRITE / 512; c++) {
		unsigned char *e = pg + WRITE + ECC_OFF + c * 3, n[3];
		unsigned dx, dp;
		if (e[0] == 0xff && e[1] == 0xff && e[2] == 0xff) continue;
		ecc_calc(pg + c * 512, n);
		dx = e[0] ^ n[0]; dp = (e[1] | e[2] << 8) ^ (n[1] | n[2] << 8);
		if (!dx && !dp) continue;
		if (__builtin_popcount(dx) == 1 && dp < 512) { pg[c * 512 + dp] ^= dx; fixed++; }
		else return -1;
	}
	return fixed;
}

static void sysbump(const char *dev, const char *attr, int by) {
	const char *s = getenv("FAKE_SYS"); char path[512]; FILE *f; long v = 0;
	if (!s || !by) return;
	snprintf(path, sizeof path, "%s/%s/%s", s, dev, attr);
	if ((f = fopen(path, "r"))) { if (fscanf(f, "%ld", &v) != 1) v = 0; fclose(f); }
	if ((f = fopen(path, "w"))) { fprintf(f, "%ld\n", v + by); fclose(f); }
}

static int part(const char *dev, uint64_t *off, uint64_t *len, int *rw) {
	const char *b = strrchr(dev, '/'); b = b ? b + 1 : dev;
	if (!strcmp(b, "mtd0")) { *off = 0; *len = PRE; *rw = 0; }
	else if (!strcmp(b, "mtd1")) { *off = PRE; *len = TEST; *rw = 1; }
	else if (!strcmp(b, "mtd2")) { *off = PRE + TEST; *len = TOTAL - PRE - TEST; *rw = 0; }
	else if (!strcmp(b, "mtdall")) { *off = 0; *len = TOTAL; *rw = 0; }
	else return -1;
	return 0;
}
static void rd(uint64_t pg) { fseeko(nand, (off_t)(pg * (WRITE + OOB)), SEEK_SET); if (fread(page, 1, WRITE + OOB, nand) != WRITE + OOB) { fprintf(stderr, "fakenand: read past end\n"); exit(1); } }
static void wr(uint64_t pg, const unsigned char *p) { fseeko(nand, (off_t)(pg * (WRITE + OOB)), SEEK_SET); fwrite(p, 1, WRITE + OOB, nand); }

static int do_dump(int argc, char **argv, const char *dev, uint64_t off, uint64_t len) {
	int oob = 0, noecc = 0, quiet = 0, i; uint64_t start = 0, pg, n;
	unsigned long long failed = 0;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--oob") || !strcmp(argv[i], "-o")) oob = 1;
		else if (!strcmp(argv[i], "--noecc") || !strcmp(argv[i], "-n")) noecc = 1;
		else if (!strcmp(argv[i], "-q")) quiet = 1;
		else if (!strcmp(argv[i], "-l") && i + 1 < argc) len = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "-s") && i + 1 < argc) start = strtoull(argv[++i], NULL, 0);
		else if (!strncmp(argv[i], "--bb=", 5)) ;
		else if (argv[i][0] == '-') { fprintf(stderr, "fakenand nanddump: unsupported option %s\n", argv[i]); return 2; }
	}
	(void)dev; (void)quiet;
	n = len / WRITE;
	for (pg = 0; pg < n; pg++) {
		uint64_t abs = off / WRITE + start / WRITE + pg;
		int r = 0;
		rd(abs);
		if (!noecc) {
			r = ecc_check(page);
			if (r < 0) { fprintf(stderr, "ECC: 1 uncorrectable bitflip(s) at offset 0x%08llx\n", (unsigned long long)(abs * WRITE - off)); failed++; }
			else if (r > 0) { fprintf(stderr, "ECC: %d corrected bitflip(s) at offset 0x%08llx\n", r, (unsigned long long)(abs * WRITE - off)); sysbump(strrchr(dev, '/') ? strrchr(dev, '/') + 1 : dev, "corrected_bits", r); }
		}
		fwrite(page, 1, WRITE, stdout);
		if (oob) fwrite(page + WRITE, 1, OOB, stdout);
	}
	if (failed) sysbump(strrchr(dev, '/') ? strrchr(dev, '/') + 1 : dev, "ecc_failures", (int)failed);
	if (fflush(stdout)) return 1;
	return 0;
}

static int do_write(int argc, char **argv, const char *dev, uint64_t off, uint64_t len, int rw) {
	int pad = 0, noecc = 0, oob = 0, i, call = callno("nandwrite");
	uint64_t start = 0, pos, nblk = ERASE / WRITE;
	const char *file = NULL; FILE *in; unsigned char *buf; size_t isz = WRITE + (oob ? OOB : 0);
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-p")) pad = 1;
		else if (!strcmp(argv[i], "-n")) noecc = 1;
		else if (!strcmp(argv[i], "-o")) oob = 1;
		else if (!strcmp(argv[i], "-q")) ;
		else if (!strcmp(argv[i], "-s") && i + 1 < argc) start = strtoull(argv[++i], NULL, 0);
		else if (argv[i][0] == '-') { fprintf(stderr, "fakenand nandwrite: unsupported option %s\n", argv[i]); return 2; }
		else if (i == argc - 1) file = argv[i];
	}
	isz = WRITE + (oob ? OOB : 0);
	logline("nandwrite %s start=%llu", dev, start);
	if (!rw) { fprintf(stderr, "nandwrite: %s: Operation not permitted\n", dev); return 1; }
	if (envu("FAKE_FAIL_WRITE", 0) == (uint64_t)call && call) { fprintf(stderr, "nandwrite: Input/output error (injected)\n"); return 1; }
	if (!file || !(in = fopen(file, "rb"))) { fprintf(stderr, "nandwrite: cannot open input\n"); return 1; }
	buf = malloc(isz);
	pos = start / WRITE;
	for (;;) {
		size_t got = fread(buf, 1, isz, in);
		uint64_t abs, blk; unsigned char *np; unsigned k;
		if (got == 0) break;
		if (got < isz) { if (!pad) { fprintf(stderr, "nandwrite: short input page\n"); return 1; } memset(buf + got, 0xff, isz - got); }
		while (is_bad((off / WRITE + pos) / nblk)) pos = (pos / nblk + 1) * nblk;     /* skip bad blocks */
		if ((pos + 1) * WRITE > len) { fprintf(stderr, "nandwrite: input exceeds the device\n"); return 1; }
		abs = off / WRITE + pos; blk = abs / nblk; (void)blk;
		rd(abs);
		np = malloc(WRITE + OOB);
		memcpy(np, buf, WRITE);
		memset(np + WRITE, 0xff, OOB);
		if (oob) memcpy(np + WRITE, buf + WRITE, OOB);
		if (!noecc) ecc_fill(np);
		if (getenv("FAKE_WRITE_BAD") && !noecc) { np[10] ^= 0x01; np[20] ^= 0x01; }     /* two flipped bits in chunk 0, after ECC */
		for (k = 0; k < WRITE + OOB; k++) np[k] &= page[k];                            /* programming only clears bits */
		wr(abs, np); free(np);
		pos++;
	}
	free(buf); fclose(in);
	return fflush(nand) ? 1 : 0;
}

static int do_erase(int argc, char **argv, const char *dev, uint64_t off, uint64_t len, int rw) {
	int jffs2 = 0, i, call = callno("flash_erase");
	uint64_t b, nblk = ERASE / WRITE, first = off / ERASE, cnt = len / ERASE;
	unsigned char *ff = malloc(WRITE + OOB); const char *st;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-j")) jffs2 = 1;
		else if (!strcmp(argv[i], "-q")) ;
		else if (argv[i][0] == '-') { fprintf(stderr, "fakenand flash_erase: unsupported option %s\n", argv[i]); return 2; }
	}
	logline(jffs2 ? "flash_erase -j %s %llu" : "flash_erase %s %llu", dev, 0);
	if (!rw) { fprintf(stderr, "flash_erase: %s: Operation not permitted\n", dev); return 1; }
	if (envu("FAKE_FAIL_ERASE", 0) == (uint64_t)call && call) { fprintf(stderr, "flash_erase: Input/output error (injected)\n"); return 1; }
	if ((st = getenv("FAKE_STORE"))) unlink(st);
	for (b = first; b < first + cnt; b++) {
		uint64_t p;
		if (is_bad(b)) continue;
		for (p = 0; p < nblk; p++) {
			memset(ff, 0xff, WRITE + OOB);
			if (jffs2 && p == 0 && !getenv("FAKE_NO_CM")) memcpy(ff + WRITE + CM_OFF, "\x85\x19\x03\x20\x08\x00\x00\x00", 8);
			wr(b * nblk + p, ff);
		}
	}
	free(ff);
	return fflush(nand) ? 1 : 0;
}

/* mkimage: FAKE_NAND is created with random data and consistent ECC everywhere except the window
 * blocks FIRST..FIRST+COUNT-1 (erased, cleanmarkers); bad blocks get random data and no ECC.
 *   fakenand mkimage SEED FIRST COUNT */
static int do_mkimage(char **argv) {
	uint64_t seed = strtoull(argv[2], NULL, 0), first = strtoull(argv[3], NULL, 0), cnt = strtoull(argv[4], NULL, 0), p, nblk = ERASE / WRITE;
	unsigned char *pg = malloc(WRITE + OOB); unsigned k;
	uint64_t x = seed * 0x9E3779B97F4A7C15ULL + 1;
	for (p = 0; p < TOTAL / WRITE; p++) {
		uint64_t b = p / nblk;
		memset(pg, 0xff, WRITE + OOB);
		if (b >= first && b < first + cnt) { if (p % nblk == 0) memcpy(pg + WRITE + CM_OFF, "\x85\x19\x03\x20\x08\x00\x00\x00", 8); }
		else {
			for (k = 0; k < WRITE + OOB; k++) { x ^= x >> 12; x ^= x << 25; x ^= x >> 27; pg[k] = (x * 0x2545F4914F6CDD1DULL) >> 56; }
			if (!is_bad(b)) { for (k = 0; k < OOB; k++) pg[WRITE + k] = (pg[WRITE + k] & 0x0f) | 0xf0; ecc_fill(pg); }
			else pg[WRITE + ECC_OFF] = 0x12;
		}
		wr(p, pg);
	}
	return fflush(nand) ? 1 : 0;
}

int main(int argc, char **argv) {
	char *tool = basename(argv[0]);
	const char *path = getenv("FAKE_NAND");
	uint64_t off, len; int rw, rc;
	ERASE = envu("FAKE_ERASE", ERASE); WRITE = envu("FAKE_WRITE", WRITE); OOB = envu("FAKE_OOB", OOB);
	ECC_OFF = envu("FAKE_ECC_OFF", 0); CM_OFF = envu("FAKE_CM_OFF", 14);
	PRE = envu("FAKE_PRE", 0); TEST = envu("FAKE_TEST", 0); TOTAL = envu("FAKE_SIZE", 0);
	if (!path) { fprintf(stderr, "fakenand: FAKE_NAND not set\n"); return 2; }
	page = malloc(WRITE + OOB);
	if (!strcmp(tool, "fakenand") && argc >= 5 && !strcmp(argv[1], "mkimage")) {
		if (!(nand = fopen(path, "wb"))) { perror("fakenand"); return 1; }
		return do_mkimage(argv);
	}
	if (!(nand = fopen(path, "r+b"))) { perror("fakenand"); return 1; }
	if (argc < 2) return 2;
	if (part(argv[argc - 1], &off, &len, &rw)) {            /* nandwrite has the file last: find the device */
		int i; for (i = argc - 2; i > 0 && part(argv[i], &off, &len, &rw); i--) ;
		if (i == 0) { fprintf(stderr, "fakenand: no known device in the arguments\n"); return 2; }
	}
	{ int i; const char *dev = argv[argc - 1];
	  for (i = argc - 1; i > 0; i--) { uint64_t o2, l2; int r2; if (!part(argv[i], &o2, &l2, &r2)) { dev = argv[i]; break; } }
	  if (!strcmp(tool, "nanddump")) rc = do_dump(argc, argv, dev, off, len);
	  else if (!strcmp(tool, "nandwrite")) rc = do_write(argc, argv, dev, off, len, rw);
	  else if (!strcmp(tool, "flash_erase")) rc = do_erase(argc, argv, dev, off, len, rw);
	  else { fprintf(stderr, "fakenand: unknown tool %s\n", tool); rc = 2; } }
	fclose(nand);
	return rc;
}
