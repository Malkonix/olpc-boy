/* nbhash: stdin -> stdout pass-through that also computes SHA-256 and counts bytes.
 *   nbhash [-n] [-c countfile] [-o resultfile] [-B BLOCK -L listfile]
 *   -n  discard (no stdout)      -c  rewrite "BYTES" about once a second
 *   -o  at EOF write "SHA256HEX BYTES\n"
 *   -B/-L  also SHA-256 every BLOCK bytes, one "INDEX HEX" line each into listfile
 * Exit 0 only on a clean EOF with every byte written.  No SSE, no libs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

struct sha { uint32_t H[8]; uint8_t tail[64]; unsigned ntail; uint64_t total; };

static void sha_init(struct sha *c)
{
	static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	memcpy(c->H, iv, sizeof iv); c->ntail = 0; c->total = 0;
}

static void block(struct sha *c, const uint8_t *p)
{
	uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2, *H = c->H;
	int i;
	for (i = 0; i < 16; i++)
		w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
	for (; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = H[0]; b = H[1]; cc = H[2]; d = H[3]; e = H[4]; f = H[5]; g = H[6]; h = H[7];
	for (i = 0; i < 64; i++) {
		t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
		t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
		h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
	}
	H[0] += a; H[1] += b; H[2] += cc; H[3] += d; H[4] += e; H[5] += f; H[6] += g; H[7] += h;
}

static void feed(struct sha *c, const uint8_t *p, size_t n)
{
	c->total += n;
	if (c->ntail) {
		size_t k = 64 - c->ntail < n ? 64 - c->ntail : n;
		memcpy(c->tail + c->ntail, p, k); c->ntail += k; p += k; n -= k;
		if (c->ntail < 64) return;
		block(c, c->tail); c->ntail = 0;
	}
	for (; n >= 64; p += 64, n -= 64) block(c, p);
	memcpy(c->tail, p, n); c->ntail = n;
}

/* finish: writes 64 hex chars + NUL into out; c->total keeps the message length */
static void finish(struct sha *c, char *out)
{
	uint64_t bits = c->total * 8, keep = c->total;
	uint8_t pad[72] = { 0x80 };
	unsigned padlen = (c->ntail < 56 ? 56 : 120) - c->ntail;
	int i;
	for (i = 0; i < 8; i++) pad[padlen + i] = bits >> (56 - 8 * i);
	feed(c, pad, padlen + 8);
	c->total = keep;
	for (i = 0; i < 8; i++) sprintf(out + 8 * i, "%08x", c->H[i]);
}

static void write_file(const char *path, const char *s)
{
	FILE *f = fopen(path, "w");
	if (f) { fputs(s, f); fclose(f); }
}

int main(int argc, char **argv)
{
	static uint8_t buf[65536];
	const char *cfile = NULL, *ofile = NULL, *lfile = NULL;
	int discard = 0, i;
	unsigned long long bsize = 0, bn = 0, inblk = 0;
	time_t last = 0;
	struct sha all, blk;
	FILE *lf = NULL;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-n")) discard = 1;
		else if (!strcmp(argv[i], "-c") && i + 1 < argc) cfile = argv[++i];
		else if (!strcmp(argv[i], "-o") && i + 1 < argc) ofile = argv[++i];
		else if (!strcmp(argv[i], "-B") && i + 1 < argc) bsize = strtoull(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "-L") && i + 1 < argc) lfile = argv[++i];
		else { fprintf(stderr, "usage: nbhash [-n] [-c countfile] [-o resultfile] [-B BLOCK -L listfile]\n"); return 2; }
	}
	if (!!bsize != !!lfile) { fprintf(stderr, "nbhash: -B and -L go together\n"); return 2; }
	sha_init(&all); sha_init(&blk);
	if (lfile && !(lf = fopen(lfile, "w"))) { perror("nbhash: list"); return 1; }
	for (;;) {
		ssize_t n = read(0, buf, sizeof buf);
		if (n < 0) { if (errno == EINTR) continue; perror("nbhash: read"); return 1; }
		if (n == 0) break;
		feed(&all, buf, n);
		if (bsize) {                       /* per-block hashes: split the chunk at block borders */
			size_t off = 0;
			while (off < (size_t)n) {
				size_t k = bsize - inblk < (unsigned long long)(n - off) ? bsize - inblk : n - off;
				feed(&blk, buf + off, k); off += k; inblk += k;
				if (inblk == bsize) {
					char h[65]; finish(&blk, h);
					fprintf(lf, "%llu %s\n", bn++, h);
					sha_init(&blk); inblk = 0;
				}
			}
		}
		if (!discard) {
			ssize_t off = 0;
			while (off < n) {
				ssize_t w = write(1, buf + off, n - off);
				if (w < 0) { if (errno == EINTR) continue; perror("nbhash: write"); return 1; }
				off += w;
			}
		}
		if (cfile) {
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			if (ts.tv_sec != last) {
				char s[32];
				last = ts.tv_sec;
				snprintf(s, sizeof s, "%llu\n", (unsigned long long)all.total);
				write_file(cfile, s);
			}
		}
	}
	if (lf) {
		if (inblk) { char h[65]; finish(&blk, h); fprintf(lf, "%llu %s\n", bn++, h); }
		if (fclose(lf)) return 1;
	}
	{
		char hex[65];
		finish(&all, hex);
		if (cfile) {
			char s[32];
			snprintf(s, sizeof s, "%llu\n", (unsigned long long)all.total);
			write_file(cfile, s);
		}
		if (ofile) {
			char s[128];
			snprintf(s, sizeof s, "%s %llu\n", hex, (unsigned long long)all.total);
			write_file(ofile, s);
		}
	}
	return 0;
}
