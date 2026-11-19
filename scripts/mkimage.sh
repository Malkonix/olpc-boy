#!/usr/bin/env bash
# mkimage.sh -- assemble out/olpc-boy.img (runs inside the builder container as root).
#
# Layout (MBR, 1 MiB aligned):
#   p1  FAT32 (type 0x0c, bootable)  label OLPCLOG   256 MiB
#         boot/olpc.fth  boot/bzImage  logs/  roms/        (8.3 short names only)
#   p2  ext4  (type 0x83)            label OLPCROOT  rootfs.tar content + FREE_MIB free
#
# No loop devices / mounts: each filesystem is built as a plain file
# (mkfs.vfat + mtools, mke2fs -d) and dd'ed into place after sfdisk wrote the table.
#
# Inputs : out/kernel/bzImage  out/rootfs.tar  boot/olpc.fth
# Output : out/olpc-boy.img     (chown'ed to $HOST_UID:$HOST_GID)
# Env    : HOST_UID HOST_GID  IMG_OUT KERNEL_IMG ROOTFS_TAR (override paths, for tests)
#            FREE_MIB (default 200)  FAT_MIB (default 256)
set -euo pipefail

cd "$(dirname "$0")/.."
OUT=out
IMG=${IMG_OUT:-$OUT/olpc-boy.img}
KERNEL=${KERNEL_IMG:-$OUT/kernel/bzImage}
ROOTFS=${ROOTFS_TAR:-$OUT/rootfs.tar}
FTH=boot/olpc.fth
FAT_MIB=${FAT_MIB:-256}
FREE_MIB=${FREE_MIB:-200}
ALIGN_SECT=2048                     # 1 MiB in 512-byte sectors

die() { echo "mkimage: ERROR: $*" >&2; exit 1; }
for f in "$KERNEL" "$ROOTFS" "$FTH"; do
    [ -s "$f" ] || die "missing input $f (run the earlier build steps first)"
done
for t in mkfs.vfat mcopy mmd mdir mke2fs e2fsck sfdisk blkid dd tar; do
    command -v "$t" >/dev/null || die "tool not found in container: $t"
done
head -n1 "$FTH" | grep -q '^\\ ' || die "$FTH must start with a '\\ ' comment line (Open Firmware requirement)"

