#!/bin/bash
# Build out/rootfs.tar (Alpine 3.18 x86 root filesystem for olpc-boy).
# Run inside the olpc-boy-builder container (needs root for apk --root):
#   docker run --rm -v $PWD:/work -w /work -e HOST_UID=$(id -u) -e HOST_GID=$(id -g) \
#       olpc-boy-builder bash scripts/mkrootfs.sh
set -eu

cd "$(dirname "$0")/.."
ROOT=$PWD
WORK=$ROOT/out/rootfs-work
R=$WORK/root
CACHE=$WORK/apk-cache
MAIN=https://dl-cdn.alpinelinux.org/alpine/v3.18/main
COMM=https://dl-cdn.alpinelinux.org/alpine/v3.18/community
KMOD_SRC=$ROOT/out/kernel/modules/lib/modules

[ "$(id -u)" = 0 ] || { echo "ERROR: must run as root (container)" >&2; exit 1; }

echo "== packages"
PKGS=$(sed 's/#.*//' rootfs/packages | tr -s ' \t\n' ' ')
echo "$PKGS"

rm -rf "$R"
mkdir -p "$R/etc/apk/keys" "$CACHE"
cp /etc/apk/keys/* "$R/etc/apk/keys/"
printf '%s\n%s\n' "$MAIN" "$COMM" >"$R/etc/apk/repositories"

echo "== apk add"
# shellcheck disable=SC2086
apk --root "$R" --arch x86 --initdb --cache-dir "$CACHE" -U --no-progress \
	-X "$MAIN" -X "$COMM" add $PKGS

echo "== overlay"
# cp without -p: files become root-owned, modes (exec bits) preserved
cp -R --no-dereference rootfs/overlay/. "$R/"
chown -R 0:0 "$R"
chmod 0700 "$R/root"
find "$R/usr/local/sbin" "$R/usr/libexec/olpcboy" "$R/etc/init.d" -type f \
	! -name common.sh -exec chmod 0755 {} +
chmod 0644 "$R/usr/libexec/olpcboy/common.sh"
# keep empty overlay dirs (git does not track them)
mkdir -p "$R/mnt/olpclog" "$R/var/log/olpc-fallback" "$R/run" "$R/tmp" "$R/proc" "$R/sys" "$R/dev"
chmod 1777 "$R/tmp"

echo "== system config"
echo olpcboy >"$R/etc/hostname"
# root: no password
if grep -q '^root:' "$R/etc/shadow"; then
	sed -i 's/^root:[^:]*:/root::/' "$R/etc/shadow"
fi
# openrc: log every service's output into the boot dir (symlink made by the
# initramfs), core size limit for services, no hotplug of wifi, keep it simple
cat >>"$R/etc/rc.conf" <<'RC'

# ---- olpc-boy ----
rc_logger="YES"
rc_log_path="/var/log/olpc-current/rc.log"
rc_ulimit="-c 8192"
rc_parallel="NO"
rc_interactive="NO"
rc_shell=/bin/sh
RC
# the Alpine default inittab is replaced by the overlay; busybox init needs
# /etc/inittab only.  Console login restrictions: none (getty -n)
: >"$R/etc/securetty"
sed -i 's/^tty/#tty/' "$R/etc/securetty" 2>/dev/null || true

echo "== olpc-chordd"
# Logic test first (host-independent, fast), then the static i386 build for the Geode.
sh src/olpc-chordd/test.sh >"$WORK/chordd-test.log" 2>&1 \
	|| { cat "$WORK/chordd-test.log" >&2; echo "ERROR: olpc-chordd logic test failed" >&2; exit 1; }
tail -n1 "$WORK/chordd-test.log"
make -C src/olpc-chordd OUT="$R/usr/local/sbin" CFLAGS="-Os -march=geode -mtune=geode -Wall -Wextra"
chmod 0755 "$R/usr/local/sbin/olpc-chordd"
file "$R/usr/local/sbin/olpc-chordd" | sed "s/^/  /"

echo "== olpc-fbbench"
sh src/olpc-fbbench/test.sh >"$WORK/fbbench-test.log" 2>&1 \
	|| { cat "$WORK/fbbench-test.log" >&2; echo "ERROR: olpc-fbbench scaling self-test failed" >&2; exit 1; }
tail -n2 "$WORK/fbbench-test.log"
make -C src/olpc-fbbench OUT="$R/usr/local/sbin" CFLAGS="-O2 -march=geode -mtune=geode -Wall -Wextra"
chmod 0755 "$R/usr/local/sbin/olpc-fbbench"
file "$R/usr/local/sbin/olpc-fbbench" | sed "s/^/  /"

# Emulator cores + headless speed test (built separately by src/cores/build.sh).
# ROMs for the on-device speed test are optional and local only: they come from
# out/cores/real-roms (user supplied) or out/cores/test-roms (free test ROMs).
if [ -f out/cores/coretest ]; then
	echo "== libretro cores"
	mkdir -p "$R/usr/lib/libretro" "$R/usr/share/olpcboy/bench"
	cp out/cores/*_libretro.so "$R/usr/lib/libretro/"
	install -m 0755 out/cores/coretest "$R/usr/local/bin/coretest" 2>/dev/null \
		|| { mkdir -p "$R/usr/local/bin"; install -m 0755 out/cores/coretest "$R/usr/local/bin/coretest"; }
	for d in out/cores/real-roms out/cores/test-roms; do
		[ -d "$d" ] || continue
		find "$d" -type f \( -name '*.gb' -o -name '*.gbc' -o -name '*.nes' \) | while read -r f; do
			case $f in *.gbc) n=gbc ;; *.gb) n=gb ;; *) n=nes ;; esac
			[ -e "$R/usr/share/olpcboy/bench/rom.$n" ] || cp "$f" "$R/usr/share/olpcboy/bench/rom.$n"
		done
	done
	ls -l "$R/usr/lib/libretro" "$R/usr/share/olpcboy/bench" | sed "s/^/  /"
fi

echo "== runlevels"
RL=$R/etc/runlevels
rm -rf "$RL"
mkdir -p "$RL/sysinit" "$RL/boot" "$RL/default" "$RL/shutdown" "$RL/nonetwork"
addsvc() {  # addsvc <runlevel> <svc...>
	lvl=$1; shift
	for s in "$@"; do
		if [ -e "$R/etc/init.d/$s" ]; then
			ln -sf "/etc/init.d/$s" "$RL/$lvl/$s"
		else
			echo "WARNING: init script $s not found (runlevel $lvl)" >&2
		fi
	done
}
addsvc sysinit olpclog devfs dmesg mdev hwdrivers sysfs procfs olpc-watchdog olpc-keymap olpc-dcon
addsvc boot modules sysctl hostname hwclock bootmisc syslog root localmount
# olpc-inventory is NOT a service: olpc-bootdone starts it (background, nice 19)
# after REACHED_USERSPACE_OK.
addsvc default olpc-powerbtn olpc-chordd olpc-bootdone
addsvc shutdown mount-ro killprocs savecache
# NOT enabled on purpose: networking wpa_supplicant dropbear acpid crond

echo "== firmware (libertas)"
for fw in usb8388_v9.bin usb8388_olpc.bin; do
	if [ -e "$R/lib/firmware/libertas/$fw" ]; then
		echo "  $fw: from linux-firmware-libertas"
	else
		echo "  $fw: missing from package, downloading from linux-firmware git"
		mkdir -p "$R/lib/firmware/libertas"
		curl -fsSL -o "$R/lib/firmware/libertas/$fw" \
		  "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/libertas/$fw" \
		  || echo "WARNING: could not download $fw" >&2
	fi
done

# linux-firmware-libertas depends on linux-firmware-mrvl (83 MB of firmware for
# cards the XO-1 does not have).  Only libertas/ is needed: drop mrvl/.
rm -rf "$R/lib/firmware/mrvl"

echo "== kernel modules"
KVER=""
if [ -d "$KMOD_SRC" ] && [ -n "$(ls -A "$KMOD_SRC" 2>/dev/null)" ]; then
	for kd in "$KMOD_SRC"/*; do
		[ -d "$kd" ] || continue
		KVER=$(basename "$kd")
		mkdir -p "$R/lib/modules"
		rm -rf "$R/lib/modules/$KVER"
		cp -a "$kd" "$R/lib/modules/$KVER"
		rm -f "$R/lib/modules/$KVER/build" "$R/lib/modules/$KVER/source"
		if depmod -b "$R" "$KVER"; then echo "  depmod ok for $KVER"
		else echo "WARNING: depmod failed for $KVER" >&2; fi
	done
else
	echo "WARNING: no kernel modules at $KMOD_SRC; rootfs has none (rerun after the kernel build)" >&2
fi

echo "== sanity"
for f in sbin/init sbin/openrc bin/sh etc/inittab usr/local/sbin/olpc-selftest \
	usr/bin/evtest usr/bin/aplay sbin/getty; do
	[ -e "$R/$f" ] || [ -L "$R/$f" ] || echo "WARNING: $f missing in rootfs" >&2
done
# syntax-check our own scripts with the target's busybox ash
for f in $(find "$R/usr/local/sbin" "$R/usr/libexec/olpcboy" "$R/etc/init.d" -type f); do
	head -n1 "$f" | grep -q '^#!' || continue
	case "$(head -n1 "$f")" in
	*openrc-run*|*/bin/sh*) busybox ash -n "$f" || { echo "ERROR: syntax $f" >&2; exit 1; } ;;
	esac
