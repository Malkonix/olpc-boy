#!/bin/bash
# mkbackup.sh -- build out/backup-rootfs.tar, the root of the read-only NAND backup image
# (runs inside the builder container as root; the normal rootfs is not touched).
# Inputs : backup/packages backup/overlay backup/src out/kernel (cafe_nand.ko, bzImage version)
# Output : out/backup-rootfs.tar
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD
WORK=$ROOT/out/backup-work
R=$WORK/root
CACHE=$ROOT/out/rootfs-work/apk-cache
MAIN=https://dl-cdn.alpinelinux.org/alpine/v3.18/main
COMM=https://dl-cdn.alpinelinux.org/alpine/v3.18/community
KMOD_SRC=$ROOT/out/kernel/modules/lib/modules
CFLAGS="-Os -march=geode -mtune=geode -Wall -Wextra"
[ "$(id -u)" = 0 ] || { echo "ERROR: must run as root (container)" >&2; exit 1; }
mkdir -p "$WORK"

echo "== backup logic tests"
sh backup/test.sh >"$WORK.test.log" 2>&1 \
	|| { cat "$WORK.test.log" >&2; echo "ERROR: backup tests failed" >&2; exit 1; }
tail -n1 "$WORK.test.log"

PKGS=$(sed 's/#.*//' backup/packages | tr -s ' \t\n' ' ')
rm -rf "$R"; mkdir -p "$R/etc/apk/keys" "$CACHE"
cp /etc/apk/keys/* "$R/etc/apk/keys/"
printf '%s\n%s\n' "$MAIN" "$COMM" >"$R/etc/apk/repositories"
echo "== apk add: $PKGS"
# shellcheck disable=SC2086
apk --root "$R" --arch x86 --initdb --cache-dir "$CACHE" -U --no-progress -X "$MAIN" -X "$COMM" add $PKGS

echo "== overlay"
cp -R --no-dereference backup/overlay/. "$R/"
mkdir -p "$R/usr/libexec/olpcboy"
cp rootfs/overlay/usr/libexec/olpcboy/common.sh "$R/usr/libexec/olpcboy/common.sh"
chown -R 0:0 "$R"
chmod 0755 "$R/usr/local/sbin/olpc-nandbackup"
chmod 0644 "$R/usr/libexec/olpcboy/common.sh"
mkdir -p "$R/mnt/olpclog" "$R/var/log/olpc-fallback" "$R/run" "$R/tmp" "$R/proc" "$R/sys" "$R/dev" "$R/root"
chmod 1777 "$R/tmp"; chmod 0700 "$R/root"
echo olpcboy-nandbackup >"$R/etc/hostname"
: >"$R/etc/securetty"

echo "== nbhash, nbcompact, nbcmp + olpc-splash"
for t in nbhash nbcompact nbcmp; do
	gcc $CFLAGS -static -o "$R/usr/local/sbin/$t" backup/src/$t.c
	strip "$R/usr/local/sbin/$t"
	chmod 0755 "$R/usr/local/sbin/$t"
done
make -C src/olpc-splash OUT="$R/usr/local/sbin" CFLAGS="$CFLAGS"
chmod 0755 "$R/usr/local/sbin/olpc-splash"
file "$R/usr/local/sbin/nbhash" | sed 's/^/  /'

echo "== only nanddump from mtd-utils-nand"
for f in nandwrite nandtest nandflipbits nftl_format nftldump; do rm -f "$R/usr/sbin/$f"; done
for a in nanddump nandwrite flash_eraseall flash_erase flash_lock flash_unlock flashcp ubiattach ubidetach \
	ubimkvol ubirmvol ubirsvol ubiupdatevol; do
	find "$R/bin" "$R/sbin" "$R/usr/bin" "$R/usr/sbin" -maxdepth 1 -name "$a" -type l -delete 2>/dev/null
done
# no package manager needed at run time
rm -rf "$R/sbin/apk" "$R/etc/apk" "$R/var/cache"

echo "== kernel module (cafe_nand only)"
KVER=$(ls "$KMOD_SRC" | head -n1)
KO=$(find "$KMOD_SRC/$KVER" -name cafe_nand.ko | head -n1)
[ -n "$KO" ] || { echo "ERROR: cafe_nand.ko not found (build the kernel first)" >&2; exit 1; }
mkdir -p "$R/lib/modules/$KVER/kernel/drivers/mtd/nand/raw"
cp "$KO" "$R/lib/modules/$KVER/kernel/drivers/mtd/nand/raw/"
for f in modules.builtin modules.builtin.modinfo modules.order; do
	[ -f "$KMOD_SRC/$KVER/$f" ] && cp "$KMOD_SRC/$KVER/$f" "$R/lib/modules/$KVER/"
done
depmod -b "$R" "$KVER"
grep -q cafe_nand "$R/lib/modules/$KVER/modules.dep" || { echo "ERROR: depmod lost cafe_nand" >&2; exit 1; }

echo "== sanity + NAND write-tool policy"
for f in sbin/init bin/sh sbin/modprobe usr/sbin/nanddump usr/local/sbin/nbhash usr/local/sbin/nbcompact usr/local/sbin/nbcmp usr/local/sbin/olpc-splash \
	usr/local/sbin/olpc-nandbackup etc/inittab usr/bin/sha256sum bin/df bin/stat bin/dmesg usr/bin/awk sbin/blkid; do
	[ -e "$R/$f" ] || [ -L "$R/$f" ] || { echo "ERROR: $f missing in the backup rootfs" >&2; exit 1; }
done
busybox ash -n "$R/usr/local/sbin/olpc-nandbackup" || { echo "ERROR: syntax olpc-nandbackup" >&2; exit 1; }
for f in nandwrite nandtest nandflipbits flash_erase flash_eraseall flashcp flash_lock flash_unlock mtd_debug \
	ubiformat ubiupdatevol mkfs.jffs2 jffs2dump sumtool nftl_format; do
	found=$(find "$R" \( -name "$f" -o -name "$f.*" \) 2>/dev/null | head -n1)
	[ -z "$found" ] || { echo "ERROR: write/erase tool present in the backup rootfs: $found" >&2; exit 1; }
done
grep -q '^install cafe_nand /bin/false' "$R/etc/modprobe.d/nandbackup.conf" \
	|| { echo "ERROR: cafe_nand install-guard missing" >&2; exit 1; }
# ROMs and WiFi credentials never go into this image
[ ! -e "$R/WIFI.CFG" ] && ! find "$R" -iname 'wifi.cfg' | grep -q . || { echo "ERROR: wifi.cfg in the rootfs" >&2; exit 1; }
find "$R" -xdev -exec touch -h -d '1970-01-02 12:00:00' {} + 2>/dev/null || true

echo "== tar"
rm -f out/backup-rootfs.tar
tar --numeric-owner -C "$R" -cpf out/backup-rootfs.tar .
chown "${HOST_UID:-0}:${HOST_GID:-0}" out/backup-rootfs.tar
echo "backup rootfs: $(du -sm "$R" | cut -f1) MB; packages: $(grep -c '^P:' "$R/lib/apk/db/installed" 2>/dev/null || echo '?')"
