/* nbtool: small helpers for olpc-nandtest.  No SSE, no libs.
 *   nbtool gen SEED BYTES            deterministic pseudo-random stream on stdout
 *   nbtool fill HEXBYTE BYTES        BYTES copies of one byte on stdout
 *   nbtool flip FILE OFFSET BIT      flip one bit of FILE in place
 *   nbtool ff                        stdin: count bytes != 0xFF -> "NONFF FIRST_OFFSET TOTAL"
 *   nbtool oobx FILE FIRSTPAGE NPAGES [W O]   the OOB bytes of NPAGES raw pages of FILE (page = W+O bytes)
 *   nbtool oob [-w W] [-o O] [-x EXPOOB] [-r REPORT] [-E OFF LEN] [-K OFF LEN]
 *        stdin = raw pages (W data bytes + O oob bytes, no ECC), as `nanddump --oob --noecc`.
 *        Prints "PAGES DATA_NONFF OOB_DIFF_BYTES OOB_DIFF_BITS OOB_DIFF_PAGES ECC_FF_PAGES CRIT_DIFF_BYTES".
 *        DATA_NONFF: data-area bytes that are not 0xFF.  With -x, the OOB of page N is compared with
 *        bytes N*O.. of EXPOOB; the first 40 differences go to REPORT as "page P oob+K got XX exp YY".
 *        ECC_FF_PAGES: pages whose OOB bytes OFF..OFF+LEN-1 are all 0xFF (no ECC stored).
 *        CRIT_DIFF_BYTES: differing OOB bytes that fall inside the -K range (ECC + cleanmarker area).
 */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

static int cmd_gen(const char *seed, const char *bytes)
{
	uint64_t x = strtoull(seed, NULL, 0) * 0x9E3779B97F4A7C15ULL + 0x1234567ULL, n = strtoull(bytes, NULL, 0);
	static unsigned char buf[65536];
	if (!x) x = 1;
	while (n) {
		size_t k = n < sizeof buf ? n : sizeof buf, i;
		for (i = 0; i < k; i += 8) {                 /* xorshift64* */
			uint64_t v, j;
			x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
			v = x * 0x2545F4914F6CDD1DULL;
			for (j = 0; j < 8 && i + j < k; j++) buf[i + j] = v >> (8 * j);
		}
		if (fwrite(buf, 1, k, stdout) != k) return 1;
		n -= k;
	}
	return fflush(stdout) ? 1 : 0;
}

static int cmd_fill(const char *hex, const char *bytes)
{
	static unsigned char buf[65536];
	uint64_t n = strtoull(bytes, NULL, 0);
	memset(buf, (int)strtoul(hex, NULL, 16), sizeof buf);
	while (n) {
		size_t k = n < sizeof buf ? n : sizeof buf;
		if (fwrite(buf, 1, k, stdout) != k) return 1;
		n -= k;
	}
	return fflush(stdout) ? 1 : 0;
}

static int cmd_flip(const char *path, const char *off, const char *bit)
{
	FILE *f = fopen(path, "r+b");
	long o = strtol(off, NULL, 0);
	int b = atoi(bit), c;
	if (!f || b < 0 || b > 7) { perror("nbtool flip"); return 1; }
	if (fseek(f, o, SEEK_SET) || (c = fgetc(f)) == EOF || fseek(f, o, SEEK_SET)) { fprintf(stderr, "nbtool flip: bad offset\n"); return 1; }
	if (fputc(c ^ (1 << b), f) == EOF || fclose(f)) { perror("nbtool flip"); return 1; }
	return 0;
}

static int cmd_ff(void)
{
	static unsigned char buf[65536];
	unsigned long long pos = 0, nonff = 0, first = 0;
	size_t n, i;
	while ((n = fread(buf, 1, sizeof buf, stdin)) > 0) {
		for (i = 0; i < n; i++)
			if (buf[i] != 0xFF) { if (!nonff) first = pos + i; nonff++; }
		pos += n;
	}
	if (ferror(stdin)) return 1;
	printf("%llu %llu %llu\n", nonff, first, pos);
	return 0;
}

