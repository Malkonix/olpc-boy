/* nbjffs: look at a JFFS2 image on stdin.   nbjffs sum ERASE
 * Prints "BLOCKS USED SUMMED LAST":
 *   BLOCKS  erase blocks read (a short last block counts)
 *   USED    blocks that are not entirely 0xFF
 *   SUMMED  used blocks whose last 8 bytes are a JFFS2 summary marker (offset, magic 0x02851885, little endian)
 *   LAST    1 + index of the last used block (0 when everything is erased)
 * No SSE, no libs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	unsigned long long erase, blocks = 0, used = 0, summed = 0, last = 0;
	unsigned char *b;
	size_t n;

	if (argc != 3 || strcmp(argv[1], "sum") || (erase = strtoull(argv[2], NULL, 0)) < 16 || erase > 16 << 20) {
		fprintf(stderr, "usage: nbjffs sum ERASE   (JFFS2 image on stdin)\n");
		return 2;
	}
	b = malloc(erase);
	if (!b) return 1;
	while ((n = fread(b, 1, erase, stdin)) > 0) {
		size_t k;
		for (k = 0; k < n && b[k] == 0xFF; k++) ;
		blocks++;
		if (k < n) {
			used++; last = blocks;
			if (n == erase && !memcmp(b + n - 4, "\x85\x18\x85\x02", 4)) summed++;
		}
	}
	printf("%llu %llu %llu %llu\n", blocks, used, summed, last);
	return 0;
}
