#!/bin/sh
# Build libretro cores (gambatte, quicknes, fceumm) for the OLPC XO-1 (Geode LX).
# Runs inside the olpc-boy-builder container with the repo mounted at /work.
#   sh src/cores/build.sh [core ...]     (default: all)
# Output: out/cores/<name>_libretro.so, out/cores/VERSIONS, out/cores/test-roms/
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=$ROOT/out/src/cores
OUT=$ROOT/out/cores
JOBS=${JOBS:-$(nproc)}

# name|repo|pinned commit|license
CORES='
gambatte|https://github.com/libretro/gambatte-libretro|77f2567d718b8973d02caa2daa8dce4953faf811|GPL-2.0-only
quicknes|https://github.com/libretro/QuickNES_Core|26bb785c9deddb66a17717b21bb4e328f03ade32|LGPL-2.1+ (nes_emu, blargg) + GPL-2.0 (libretro glue); distribute as GPL-2.0
fceumm|https://github.com/libretro/libretro-fceumm|77468e568d9fe9c48d684ca911f1a3fe7259fdf7|GPL-2.0-only
'

# No SSE on Geode LX. -static-libgcc/-static-libstdc++ keep the .so free of
# libstdc++/libgcc_s runtime dependencies (only musl libc remains).
OPT=${OPT:--O2 -march=geode -mtune=geode -fomit-frame-pointer}
CFLAGS_ALL="$OPT -fPIC"
LDFLAGS_ALL="-static-libgcc -static-libstdc++"
# Exported, not passed on the make command line: the upstream Makefiles append
# their include paths with += and a command-line override would drop them.
export CFLAGS=$CFLAGS_ALL CXXFLAGS=$CFLAGS_ALL LDFLAGS=$LDFLAGS_ALL

git config --global --add safe.directory "*"   # container-local config
mkdir -p "$SRC" "$OUT"
WANT=${*:-gambatte quicknes fceumm}

fetch() { # name repo commit
    d=$SRC/$1
    if [ ! -d "$d/.git" ]; then
        git init -q "$d"
        git -C "$d" remote add origin "$2"
    fi
    if ! git -C "$d" cat-file -e "$3^{commit}" 2>/dev/null; then
        git -C "$d" fetch -q --depth 1 origin "$3"
    fi
    git -C "$d" checkout -q -f "$3"
    git -C "$d" clean -qfdx
}

build() { # name
    d=$SRC/$1
    case $1 in
    gambatte)
        make -C "$d" -f Makefile.libretro -j"$JOBS" platform=unix
        so=$d/gambatte_libretro.so ;;
    quicknes)
        make -C "$d" -j"$JOBS" platform=unix
        so=$d/quicknes_libretro.so ;;
    fceumm)
        make -C "$d" -f Makefile.libretro -j"$JOBS" platform=unix
        so=$d/fceumm_libretro.so ;;
    esac
    [ -f "$so" ] || { echo "build.sh: $so missing" >&2; exit 1; }
    cp "$so" "$OUT/$1_libretro.so"
    strip --strip-unneeded "$OUT/$1_libretro.so"
}

: > "$OUT/VERSIONS.new"
echo "$CORES" | while IFS='|' read -r name repo commit lic; do
    [ -n "$name" ] || continue
    case " $WANT " in *" $name "*) ;; *) continue ;; esac
    echo "== $name @ $commit"
    fetch "$name" "$repo" "$commit"
    build "$name"
    size=$(stat -c %s "$OUT/${name}_libretro.so")
    needed=$(readelf -d "$OUT/${name}_libretro.so" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p' | tr '\n' ' ')
    printf '%s\n  repo:    %s\n  commit:  %s\n  license: %s\n  flags:   %s | ld: %s\n  size:    %s bytes (stripped)\n  needed:  %s\n\n' \
        "$name" "$repo" "$commit" "$lic" "$CFLAGS_ALL" "$LDFLAGS_ALL" "$size" "$needed" >> "$OUT/VERSIONS.new"
done
mv "$OUT/VERSIONS.new" "$OUT/VERSIONS"

# Test ROMs + harness (never committed; out/ is git-ignored).
sh "$ROOT/src/cores/smoke.sh" fetch-and-build
