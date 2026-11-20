#!/bin/sh
# olpc-splash host test: render to a fake framebuffer, check pixels/regions, the progress band
# (only it changes), no-op cases (verbose, no marker) and the time of one progress update.
# PNG preview: SPLASH_PNG=path.  Needs gcc (+ python3 for the preview).
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
bad() { echo "  FAIL: $*"; fail=1; }
ok()  { echo "  ok: $*"; }
${CC:-gcc} -O2 -Wall -Wextra -o "$tmp/sp" "$here/olpc-splash.c" "$here/../common/gfx.c" "$here/../common/status.c" || exit 1
cat >"$tmp/chk.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
static uint16_t *px; static int bad;
static uint16_t at(int x, int y) { return px[y * 1200 + x]; }
static void want(const char *n, int x, int y, uint16_t v) { if (at(x, y) != v) { printf("  FAIL: %s (%d,%d) = %04x want %04x\n", n, x, y, at(x, y), v); bad = 1; } }
static int count(int x0, int y0, int x1, int y1, uint16_t v) { int n = 0; for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) n += at(x, y) == v; return n; }
int main(int c, char **v)
{
	FILE *f = fopen(v[1], "rb"); px = malloc(1200 * 900 * 2);
	if (!f || fread(px, 2, 1200 * 900, f) != 1200 * 900) { puts("  FAIL: read fb"); return 1; }
	int pct = atoi(v[2]);
	want("bg corner", 0, 0, 0xffff); want("bg corner2", 1199, 899, 0xffff);
	if (count(280, 230, 920, 390, 0x2104) < 2000) { puts("  FAIL: title ink"); bad = 1; }
	if (count(300, 440, 900, 490, 0x6b4d) < 200) { puts("  FAIL: subtitle"); bad = 1; }
	if (count(360, 600, 840, 604, 0x2104) < 400) { puts("  FAIL: bar frame"); bad = 1; }
	int fill = count(362, 603, 838, 611, 0x2b45) / 8;       /* filled columns */
	int exp = 476 * pct / 100;
	if (fill < exp - 1 || fill > exp + 1) { printf("  FAIL: bar fill %d cols, want %d\n", fill, exp); bad = 1; }
	if (c > 3 && count(200, 630, 1000, 680, 0x6b4d) < 100) { puts("  FAIL: status text"); bad = 1; }
	return bad;
}
C
${CC:-gcc} -O2 -o "$tmp/chk" "$tmp/chk.c" || exit 1
export OLPC_SPLASH_FB="$tmp/fb" OLPC_SPLASH_RUN="$tmp" OLPC_SPLASH_CMDLINE="$tmp/cmdline"
echo "console=tty0 quiet" >"$tmp/cmdline"

"$tmp/sp" progress 50 "ignored" ; [ ! -s "$tmp/fb" ] && ok "progress without init draws nothing" || bad "progress drew without marker"
"$tmp/sp" init 0 "Starting kernel"
[ -f "$tmp/splash" ] && ok "init marks active" || bad "no marker"
"$tmp/chk" "$tmp/fb" 0 t && ok "init render (0%)" || bad "init render"
cp "$tmp/fb" "$tmp/fb0"
"$tmp/sp" progress 60 "Loading drivers"
"$tmp/chk" "$tmp/fb" 60 t && ok "progress render (60%)" || bad "progress render"
# only the band rows 590..685 may differ from the init frame
cmp -l "$tmp/fb0" "$tmp/fb" | awk '{ r = int(($1 - 1) / 2400); if (r < 590 || r >= 686) o++ } END { exit o > 0 }' \
	&& ok "only the bar band changed" || bad "pixels outside the band changed"
[ -n "$SPLASH_PNG" ] && cp "$tmp/fb" "${SPLASH_PNG%.png}.rgb565"
# timing: 20 updates, per-call wall time including fork/exec
t0=$(date +%s%N); i=0
while [ $i -lt 20 ]; do "$tmp/sp" progress $((i * 5)) "Mounting storage"; i=$((i + 1)); done
t1=$(date +%s%N)
ms=$(( (t1 - t0) / 20000000 ))
[ "$ms" -lt 10 ] && ok "progress update ${ms} ms (incl. process start)" || bad "progress update ${ms} ms"
"$tmp/sp" done; [ ! -f "$tmp/splash" ] && ok "done clears marker" || bad "marker left"
rm -f "$tmp/fb"; echo "olpcboy.verbose" >"$tmp/cmdline"
"$tmp/sp" init 0 "x"
{ [ ! -s "$tmp/fb" ] && [ ! -f "$tmp/splash" ]; } && ok "olpcboy.verbose skips the splash" || bad "verbose drew"
[ "$("$tmp/sp" bogus 2>/dev/null; echo $?)" = 2 ] && ok "bad usage exits 2" || bad "usage"
[ "$fail" = 0 ] && echo "olpc-splash tests PASS" || { echo "olpc-splash tests FAILED"; exit 1; }
