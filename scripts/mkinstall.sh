#!/bin/bash
# mkinstall.sh -- build out/install-rootfs.tar, the root of the NAND installer / restore sticks
# (runs inside the builder container as root; the normal rootfs is not touched).
# Inputs : install/{packages,overlay,src,mkexpected.sh,test.sh}  backup/src (nbhash, nbcmp)  nandtest/src (nbtool)
#          out/kernel (modules)  out/nand-root.{jffs2,sha256} (scripts/mknand.sh)
#          the NAND backup, mounted READ-ONLY at /nandbackup (make passes NAND_BACKUP_DIR)
# Output : out/install-rootfs.tar
#          (the per-block hashes of the backup go into the image; the backup itself never does)
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD
WORK=$ROOT/out/install-work
R=$WORK/root
CACHE=$ROOT/out/rootfs-work/apk-cache
MAIN=https://dl-cdn.alpinelinux.org/alpine/v3.18/main
COMM=https://dl-cdn.alpinelinux.org/alpine/v3.18/community
KMOD_SRC=$ROOT/out/kernel/modules/lib/modules
BACKUP=${NAND_BACKUP_MNT:-/nandbackup}
CFLAGS="-Os -march=geode -mtune=geode -Wall -Wextra"
die() { echo "ERROR: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "must run as root (container)"
[ -r "$BACKUP/NANDCOPY.DAT" ] || die "NANDCOPY.DAT not found in the NAND backup directory (needed for the restored-layout gate)"
[ -r "$BACKUP/nand-data-ecc.bin" ] || die "NAND backup not found: need nand-data-ecc.bin in the directory given as NAND_BACKUP_DIR"
for f in out/nand-root.jffs2 out/nand-root.sha256; do [ -s "$f" ] || die "missing $f (run scripts/mknand.sh)"; done
mkdir -p "$WORK"

echo "== install logic tests"
sh install/test.sh >"$WORK.test.log" 2>&1 \
	|| { cat "$WORK.test.log" >&2; die "install tests failed"; }
tail -n1 "$WORK.test.log"

PKGS=$(sed 's/#.*//' install/packages | tr -s ' \t\n' ' ')
rm -rf "$R"; mkdir -p "$R/etc/apk/keys" "$CACHE"
cp /etc/apk/keys/* "$R/etc/apk/keys/"
printf '%s\n%s\n' "$MAIN" "$COMM" >"$R/etc/apk/repositories"
echo "== apk add: $PKGS"
# shellcheck disable=SC2086
apk --root "$R" --arch x86 --initdb --cache-dir "$CACHE" -U --no-progress -X "$MAIN" -X "$COMM" add $PKGS

echo "== overlay"
cp -R --no-dereference install/overlay/. "$R/"
mkdir -p "$R/usr/libexec/olpcboy"
cp rootfs/overlay/usr/libexec/olpcboy/common.sh "$R/usr/libexec/olpcboy/common.sh"
chown -R 0:0 "$R"
chmod 0755 "$R/usr/local/sbin/olpc-install"
chmod 0644 "$R/usr/libexec/olpcboy/common.sh"
mkdir -p "$R/mnt/olpclog" "$R/var/log/olpc-fallback" "$R/run" "$R/tmp" "$R/proc" "$R/sys" "$R/dev" "$R/root"
chmod 1777 "$R/tmp"; chmod 0700 "$R/root"
echo olpcboy-install >"$R/etc/hostname"
: >"$R/etc/securetty"

echo "== nbhash, nbcmp, nbtool, nbjffs + olpc-splash"
for s in backup/src/nbhash.c backup/src/nbcmp.c nandtest/src/nbtool.c install/src/nbjffs.c; do
	t=$(basename "$s" .c)
	gcc $CFLAGS -static -o "$R/usr/local/sbin/$t" "$s"
	strip "$R/usr/local/sbin/$t"
	chmod 0755 "$R/usr/local/sbin/$t"
done
make -C src/olpc-splash OUT="$R/usr/local/sbin" CFLAGS="$CFLAGS"
chmod 0755 "$R/usr/local/sbin/olpc-splash"

echo "== only nanddump, nandwrite and flash_erase from mtd-utils"
for f in nandtest nandflipbits nftl_format nftldump flash_eraseall flash_lock flash_unlock flashcp \
	flash_otp_dump flash_otp_erase flash_otp_info flash_otp_lock flash_otp_write; do rm -f "$R/usr/sbin/$f"; done
for a in flash_eraseall flash_lock flash_unlock flashcp ubiattach ubidetach ubimkvol ubirmvol ubirsvol ubiupdatevol; do
	find "$R/bin" "$R/sbin" "$R/usr/bin" "$R/usr/sbin" -maxdepth 1 -name "$a" -type l -delete 2>/dev/null || true
done
rm -rf "$R/sbin/apk" "$R/etc/apk" "$R/var/cache"

echo "== kernel modules (cafe_nand, nandsim)"
KVER=$(ls "$KMOD_SRC" | head -n1)
mkdir -p "$R/lib/modules/$KVER/kernel/drivers/mtd/nand/raw"
for m in cafe_nand nandsim; do
	KO=$(find "$KMOD_SRC/$KVER" -name "$m.ko" | head -n1)
	[ -n "$KO" ] || die "$m.ko not found (build the kernel first)"
	cp "$KO" "$R/lib/modules/$KVER/kernel/drivers/mtd/nand/raw/"
done
for f in modules.builtin modules.builtin.modinfo modules.order; do
	[ -f "$KMOD_SRC/$KVER/$f" ] && cp "$KMOD_SRC/$KVER/$f" "$R/lib/modules/$KVER/"
done
depmod -b "$R" "$KVER"
grep -q cafe_nand "$R/lib/modules/$KVER/modules.dep" && grep -q nandsim "$R/lib/modules/$KVER/modules.dep" || die "depmod lost a NAND module"

echo "== payload: NAND image + reference data from the NAND backup ($BACKUP, read-only)"
D=$R/usr/share/olpc-install
mkdir -p "$D"
gcc -O2 -o "$WORK/nbhash.host" backup/src/nbhash.c
gcc -O2 -o "$WORK/nbtool.host" nandtest/src/nbtool.c
NBHASH=$WORK/nbhash.host NBTOOL=$WORK/nbtool.host sh install/mkexpected.sh "$BACKUP" "$D" || die "reference data generation failed"
[ -s "$D/expected-blocks-restored.txt" ] || die "restored-layout list missing (NANDCOPY.DAT not in the backup directory?)"
cp out/nand-root.jffs2 out/nand-root.sha256 "$D/"
chown -R 0:0 "$D"; chmod -R a+rX "$D"

echo "== sanity + policy"
for f in sbin/init bin/sh sbin/modprobe usr/sbin/nanddump usr/sbin/nandwrite usr/sbin/flash_erase usr/local/sbin/nbhash usr/local/sbin/nbcmp \
	usr/local/sbin/nbtool usr/local/sbin/nbjffs usr/local/sbin/olpc-splash usr/local/sbin/olpc-install etc/inittab usr/bin/sha256sum bin/df bin/mount bin/umount \
	bin/dmesg usr/bin/awk sbin/blkid usr/bin/find usr/bin/xargs usr/bin/wc bin/dd usr/bin/tr usr/bin/head usr/bin/tail usr/bin/strings bin/cp usr/bin/diff \
	usr/share/olpc-install/expected-blocks.txt usr/share/olpc-install/sim/expected-blocks.txt usr/share/olpc-install/nand-root.jffs2 usr/share/olpc-install/nand-root.sha256; do
	[ -e "$R/$f" ] || [ -L "$R/$f" ] || die "$f missing in the installer rootfs"
done
busybox ash -n "$R/usr/local/sbin/olpc-install" || die "syntax olpc-install"
for f in flash_eraseall flashcp flash_lock flash_unlock mtd_debug ubiformat ubiupdatevol nandtest nandflipbits nftl_format mkfs.jffs2 sumtool jffs2dump; do
	found=$(find "$R" -not -type d -name "$f" 2>/dev/null | head -n1)
	[ -z "$found" ] || die "unneeded tool present in the installer rootfs: $found"
done
grep -q '^install cafe_nand /bin/false' "$R/etc/modprobe.d/install.conf" || die "cafe_nand install-guard missing"
find "$R" -iname 'wifi.cfg' | grep -q . && die "wifi.cfg in the rootfs" || true
# the backup itself never goes into the image, only per-block hashes
find "$R" -type f -size +64M | grep . && die "unexpectedly large file in the rootfs" || true
find "$R" -xdev -exec touch -h -d '1970-01-02 12:00:00' {} + 2>/dev/null || true

echo "== tar"
rm -f out/install-rootfs.tar
tar --numeric-owner -C "$R" -cpf out/install-rootfs.tar .
chown "${HOST_UID:-0}:${HOST_GID:-0}" out/install-rootfs.tar
echo "install rootfs: $(du -sm "$R" | cut -f1) MB"
