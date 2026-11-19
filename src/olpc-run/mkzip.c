/* mkzip OUT.zip ENTRYNAME FILE: write a one-entry stored (uncompressed) zip. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t crc32_(const uint8_t *p, size_t n)
{
	uint32_t c = 0xffffffffu;
	size_t i; int k;

	for (i = 0; i < n; i++) { c ^= p[i]; for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & -(c & 1)); }
	return ~c;
}
static void w16(FILE *f, unsigned v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }
static void w32(FILE *f, uint32_t v) { w16(f, v & 0xffff); w16(f, v >> 16); }

int main(int argc, char **argv)
{
	FILE *in, *out;
	uint8_t *d;
	long n, cd_off, cd_size;
	size_t nl;
	uint32_t crc;

	if (argc != 4) { fprintf(stderr, "usage: mkzip out.zip name file\n"); return 2; }
	in = fopen(argv[3], "rb");
	if (!in) { perror(argv[3]); return 1; }
	fseek(in, 0, SEEK_END); n = ftell(in); rewind(in);
	d = malloc((size_t)n);
	if (fread(d, 1, (size_t)n, in) != (size_t)n) return 1;
	fclose(in);
	crc = crc32_(d, (size_t)n); nl = strlen(argv[2]);
	out = fopen(argv[1], "wb");
	w32(out, 0x04034b50); w16(out, 10); w16(out, 0); w16(out, 0); w16(out, 0); w16(out, 0x21);
	w32(out, crc); w32(out, (uint32_t)n); w32(out, (uint32_t)n); w16(out, (unsigned)nl); w16(out, 0);
	fwrite(argv[2], 1, nl, out); fwrite(d, 1, (size_t)n, out);
	cd_off = ftell(out);
	w32(out, 0x02014b50); w16(out, 20); w16(out, 10); w16(out, 0); w16(out, 0); w16(out, 0); w16(out, 0x21);
	w32(out, crc); w32(out, (uint32_t)n); w32(out, (uint32_t)n); w16(out, (unsigned)nl);
	w16(out, 0); w16(out, 0); w16(out, 0); w16(out, 0); w32(out, 0); w32(out, 0);
	fwrite(argv[2], 1, nl, out);
	cd_size = ftell(out) - cd_off;
	w32(out, 0x06054b50); w16(out, 0); w16(out, 0); w16(out, 1); w16(out, 1);
	w32(out, (uint32_t)cd_size); w32(out, (uint32_t)cd_off); w16(out, 0);
	fclose(out);
	return 0;
}
