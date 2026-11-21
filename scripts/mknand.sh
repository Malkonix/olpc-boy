#!/bin/bash
# mknand.sh -- build the content of the internal NAND of an XO-1 (runs in the builder container as root).
#
# The whole NAND is ONE JFFS2 (like the original OS), made from the same rootfs as the USB image:
#   /boot/olpc.fth  /boot/bzImage  /boot/initrd.img   what Open Firmware reads (nand:\boot\...)
#   /.olpcboy-nand                                    marker the installer looks for in erase block 0
#   /data/{olpclog,roms,covers}                       persistent data (see rootfs/README.md)
#   everything else of out/rootfs.tar
# mkfs.jffs2 (128 KiB erase blocks, 2 KiB pages, no cleanmarkers, zlib/rtime only: what Open
# Firmware can read) + sumtool (summary nodes: the kernel mounts without scanning every node).
#
# Inputs : out/rootfs.tar  out/kernel/bzImage  out/kernel/modules  boot/olpc-nand.fth
# Outputs: out/nand-root.jffs2      trimmed to the last used erase block
#          out/nand-root.sha256     "sha256  ./path" of every regular file of the tree
#          out/nand-initrd.img      cpio.gz with cafe_nand.ko (boot/initrd.img on the NAND)
#          out/nand-initrd-sim.img  same with nandsim.ko (QEMU tests only)
#          out/nand-root.info       sizes
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=out
ERASE=131072 PAGE=2048
ROOTFS=${ROOTFS_TAR:-$OUT/rootfs.tar}
KERNEL=${KERNEL_IMG:-$OUT/kernel/bzImage}
FTH=${NAND_FTH:-boot/olpc-nand.fth}
KMOD_SRC=$OUT/kernel/modules/lib/modules
MARK=OLPCBOY-NAND-INSTALL-1
die() { echo "mknand: ERROR: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "must run as root (container)"
for f in "$ROOTFS" "$KERNEL" "$FTH"; do [ -s "$f" ] || die "missing input $f"; done
for t in mkfs.jffs2 sumtool jffs2dump cpio gzip gcc sha256sum; do command -v "$t" >/dev/null || die "tool not found: $t"; done
head -n1 "$FTH" | grep -q '^\\ ' || die "$FTH must start with a '\\ ' comment line"
grep -q 'nand:\\boot\\bzImage' "$FTH" && grep -q 'nand:\\boot\\initrd.img' "$FTH" || die "$FTH does not load nand:\\boot\\bzImage and initrd.img"

WORK=$(mktemp -d /tmp/mknand.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
T=$WORK/tree
mkdir -p "$T"

# ---- initrd(s): the NAND driver module only (the kernel merges it over the embedded initramfs) ----
KVER=$(ls "$KMOD_SRC" | head -n1)
[ -n "$KVER" ] || die "no kernel modules in $KMOD_SRC (build the kernel first)"
mkinitrd() {   # mkinitrd MODULE OUTFILE
	local d=$WORK/initrd.$1 ko
	ko=$(find "$KMOD_SRC/$KVER" -name "$1.ko" | head -n1)
	[ -n "$ko" ] || die "$1.ko not found"
	rm -rf "$d"; mkdir -p "$d/nandmods"
	cp "$ko" "$d/nandmods/$1.ko"
	( cd "$d" && find . -mindepth 1 | sort | cpio -o -H newc --quiet --owner 0:0 | gzip -9n ) >"$2"
	[ -s "$2" ] || die "empty initrd $2"
}
mkinitrd cafe_nand "$OUT/nand-initrd.img"
mkinitrd nandsim "$OUT/nand-initrd-sim.img"
echo "mknand: initrd $(wc -c <"$OUT/nand-initrd.img") bytes (cafe_nand.ko), sim $(wc -c <"$OUT/nand-initrd-sim.img") bytes"

# ---- the tree ---------------------------------------------------------------------------------
echo "mknand: extracting $ROOTFS"
tar -xpf "$ROOTFS" -C "$T" --numeric-owner
[ -x "$T/sbin/init" ] || [ -L "$T/sbin/init" ] || die "rootfs has no /sbin/init"
mkdir -p "$T/boot" "$T/data/olpclog" "$T/data/roms/gb" "$T/data/roms/gbc" "$T/data/roms/nes" \
	"$T/data/roms/saves" "$T/data/roms/states" "$T/data/covers" "$T/mnt/internal"
install -m 0644 "$FTH" "$T/boot/olpc.fth"
install -m 0644 "$KERNEL" "$T/boot/bzImage"
install -m 0644 "$OUT/nand-initrd.img" "$T/boot/initrd.img"
printf '%s\nkernel %s\n' "$MARK" "$(sha256sum "$KERNEL" | cut -d' ' -f1)" >"$T/.olpcboy-nand"
chmod 0644 "$T/.olpcboy-nand"
chmod 0700 "$T/root" 2>/dev/null || true
find "$T/boot" "$T/data" "$T/mnt/internal" "$T/.olpcboy-nand" -exec touch -h -d '1970-01-02 12:00:00' {} +
# no secrets of the build host in the image
find "$T" \( -iname 'wifi.cfg' -o -name '*_host_key' \) | grep . && die "credentials/keys inside the tree" || true
USED_KIB=$(du -sk "$T" | cut -f1)

# ---- manifest (what the installer verifies after writing) --------------------------------------
( cd "$T" && find . -type f -print0 | sort -z | xargs -0 sha256sum ) >"$OUT/nand-root.sha256"
NFILES=$(wc -l <"$OUT/nand-root.sha256")
[ "$NFILES" -gt 100 ] || die "manifest has only $NFILES files"
[ "$(find "$T" -type f | wc -l)" = "$NFILES" ] || die "manifest/file count mismatch (file names with newlines?)"

# ---- JFFS2 --------------------------------------------------------------------------------------
echo "mknand: mkfs.jffs2 + sumtool (tree $USED_KIB KiB, $NFILES files)"
COMPR=$(mkfs.jffs2 -L 2>&1 || true)
echo "$COMPR" | grep -q 'lzo.*disabled' || die "mkfs.jffs2: lzo is enabled by default (Open Firmware cannot read it)"
mkfs.jffs2 -r "$T" -o "$WORK/raw.jffs2" -e "$ERASE" -s "$PAGE" -l -n -p
sumtool -i "$WORK/raw.jffs2" -o "$WORK/img.jffs2" -e "$ERASE" -l -n -p
gcc -O2 -Wall -o "$WORK/nbjffs" install/src/nbjffs.c
read -r BLOCKS USED SUMMED LAST < <("$WORK/nbjffs" sum "$ERASE" <"$WORK/img.jffs2")
[ "$USED" -gt 0 ] && [ "$LAST" -gt 0 ] || die "empty image"
head -c $(( LAST * ERASE )) "$WORK/img.jffs2" >"$OUT/nand-root.jffs2"
IMG=$(wc -c <"$OUT/nand-root.jffs2")
[ $(( IMG % ERASE )) = 0 ] || die "image size $IMG is not a multiple of the erase block"
# every erase block that holds data must end with a summary node (summary is what keeps the mount fast);
# sumtool leaves the last, partly filled block without one (the kernel scans that single block)
[ "$SUMMED" -ge $(( USED - 1 )) ] && [ "$SUMMED" -ge 1 ] || die "summary nodes: $SUMMED of $USED used erase blocks"
# erase block 0 carries the marker (mkfs.jffs2 writes the root directory entries first, sorted)
BLK0=$(dd if="$OUT/nand-root.jffs2" bs="$ERASE" count=1 2>/dev/null | strings -a | grep -c "$MARK" || true)
[ "$BLK0" -ge 1 ] || die "marker not inside erase block 0"
BLK0=$(dd if="$OUT/nand-root.jffs2" bs="$ERASE" count=1 2>/dev/null | strings -a | grep -c '\.olpcboy-nand' || true)
[ "$BLK0" -ge 1 ] || die "marker file name not inside erase block 0"
jffs2dump -c "$OUT/nand-root.jffs2" >"$WORK/dump.txt" 2>&1 || die "jffs2dump -c failed"
JERR=$(grep -vE '^ +(Dirent|Inode|Cleanmarker|Summary|Padding) ' "$WORK/dump.txt" | grep -iE 'error|crc|wrong|bad|unknown|corrupt' | head -n 3 || true)
[ -z "$JERR" ] || die "jffs2dump reports errors: $JERR"
{
	echo "tree_kib=$USED_KIB files=$NFILES"
	echo "image_bytes=$IMG image_blocks=$(( IMG / ERASE )) summed_blocks=$SUMMED"
	echo "kernel_bytes=$(wc -c <"$KERNEL") initrd_bytes=$(wc -c <"$OUT/nand-initrd.img")"
} >"$OUT/nand-root.info"
cat "$OUT/nand-root.info"
chown "${HOST_UID:-0}:${HOST_GID:-0}" "$OUT/nand-root.jffs2" "$OUT/nand-root.sha256" "$OUT/nand-initrd.img" "$OUT/nand-initrd-sim.img" "$OUT/nand-root.info"
echo "mknand: out/nand-root.jffs2 $IMG bytes ($(( IMG / ERASE )) erase blocks)"