static int cmd_oob(int argc, char **argv)
{
	unsigned w = 2048, o = 64, eo = 0, el = 0, ko = 0, kl = 0;
	const char *xf = NULL, *rf = NULL;
	FILE *x = NULL, *r = NULL;
	unsigned char *pg, *ex;
	unsigned long long pages = 0, dnon = 0, db = 0, dbits = 0, dpg = 0, eccff = 0, crit = 0;
	int i, shown = 0;
	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-w") && i + 1 < argc) w = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-o") && i + 1 < argc) o = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-x") && i + 1 < argc) xf = argv[++i];
		else if (!strcmp(argv[i], "-r") && i + 1 < argc) rf = argv[++i];
		else if (!strcmp(argv[i], "-E") && i + 2 < argc) { eo = atoi(argv[i + 1]); el = atoi(argv[i + 2]); i += 2; }
		else if (!strcmp(argv[i], "-K") && i + 2 < argc) { ko = atoi(argv[i + 1]); kl = atoi(argv[i + 2]); i += 2; }
		else { fprintf(stderr, "nbtool oob: bad argument %s\n", argv[i]); return 2; }
	}
	if (eo + el > o || ko + kl > o) { fprintf(stderr, "nbtool oob: -E outside the oob\n"); return 2; }
	pg = malloc(w + o); ex = malloc(o);
	if (!pg || !ex) return 1;
	if (xf && !(x = fopen(xf, "rb"))) { perror("nbtool oob: expected file"); return 1; }
	if (rf && !(r = fopen(rf, "w"))) { perror("nbtool oob: report"); return 1; }
	while (fread(pg, 1, w + o, stdin) == w + o) {
		unsigned k, allff = 1;
		for (k = 0; k < w; k++) if (pg[k] != 0xFF) dnon++;
		for (k = eo; k < eo + el; k++) if (pg[w + k] != 0xFF) allff = 0;
		if (el && allff) eccff++;
		if (x) {
			int pdiff = 0;
			if (fread(ex, 1, o, x) != o) { fprintf(stderr, "nbtool oob: expected file too short at page %llu\n", pages); return 1; }
			for (k = 0; k < o; k++) {
				unsigned char d = pg[w + k] ^ ex[k];
				if (!d) continue;
				db++; dbits += __builtin_popcount(d); pdiff = 1; if (k >= ko && k < ko + kl) crit++;
				if (r && shown < 40) { fprintf(r, "page %llu oob+%u got %02x exp %02x\n", pages, k, pg[w + k], ex[k]); shown++; }
			}
			dpg += pdiff;
		}
		pages++;
	}
	if (r && fclose(r)) return 1;
	printf("%llu %llu %llu %llu %llu %llu %llu\n", pages, dnon, db, dbits, dpg, eccff, crit);
	return 0;
}

static int cmd_oobx(int argc, char **argv)
{
	unsigned w = argc > 3 ? atoi(argv[3]) : 2048, o = argc > 4 ? atoi(argv[4]) : 64;
	unsigned long long first = strtoull(argv[1], NULL, 0), n = strtoull(argv[2], NULL, 0), i;
	FILE *f = fopen(argv[0], "rb");
	unsigned char *b = malloc(o);
	if (!f || !b) { perror("nbtool oobx"); return 1; }
	if (fseeko(f, (off_t)(first * (w + o)), SEEK_SET)) { perror("nbtool oobx: seek"); return 1; }
	for (i = 0; i < n; i++) {
		if (fseeko(f, (off_t)((first + i) * (w + o) + w), SEEK_SET) || fread(b, 1, o, f) != o) { fprintf(stderr, "nbtool oobx: short read at page %llu\n", first + i); return 1; }
		if (fwrite(b, 1, o, stdout) != o) return 1;
	}
	return fflush(stdout) ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc >= 4 && !strcmp(argv[1], "gen")) return cmd_gen(argv[2], argv[3]);
	if (argc >= 4 && !strcmp(argv[1], "fill")) return cmd_fill(argv[2], argv[3]);
	if (argc >= 5 && !strcmp(argv[1], "flip")) return cmd_flip(argv[2], argv[3], argv[4]);
	if (argc >= 2 && !strcmp(argv[1], "ff")) return cmd_ff();
	if (argc >= 5 && !strcmp(argv[1], "oobx")) return cmd_oobx(argc - 2, argv + 2);
	if (argc >= 2 && !strcmp(argv[1], "oob")) return cmd_oob(argc - 2, argv + 2);
	fprintf(stderr, "usage: nbtool gen SEED BYTES | fill HEX BYTES | flip FILE OFF BIT | ff | oob [-w W] [-o O] [-x EXP] [-r REP] [-E OFF LEN]\n");
	return 2;
}
