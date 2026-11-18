#!/usr/bin/env bash
# Build the OLPC XO-1 kernel. Runs INSIDE the olpc-boy-builder container,
# from the project root (/work):
#   docker run --rm -u $(id -u):$(id -g) -v $PWD:/work -w /work \
#       olpc-boy-builder bash kernel/build.sh
#
# Env:
#   INITRAMFS_DIR  directory to embed as initramfs (default out/initramfs);
#                  skipped when missing or empty.
#   JOBS           parallel jobs (default 4)
#   KVER           kernel version (default below)
#
# Idempotent: the tarball is downloaded/extracted/patched once; later runs
# only re-sync config + initramfs and let make rebuild what changed.
set -euo pipefail

KVER="${KVER:-6.1.192}"
JOBS="${JOBS:-4}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KDIR="$ROOT/kernel"
SRC="$ROOT/out/src"
OUT="$ROOT/out/kernel"
INITRAMFS_DIR="${INITRAMFS_DIR:-$ROOT/out/initramfs}"
case "$INITRAMFS_DIR" in /*) ;; *) INITRAMFS_DIR="$ROOT/$INITRAMFS_DIR" ;; esac

TARBALL="linux-$KVER.tar.xz"
TREE="$SRC/linux-$KVER"          # pristine + patched source
BLD="$SRC/build-$KVER"           # out-of-tree build dir (O=)
MAJOR="${KVER%%.*}"
BASEURL="https://cdn.kernel.org/pub/linux/kernel/v${MAJOR}.x"

# ARCH=i386 always: the container is x86_64 even though the target is 32-bit.
mk() { make ARCH=i386 -C "$TREE" O="$BLD" "$@"; }

mkdir -p "$SRC" "$OUT" "$BLD"

# --- 1. download + verify -------------------------------------------------
if [ ! -f "$SRC/$TARBALL" ]; then
    echo ">> downloading $TARBALL"
    curl -fL --retry 3 -o "$SRC/$TARBALL.part" "$BASEURL/$TARBALL"
    mv "$SRC/$TARBALL.part" "$SRC/$TARBALL"
fi
# Always fetch the checksum list fresh; fall back to the cached copy offline.
if curl -fsL --retry 2 -o "$SRC/sha256sums-v$MAJOR.asc.new" "$BASEURL/sha256sums.asc"; then
    mv "$SRC/sha256sums-v$MAJOR.asc.new" "$SRC/sha256sums-v$MAJOR.asc"
fi
want="$(awk -v f="$TARBALL" '$2==f {print $1}' "$SRC/sha256sums-v$MAJOR.asc")"
[ -n "$want" ] || { echo "no sha256 for $TARBALL in sha256sums.asc" >&2; exit 1; }
have="$(sha256sum "$SRC/$TARBALL" | awk '{print $1}')"
if [ "$want" != "$have" ]; then
    echo "sha256 mismatch for $TARBALL: want $want have $have" >&2
    rm -f "$SRC/$TARBALL"
    exit 1
fi
echo ">> sha256 ok ($have)"

# --- 2. extract + patch (only when absent or the patch set changed) -------
patches=("$KDIR"/patches/*.patch)
[ -e "${patches[0]}" ] || patches=()
pstamp="$KVER $(cat "${patches[@]}" /dev/null | sha256sum | awk '{print $1}')"
if [ ! -d "$TREE" ] || [ "$(cat "$TREE/.olpc-stamp" 2>/dev/null)" != "$pstamp" ]; then
    echo ">> extracting + patching"
    # Files touched by the previous patch set must be re-touched after the
    # re-extract (tar restores old mtimes, which would hide a reverted patch
    # from make). Unchanged files keep tarball mtimes => no full rebuild.
    prev="$(cat "$TREE/.olpc-files" 2>/dev/null || true)"
    rm -rf "$TREE"
    tar -C "$SRC" -xf "$SRC/$TARBALL"
    for p in "${patches[@]}"; do
        echo "   patch $(basename "$p")"
        patch -d "$TREE" -p1 --no-backup-if-mismatch < "$p"
    done
    new="$(cat "${patches[@]}" /dev/null | sed -n 's#^+++ b/##p')"
    printf '%s\n' "$new" > "$TREE/.olpc-files"
    for f in $prev $new; do [ -f "$TREE/$f" ] && touch "$TREE/$f"; done
    echo "$pstamp" > "$TREE/.olpc-stamp"
fi

# --- 3. configuration ------------------------------------------------------
if [ ! -f "$BLD/.config" ] || ! cmp -s "$KDIR/config" "$BLD/.config.in"; then
    echo ">> applying kernel/config"
    cp "$KDIR/config" "$BLD/.config"
    cp "$KDIR/config" "$BLD/.config.in"
    mk olddefconfig
fi

# --- 4. initramfs ----------------------------------------------------------
SC="$TREE/scripts/config --file $BLD/.config"
if [ -d "$INITRAMFS_DIR" ] && [ -n "$(ls -A "$INITRAMFS_DIR" 2>/dev/null)" ]; then
    echo ">> embedding initramfs from $INITRAMFS_DIR"
    # A directory source cannot carry device nodes: /dev comes from devtmpfs.
    $SC --set-str CONFIG_INITRAMFS_SOURCE "$INITRAMFS_DIR" \
        --set-val CONFIG_INITRAMFS_ROOT_UID 0 \
        --set-val CONFIG_INITRAMFS_ROOT_GID 0
    # kbuild may not notice added/removed files: force a regen when the tree
    # listing changed since the last build.
    ihash="$(cd "$INITRAMFS_DIR" && find . -printf '%p %y %s %T@ %m\n' | LC_ALL=C sort | sha256sum | awk '{print $1}')"
    if [ "$(cat "$BLD/.initramfs-hash" 2>/dev/null)" != "$ihash" ]; then
        rm -f "$BLD"/usr/initramfs_data.cpio* "$BLD"/usr/initramfs_inc_data "$BLD"/usr/.initramfs_data.cpio*
        echo "$ihash" > "$BLD/.initramfs-hash"
    fi
else
    echo ">> no initramfs at $INITRAMFS_DIR; building without one"
    $SC --set-str CONFIG_INITRAMFS_SOURCE ""
    rm -f "$BLD/.initramfs-hash"
fi
mk olddefconfig

# --- 5. build --------------------------------------------------------------
mk -j"$JOBS" bzImage modules

# --- 6. install ------------------------------------------------------------
REL="$(mk -s kernelrelease)"
echo ">> kernel release: $REL"
rm -rf "$OUT/modules"
mkdir -p "$OUT/modules"
mk -j"$JOBS" modules_install INSTALL_MOD_PATH="$OUT/modules" INSTALL_MOD_STRIP=1
# build/source symlinks point into the build tree: useless on the target.
rm -f "$OUT/modules/lib/modules/$REL/build" "$OUT/modules/lib/modules/$REL/source"
cp "$BLD/arch/x86/boot/bzImage" "$OUT/bzImage"
echo "$REL" > "$OUT/version"
cp "$BLD/.config" "$OUT/config"
ls -l "$OUT/bzImage"
echo ">> done"