done

echo "== normalise mtimes (OpenRC clock-skew)"
# The XO-1 RTC says 2013 (or 1970), the build host says 2026.  Files newer than
# the system clock make OpenRC print "clock skew detected" and rebuild its
# dependency cache before EVERY service (~3 s each on the Geode: most of the
# first boot's userland time).  Put every mtime in the distant past instead.
find "$R" -xdev -exec touch -h -d '1970-01-02 12:00:00' {} + 2>/dev/null || true

echo "== NAND policy checks"
grep -q '^install cafe_nand /bin/false' "$R/etc/modprobe.d/olpcboy.conf" \
	|| { echo "ERROR: cafe_nand install-guard missing from modprobe.d" >&2; exit 1; }
# busybox ships NAND/flash applets: drop their links so nothing can run them by accident
for a in nanddump nandwrite flash_eraseall flash_lock flash_unlock flashcp ubiattach ubidetach \
	ubimkvol ubirmvol ubirsvol ubiupdatevol; do
	find "$R/bin" "$R/sbin" "$R/usr/bin" "$R/usr/sbin" -maxdepth 1 -name "$a" -type l -delete 2>/dev/null
done
for f in usr/sbin/nanddump usr/sbin/nandwrite usr/sbin/flash_erase usr/sbin/nandtest; do
	[ -e "$R/$f" ] && { echo "ERROR: $f present in the rootfs" >&2; exit 1; }
