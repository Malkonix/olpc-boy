#!/bin/sh
# smoke.sh fetch-and-build | run     (inside the builder container)
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/out/cores
R=$OUT/test-roms
mkdir -p "$R"

get() { # url sha256 file
    [ -f "$R/$3" ] && echo "$2  $R/$3" | sha256sum -c - >/dev/null 2>&1 && return 0
    curl -fsSL -o "$R/$3" "$1"
    echo "$2  $R/$3" | sha256sum -c -
}
# blargg cpu_instrs (LGPL-2.1+), retrio/gb-test-roms@c240dd7
get https://raw.githubusercontent.com/retrio/gb-test-roms/c240dd7d700e5c0b00a7bbba52b53e4ee67b5f15/cpu_instrs/cpu_instrs.gb \
    8c5e12f41e0ba5bbca796944f92ffe6de28809198682c4332e38d1b3cf56fcf2 cpu_instrs.gb
# nestest by kevtris (freely distributable), christopherpow/nes-test-roms@95d8f62
get https://raw.githubusercontent.com/christopherpow/nes-test-roms/95d8f621ae55cee0d09b91519a8989ae0e64753b/other/nestest.nes \
    f67d55fd6b3cf0bad1cc85f1df0d739c65b53e79cecb7fea8f77ec0eadab0004 nestest.nes

LRH=$(dirname "$(find "$ROOT/out/src/cores/gambatte" -name libretro.h | head -n1)")
gcc -O2 -march=i586 -Wall -I"$LRH" -o "$OUT/coretest" "$ROOT/src/cores/coretest.c" -ldl

if [ "${1:-}" = run ] || [ "${1:-}" = fetch-and-build ]; then
    FRAMES=${FRAMES:-1800}
    for c in gambatte:cpu_instrs.gb quicknes:nestest.nes fceumm:nestest.nes; do
        so=$OUT/${c%%:*}_libretro.so; [ -f "$so" ] || continue
        "$OUT/coretest" "$so" "$R/${c#*:}" "$FRAMES"
    done
fi
