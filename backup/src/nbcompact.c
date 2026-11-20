/* nbcompact: build the input file for Open Firmware's  nd-copy-nand  from the two dumps.
 *   nbcompact -r RAW -d DATA -R REPORT [-e ERASE] [-w WRITE] [-o OOB] > NANDCOPY.DAT
 * RAW  = data+OOB per page (no ECC), DATA = ECC-corrected data only, same NAND, every block.
 * Emits, in order, the erase blocks of DATA that OFW's own save-nand would have kept:
 *   - below the bad block tables (OFW's usable-page-limit = lowest BBT block of the last 16;
 *     BBT = OOB bytes 14..17 of a block's first page read "Bbt0" or "1tbB"; none found -> last 2),
 *   - not entirely 0xFF (erased/cleanmarker-only blocks are dropped; OFW re-creates them).
 * REPORT gets key=value lines.  Exit 0 only if everything was read and written. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int rd(FILE *f, unsigned char *b, size_t n, const char *what)
{
	if (fread(b, 1, n, f) != n) { fprintf(stderr, "nbcompact: short read on %s\n", what); return -1; }
	return 0;
}

int main(int argc, char **argv)
{
	const char *rawp = NULL, *datp = NULL, *repp = NULL;
	long erase = 131072, wr = 2048, oob = 64;
	int i;
	for (i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "-r")) rawp = argv[i + 1];
		else if (!strcmp(argv[i], "-d")) datp = argv[i + 1];
		else if (!strcmp(argv[i], "-R")) repp = argv[i + 1];
		else if (!strcmp(argv[i], "-e")) erase = atol(argv[i + 1]);
		else if (!strcmp(argv[i], "-w")) wr = atol(argv[i + 1]);
		else if (!strcmp(argv[i], "-o")) oob = atol(argv[i + 1]);
		else { fprintf(stderr, "nbcompact: bad option %s\n", argv[i]); return 2; }
	}
	if (!rawp || !datp || !repp || erase <= 0 || wr <= 0 || erase % wr) { fprintf(stderr, "usage: nbcompact -r RAW -d DATA -R REPORT [-e ERASE] [-w WRITE] [-o OOB]\n"); return 2; }
	FILE *raw = fopen(rawp, "rb"), *dat = fopen(datp, "rb");
	if (!raw || !dat) { perror("nbcompact: open"); return 1; }
	fseeko(dat, 0, SEEK_END); long long dsz = ftello(dat); rewind(dat);
	fseeko(raw, 0, SEEK_END); long long rsz = ftello(raw);
	long ppb = erase / wr, rblock = ppb * (wr + oob);
	long long nblk = dsz / erase;
	if (nblk < 20 || dsz % erase || rsz != nblk * rblock) {
		fprintf(stderr, "nbcompact: sizes do not match (data %lld, raw %lld, %lld blocks)\n", dsz, rsz, nblk);
		return 1;
	}
	unsigned char *buf = malloc(erase), sig[4];
	if (!buf) return 1;

	/* BBT blocks: last 16, signature at OOB offset 14 of page 0 */
	long long limit = nblk;
	int issig[16] = { 0 };
	char list[256] = ""; int nbbt = 0;
	for (long long b = nblk - 16; b < nblk; b++) {
		if (fseeko(raw, b * rblock + wr + 14, SEEK_SET) || rd(raw, sig, 4, "raw")) return 1;
		if (!memcmp(sig, "Bbt0", 4) || !memcmp(sig, "1tbB", 4)) {
			if (b < limit) limit = b;
			issig[b - (nblk - 16)] = 1;
			if (strlen(list) < 200) sprintf(list + strlen(list), "%s%lld", nbbt ? "," : "", b);
			nbbt++;
		}
	}
	if (!nbbt) limit = nblk - 2;

	long long kept = 0, erased = 0, beyond = 0;
	for (long long b = 0; b < nblk; b++) {
		if (rd(dat, buf, erase, "data")) return 1;
		long k = 0;
		while (k < erase && buf[k] == 0xff) k++;
		if (k == erase) { erased++; continue; }
		if (b >= limit) { if (b < nblk - 16 || !issig[b - (nblk - 16)]) beyond++; continue; }
		if (fwrite(buf, 1, erase, stdout) != (size_t)erase) { perror("nbcompact: write"); return 1; }
		kept++;
	}
	if (fflush(stdout)) { perror("nbcompact: flush"); return 1; }
	FILE *rep = fopen(repp, "w");
	if (!rep) return 1;
	fprintf(rep, "blocks=%lld\nbbt_blocks=%s\nbbt_count=%d\nlimit=%lld\nkept=%lld\nerased=%lld\nbeyond_limit_nonff=%lld\n",
		nblk, nbbt ? list : "none", nbbt, limit, kept, erased, beyond);
	return fclose(rep) ? 1 : 0;
}
