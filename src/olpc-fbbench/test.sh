#!/bin/sh
# Scaling-kernel self-test: every blit variant (C, dirty-row, MMX, movntq when
# the CPU has it) must match the naive one byte for byte.  Needs only gcc.
set -e
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
${CC:-gcc} -O2 -march=pentium-mmx -mmmx -Wall -Wextra -DFBBENCH_SELFTEST -o "$tmp/t" "$here/olpc-fbbench.c"
"$tmp/t"
