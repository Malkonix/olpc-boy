/* nbcmp: compare stdin with FILE, report how they differ.
 *   nbcmp [-b BLOCK] [-c countfile] [-o resultfile] FILE
 *   -b  block size for the "blocks with differences" count (default 131072)
 *   -c  rewrite "BYTES" read from stdin about once a second
 *   -o  at EOF write "DIFFBYTES DIFFBITS DIFFBLOCKS STREAMBYTES FILEBYTES\n"
 * Bytes present in only one of the two count as fully different.  Exit 0 if both were read
 * to the end without I/O error (differences are reported, not an error). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

int main(int argc, char **argv)
{
	static unsigned char a[65536], b[65536];
	const char *cfile = NULL, *ofile = NULL, *path = NULL;
	unsigned long long blk = 131072, pos = 0, fpos = 0, dbytes = 0, dbits = 0, dblocks = 0, lastblk = ~0ULL;
	time_t last = 0;
	int i;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-b") && i + 1 < argc) blk = strtoull(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "-c") && i + 1 < argc) cfile = argv[++i];
		else if (!strcmp(argv[i], "-o") && i + 1 < argc) ofile = argv[++i];
		else if (argv[i][0] != '-' && !path) path = argv[i];
		else { fprintf(stderr, "usage: nbcmp [-b BLOCK] [-c countfile] [-o resultfile] FILE\n"); return 2; }
	}
	if (!path || !blk) { fprintf(stderr, "usage: nbcmp [-b BLOCK] [-c countfile] [-o resultfile] FILE\n"); return 2; }
	FILE *f = fopen(path, "rb");
	if (!f) { perror("nbcmp: open"); return 1; }
	for (;;) {
		ssize_t n = read(0, a, sizeof a);
		if (n < 0) { if (errno == EINTR) continue; perror("nbcmp: read"); return 1; }
		if (n == 0) break;
		size_t m = fread(b, 1, n, f);
		if (m < (size_t)n && ferror(f)) { perror("nbcmp: file read"); return 1; }
		if (memcmp(a, b, m)) {
			for (size_t k = 0; k < m; k++) {
				unsigned char x = a[k] ^ b[k];
				if (!x) continue;
				dbytes++; dbits += __builtin_popcount(x);
				if ((pos + k) / blk != lastblk) { lastblk = (pos + k) / blk; dblocks++; }
			}
		}
		for (size_t k = m; k < (size_t)n; k++) {            /* stream longer than the file */
			dbytes++; dbits += 8;
			if ((pos + k) / blk != lastblk) { lastblk = (pos + k) / blk; dblocks++; }
		}
		fpos += m; pos += n;
		if (cfile) {
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			if (ts.tv_sec != last) {
				FILE *c = fopen(cfile, "w");
				last = ts.tv_sec;
				if (c) { fprintf(c, "%llu\n", pos); fclose(c); }
			}
		}
	}
	for (;;) {                                           /* file longer than the stream */
		size_t m = fread(b, 1, sizeof b, f);
		if (!m) break;
		for (size_t k = 0; k < m; k++) {
			dbytes++; dbits += 8;
			if ((pos + k) / blk != lastblk) { lastblk = (pos + k) / blk; dblocks++; }
		}
		fpos += m;
	}
	if (ferror(f)) { perror("nbcmp: file read"); return 1; }
	if (cfile) { FILE *c = fopen(cfile, "w"); if (c) { fprintf(c, "%llu\n", pos); fclose(c); } }
	if (ofile) {
		FILE *o = fopen(ofile, "w");
		if (!o) return 1;
		fprintf(o, "%llu %llu %llu %llu %llu\n", dbytes, dbits, dblocks, pos, fpos);
		if (fclose(o)) return 1;
	}
	return 0;
}
