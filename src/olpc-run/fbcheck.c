/* fbcheck FILE X0 Y0 W H: count non-black RGB565 pixels of a 1200x900 raw dump
 * inside and outside the rectangle.  Prints "inside=N outside=M". */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(int argc, char **argv)
{
	enum { FW = 1200, FH = 900 };
	FILE *f;
	static uint16_t px[FW * FH];
	int x0, y0, w, h, x, y;
	long in = 0, out = 0;

	if (argc != 6) { fprintf(stderr, "usage: fbcheck file x0 y0 w h\n"); return 2; }
	x0 = atoi(argv[2]); y0 = atoi(argv[3]); w = atoi(argv[4]); h = atoi(argv[5]);
	f = fopen(argv[1], "rb");
	if (!f || fread(px, 2, FW * FH, f) != FW * FH) { fprintf(stderr, "fbcheck: short or missing %s\n", argv[1]); return 2; }
	fclose(f);
	for (y = 0; y < FH; y++)
		for (x = 0; x < FW; x++)
			if (px[y * FW + x]) { if (x >= x0 && x < x0 + w && y >= y0 && y < y0 + h) in++; else out++; }
	printf("inside=%ld outside=%ld\n", in, out);
	return 0;
}