# Work area on container-local storage (device nodes cannot live on some bind mounts).
WORK=$(mktemp -d /tmp/mkimage.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
export MTOOLS_SKIP_CHECK=1

# ---- 1. FAT32 log/boot partition ------------------------------------------------
echo "mkimage: building FAT32 OLPCLOG (${FAT_MIB} MiB)"
FAT=$WORK/fat.img
truncate -s "${FAT_MIB}M" "$FAT"
# 2 KiB clusters keep the cluster count well above the FAT32 minimum (65525).
mkfs.vfat -F 32 -s 4 -n OLPCLOG "$FAT" >/dev/null
mmd   -i "$FAT" ::boot ::logs ::roms
# Upper-case 8.3 names: Open Firmware's FAT reader sees short names only
# (it upper-cases lookups, so "\boot\bzImage" matches BZIMAGE).
mcopy -i "$FAT" "$FTH"    ::boot/OLPC.FTH
mcopy -i "$FAT" "$KERNEL" ::boot/BZIMAGE
# Optional WiFi credentials (git-ignored, repo root): the device reads WIFI.CFG from the
# FAT root; it can also be edited from any PC.  Never echo its contents.
if [ -f wifi.cfg ]; then
    mcopy -i "$FAT" wifi.cfg ::WIFI.CFG
    echo "mkimage: wifi.cfg -> ::WIFI.CFG"
fi

# Games: the repo's ROMs/GB, ROMs/GBC, ROMs/NES (git-ignored user data) go to roms/gb|gbc|nes on the
# FAT partition with their long names (VFAT LFN; only the files the firmware reads need 8.3 names).
ROMS_COPIED=0
for sysd in GB GBC NES; do
    [ -d "ROMs/$sysd" ] || continue
    low=$(echo "$sysd" | tr 'A-Z' 'a-z')
    LIST=$WORK/roms-$low.list
    find "ROMs/$sysd" -maxdepth 1 -type f \( -iname '*.gb' -o -iname '*.gbc' -o -iname '*.nes' -o -iname '*.zip' \) | sort >"$LIST"
    [ -s "$LIST" ] || continue
    ROMS_COPIED=1
done
if [ "$ROMS_COPIED" = 1 ]; then
    NEED=0; NFILES=0
    for l in "$WORK"/roms-*.list; do
        while IFS= read -r f; do
            sz=$(stat -c %s "$f")
            NEED=$(( NEED + (sz + 2047) / 2048 * 2048 + 512 )); NFILES=$(( NFILES + 1 ))
        done <"$l"
    done
    FREE=$(mdir -i "$FAT" :: | sed -n 's/^ *\([0-9][0-9 ,.]*\) bytes free.*/\1/p' | tr -d ' ,.')
    [ -n "$FREE" ] || die "cannot read the free space of the FAT partition"
    echo "mkimage: ROMs: $NFILES files need ~$(( NEED / 1048576 )) MiB, FAT has $(( FREE / 1048576 )) MiB free"
    [ $(( NEED + 1048576 )) -le "$FREE" ] || die "ROMs do not fit on the ${FAT_MIB} MiB FAT partition ($(( NEED / 1048576 )) MiB needed, $(( FREE / 1048576 )) MiB free): remove some or raise FAT_MIB"
    mmd -i "$FAT" ::roms/gb ::roms/gbc ::roms/nes
    for l in "$WORK"/roms-*.list; do
        low=$(basename "$l" .list); low=${low#roms-}
        while IFS= read -r f; do
            mcopy -i "$FAT" "$f" "::roms/$low/" || die "cannot copy $f to the FAT partition"
        done <"$l"
    done
    echo "mkimage: roms/ on the FAT partition:"
    for low in gb gbc nes; do mdir -b -i "$FAT" "::roms/$low" 2>/dev/null | sed 's/^/    /'; done
fi

# ---- 2. ext4 root partition -----------------------------------------------------
echo "mkimage: extracting $ROOTFS"
TREE=$WORK/tree
mkdir -p "$TREE"
tar -xpf "$ROOTFS" -C "$TREE" --numeric-owner
USED_KIB=$(du -sk "$TREE" | cut -f1)
# content + 12% for metadata/rounding + requested free space
EXT_MIB=$(( (USED_KIB * 112 / 100 + 1023) / 1024 + FREE_MIB ))
echo "mkimage: rootfs content ${USED_KIB} KiB -> ext4 of ${EXT_MIB} MiB"
EXT=$WORK/root.img
truncate -s "${EXT_MIB}M" "$EXT"
# -m 1: small reserved-blocks share (this is a tiny single-purpose system).
mke2fs -q -t ext4 -L OLPCROOT -m 1 -E root_owner=0:0 -d "$TREE" "$EXT"

# ---- 3. Partition table + assembly ---------------------------------------------
P1_START=$ALIGN_SECT
P1_SECT=$(( FAT_MIB * 2048 ))
P2_START=$(( P1_START + P1_SECT ))
P2_SECT=$(( EXT_MIB * 2048 ))
TOTAL_SECT=$(( P2_START + P2_SECT + ALIGN_SECT ))      # 1 MiB slack at the end
echo "mkimage: writing $IMG"
rm -f "$IMG"
truncate -s $(( TOTAL_SECT * 512 )) "$IMG"
sfdisk --quiet "$IMG" <<EOF
label: dos
unit: sectors

start=$P1_START, size=$P1_SECT, type=c, bootable
start=$P2_START, size=$P2_SECT, type=83
EOF
dd if="$FAT" of="$IMG" bs=1M seek=$(( P1_START / 2048 )) conv=notrunc status=none
dd if="$EXT" of="$IMG" bs=1M seek=$(( P2_START / 2048 )) conv=notrunc status=none
rm -f "$FAT" "$EXT"; rm -rf "$TREE"

# ---- 4. Verification (extract the partitions back out of the image) -------------
echo "mkimage: ---- verification ----"
sfdisk -l "$IMG"
dd if="$IMG" of="$WORK/p1.img" bs=1M skip=$(( P1_START / 2048 )) count=$FAT_MIB status=none
dd if="$IMG" of="$WORK/p2.img" bs=1M skip=$(( P2_START / 2048 )) count=$EXT_MIB status=none
echo "--- mdir ::boot"
mdir -i "$WORK/p1.img" ::boot
echo "--- mdir ::"
mdir -i "$WORK/p1.img" ::
mcopy -i "$WORK/p1.img" ::boot/BZIMAGE "$WORK/bz.check"
cmp "$WORK/bz.check" "$KERNEL"   || die "bzImage on FAT differs from input"
mcopy -i "$WORK/p1.img" ::boot/OLPC.FTH "$WORK/fth.check"
cmp "$WORK/fth.check" "$FTH"     || die "olpc.fth on FAT differs from input"
echo "--- blkid"
# busybox blkid has no -o/-s: parse the default output.
blk() { blkid "$1" | sed -n "s/.* $2=\"\([^\"]*\)\".*/\1/p"; }
L1=$(blk "$WORK/p1.img" LABEL); T1=$(blk "$WORK/p1.img" TYPE)
L2=$(blk "$WORK/p2.img" LABEL); T2=$(blk "$WORK/p2.img" TYPE)
echo "p1: LABEL=$L1 TYPE=$T1"; echo "p2: LABEL=$L2 TYPE=$T2"
[ "$L1" = OLPCLOG ]  && [ "$T1" = vfat ] || die "p1 label/type wrong"
[ "$L2" = OLPCROOT ] && [ "$T2" = ext4 ] || die "p2 label/type wrong"
echo "--- e2fsck -fn"
e2fsck -fn "$WORK/p2.img" || die "e2fsck reported problems"

# ---- 5. Ownership + size ---------------------------------------------------------
if [ -n "${HOST_UID:-}" ]; then chown "${HOST_UID}:${HOST_GID:-$HOST_UID}" "$IMG"; fi
SIZE=$(stat -c %s "$IMG")
echo "mkimage: OK  $IMG  $SIZE bytes ($(( SIZE / 1048576 )) MiB)"
