#!/usr/bin/env bash
# flash-keep.sh <image> <device> [flash.sh options]      (HOST script)
#
# Re-flash the test stick without losing what the player made on it: battery saves
# (roms/saves), save states (roms/states), downloaded covers + indexes (covers/) and the
# launcher state (olpc-menu.state), the SSH-off flag (ssh.off) are copied off the stick's OLPCLOG partition
# (mounted READ-ONLY), injected into a copy of the image with mtools (nothing is written
# to the stick here), and that copy is written by flash.sh with all of its guards.
# A copy of what was kept stays in out/stick-keep/<timestamp>/.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"
die() { echo "flash-keep: ERROR: $*" >&2; exit 1; }
[ $# -ge 2 ] || { echo "usage: $0 <image> <device> [flash.sh options]" >&2; exit 2; }
IMG=$1; DEV=$2; shift 2
[ -s "$IMG" ] || die "no image $IMG"
case $DEV in /dev/sda|/dev/sda[0-9]*) die "refusing $DEV (system disk)" ;; esac
[ -b "$DEV" ] || die "$DEV is not a block device"
command -v mcopy >/dev/null || die "mtools not installed"

KEEP=out/stick-keep/$(date +%Y%m%d-%H%M%S)
PART=$(lsblk -lnpo NAME,LABEL "$DEV" | awk '$2=="OLPCLOG"{print $1; exit}')

# the boot logs on the stick are about to be overwritten: collect them first
[ -n "$PART" ] && { bash scripts/collect-logs.sh "$DEV" >/dev/null 2>&1 \
    && echo "flash-keep: logs collected into logs/" || echo "flash-keep: warning: log collection failed"; }

kept=0
if [ -n "$PART" ]; then
    MNT=$(lsblk -nro MOUNTPOINT "$PART" | head -n1); MOUNTED=""
    if [ -z "$MNT" ]; then
        udisksctl mount -b "$PART" -o ro >/dev/null 2>&1 && MOUNTED=udisks \
            || die "could not mount $PART read-only"
        MNT=$(lsblk -nro MOUNTPOINT "$PART" | head -n1)
    fi
    MNT=$(printf '%b' "$MNT")
    mkdir -p "$KEEP"
    for p in roms/saves roms/states covers olpc-menu.state ssh.off; do
        [ -e "$MNT/$p" ] || continue
        (cd "$MNT" && cp -r --parents "$p" "$REPO/$KEEP/")
        kept=1
    done
    [ "$MOUNTED" = udisks ] && udisksctl unmount -b "$PART" >/dev/null
fi

if [ "$kept" = 0 ]; then
    echo "flash-keep: nothing to keep on the stick, plain flash"
    rmdir "$KEEP" 2>/dev/null || true
    exec bash scripts/flash.sh "$IMG" "$DEV" "$@"
fi

echo "flash-keep: kept in $KEEP:"
(cd "$KEEP" && find . -type f | sed 's/^/  /')
OUT=out/olpc-boy-keep.img
cp --sparse=always "$IMG" "$OUT"
export MTOOLS_SKIP_CHECK=1
P1="$OUT@@1M"                      # partition 1 (OLPCLOG) starts at 1 MiB, see mkimage.sh
for top in "$KEEP"/*; do
    mcopy -s -o -D o -i "$P1" "$top" ::/ || die "mcopy of ${top##*/} into the image failed"
done
# every kept file must now be in the image copy
missing=0
while IFS= read -r f; do
    mtype -i "$P1" "::/${f#./}" >/dev/null 2>&1 || { echo "  missing in image: $f"; missing=1; }
done < <(cd "$KEEP" && find . -type f)
[ "$missing" = 0 ] || die "kept files did not all reach the image copy (stick untouched)"
echo "flash-keep: injected into $OUT, flashing it"
exec bash scripts/flash.sh "$OUT" "$DEV" "$@"