done
# no overlay script may reference an MTD device node outside comments
if grep -rsnE '^[^#]*(/dev/mtd|mtdblock)' "$R/usr/local/sbin" "$R/usr/libexec/olpcboy" "$R/etc/init.d/olpc"* "$R/etc/init.d/olpclog" | grep .; then
	echo "ERROR: an overlay script references /dev/mtd*" >&2; exit 1
fi

echo "== tar"
rm -f "$ROOT/out/rootfs.tar"
tar --numeric-owner -C "$R" -cpf "$ROOT/out/rootfs.tar" .
chown "${HOST_UID:-0}:${HOST_GID:-0}" "$ROOT/out/rootfs.tar"

echo "== report"
NPKG=$(grep -c '^P:' "$R/lib/apk/db/installed")
echo "packages installed: $NPKG"
echo "rootfs size:        $(du -sm "$R" | cut -f1) MB (tar: $(du -sm "$ROOT/out/rootfs.tar" | cut -f1) MB)"
echo "15 largest packages (installed size):"
awk '/^P:/{p=substr($0,3)} /^I:/{print $0 " " p}' "$R/lib/apk/db/installed" \
	| sed 's/^I://' | sort -rn | head -n 15 \
	| awk '{printf "  %8.1f KB  %s\n", $1/1024, $2}'
echo "i386 check:"
file "$R/bin/busybox" "$R/sbin/apk" | sed 's/^/  /'
