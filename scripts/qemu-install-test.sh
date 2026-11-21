#!/usr/bin/env bash
# qemu-install-test.sh        (HOST script)
# QEMU has no CaFe NAND and no Open Firmware, so the NAND is simulated (nandsim, 256 MiB, the XO-1
# geometry 2048+64 / 128 KiB scaled down; mtdparts like olpc.fth makes, with the sim's chip id) and
# the kernel gets the command lines of the olpc.fth files.  Nothing real is touched (images are copied).
#   1. INSTALL   out/olpc-boy-install.img: precheck, erase, write the real out/nand-root.jffs2, verify,
#                mount, copy the stick's data -> logs on the stick (INSTALL.TXT)
#   2. NAND BOOT kernel + initrd (cafe_nand's twin for nandsim) + the NAND cmdline of boot/olpc-nand.fth,
#                the simulated NAND pre-filled with the same image: initramfs loads the driver, mounts
#                JFFS2 as root, switch_root, OpenRC, the launcher starts (nandsim cannot keep its content
#                between QEMU runs, hence the pre-fill by the initramfs from a read-only second disk)
#   3. RESTORE   out/olpc-boy-restore.img + a fake backup stick (vfat OLPCNAND, read-only disk): verify
#                NANDCOPY.DAT, erase, write, read back; and once more with a corrupted NANDCOPY.DAT (abort)
# Prints the boot-to-menu time (kernel uptime of "olpc-menu start") from the NAND vs the USB image.
# Exit: 0 pass, 1 failure, 2 setup problem.   Env: TIMEOUT_INSTALL (default 2400) TIMEOUT_BOOT (600) TIMEOUT_RESTORE (1500)
#      RUNS="install boot restore"  which runs to do (boot needs only the installer's data, not run 1)
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd); cd "$REPO"
KERNEL=${KERNEL:-out/kernel/bzImage}
INSTALL_IMG=${INSTALL_IMG:-out/olpc-boy-install.img}
RESTORE_IMG=${RESTORE_IMG:-out/olpc-boy-restore.img}
SIM_INITRD=${SIM_INITRD:-out/nand-initrd-sim.img}
NAND_IMG=${NAND_IMG:-out/nand-root.jffs2}
QEMU_CPU=${QEMU_CPU:-pentium,+mmx,+cmov}
T_INSTALL=${TIMEOUT_INSTALL:-2400}
T_BOOT=${TIMEOUT_BOOT:-600}
T_RESTORE=${TIMEOUT_RESTORE:-1500}
RUNS=${RUNS:-install boot restore}
want() { case " $RUNS " in *" $1 "*) return 0 ;; esac; return 1; }
T_INST=- T_REST=- T_NAND_MENU="" T_FILL=0
die() { echo "qemu-install: ERROR: $*" >&2; exit 2; }
command -v qemu-system-i386 >/dev/null || die "qemu-system-i386 not installed"
command -v mcopy >/dev/null && command -v mformat >/dev/null || die "mtools not installed"
for f in "$KERNEL" "$INSTALL_IMG" "$RESTORE_IMG" "$SIM_INITRD" "$NAND_IMG"; do [ -s "$f" ] || die "missing $f (make install-image restore-image)"; done
export MTOOLS_SKIP_CHECK=1
FAIL=0
ok()  { echo "qemu-install: ok: $*"; }
nok() { echo "qemu-install: FAIL: $*"; FAIL=1; }

cmdline_of() { sed -n 's/^" \(.*\)" to boot-file[[:space:]]*$/\1/p' "$1" | tail -n1; }
SIMPARTS='mtdparts="NAND 256MiB 3,3V 8-bit:261632k(olpcboy),-(bbt)ro"'
simcmd() { echo "$(cmdline_of "$1" | sed 's/ mtdparts=[^ ]*//') $SIMPARTS olpcboy.nandsim console=ttyS0 loglevel=7 olpcboy.nobuttons $2"; }

# run_qemu TAG TIMEOUT STOP_REGEX CMDLINE [qemu args...]   (serial log out/qemu-install-TAG.serial.log)
run_qemu() {
	local tag=$1 to=$2 stop=$3 cmd=$4; shift 4
	local ser=out/qemu-install-$tag.serial.log qpid secs=0
	: >"$ser"
	set +e
	qemu-system-i386 -M pc -cpu "$QEMU_CPU" -m 256 -nographic -monitor none -no-reboot -serial "file:$ser" \
	    -kernel "$KERNEL" -append "$cmd" -usb "$@" </dev/null >/dev/null 2>"$ser.qemu-stderr" &
	qpid=$!
	while kill -0 "$qpid" 2>/dev/null && [ "$secs" -lt "$to" ]; do
		sleep 2; secs=$((secs + 2))
		if grep -aEq "$stop" "$ser" 2>/dev/null; then echo "qemu-install[$tag]: end marker seen after ${secs}s"; sleep 8; break; fi
	done
	if kill -0 "$qpid" 2>/dev/null; then kill "$qpid"; wait "$qpid" 2>/dev/null; echo "qemu-install[$tag]: stopped QEMU after ${secs}s"
	else wait "$qpid" 2>/dev/null; echo "qemu-install[$tag]: QEMU exited by itself after ${secs}s"; fi
	set -e
	RUNSECS=$secs
	grep -aqi 'invalid opcode' "$ser" && nok "$tag: invalid opcode" || true
	grep -aEq 'Kernel panic|Oops:|BUG:' "$ser" && nok "$tag: kernel panic/oops" || true
	[ -s "$ser.qemu-stderr" ] && head -n 5 "$ser.qemu-stderr" | sed 's/^/  qemu: /' || true
}
usbdisk() {   # usbdisk ID FILE [readonly]
	echo -drive; echo "if=none,id=$1,format=raw,file=$2${3:+,readonly=on}"; echo -device; echo "usb-storage,drive=$1"
}
mapfile_args() { local -n arr=$1; shift; arr=(); while IFS= read -r l; do arr+=("$l"); done < <("$@"); }

if want install; then
# ======================================================================== 1. install
echo "qemu-install: run 1: INSTALL into the simulated NAND"
cp --sparse=always "$INSTALL_IMG" out/qemu-install-install.img
mapfile_args D1 usbdisk stick out/qemu-install-install.img
run_qemu install "$T_INSTALL" '\[install\] (RESULT:|state:)' "$(simcmd install/olpc.fth olpcboy.install)" "${D1[@]}"
S=out/qemu-install-install.serial.log
T_INST=$RUNSECS
grep -a '\[install\]' "$S" | tail -n 30 | sed 's/^/  | /'
grep -aq '\[install\] layout ok' "$S" && ok "install: layout verified (mtd0 olpcboy rw, mtd1 bbt ro)" || nok "install: no 'layout ok'"
grep -aq '\[install\] precheck ok' "$S" && ok "install: precheck against the (sim) backup hashes passed" || nok "install: no precheck ok"
grep -aq '\[install\] RESULT: OK' "$S" && ok "install: RESULT: OK" || nok "install: no RESULT: OK"
for pat in 'erase: flash_erase -j' 'write: nandwrite of the JFFS2 image' 'readback: the written region equals the image' 'files: olpc-boy marker' \
	'files: /boot/' 'files: [0-9]* files mounted and hashed' 'files: no JFFS2 error'; do
	grep -aq "PASS  $pat" "$S" && ok "install: $pat" || nok "install: check missing/failed: $pat"
done
grep -a '\[install\] \(FAIL\|WARN\)' "$S" | head -n 5 | sed 's/^/  | /' || true
rm -rf out/qemu-install-install-logs; mkdir -p out/qemu-install-install-logs
mcopy -s -n -i out/qemu-install-install.img@@1M ::logs/ out/qemu-install-install-logs/ 2>/dev/null || true
find out/qemu-install-install-logs -name INSTALL.TXT | grep -q . && ok "install: INSTALL.TXT on the stick" || nok "install: no INSTALL.TXT"

fi

if want boot; then
# ======================================================================== 2. boot from the NAND
echo "qemu-install: run 2: boot from the (pre-filled) simulated NAND, no boot stick"
IMGSZ=$(stat -c %s "$NAND_IMG")
mapfile_args D2 usbdisk fill "$NAND_IMG" ro
run_qemu boot "$T_BOOT" 'tty1: olpc-menu start|tty1: status screen' "$(simcmd boot/olpc-nand.fth "olpcboy.nandsimfill=sda:$IMGSZ")" -initrd "$SIM_INITRD" "${D2[@]}"
B=out/qemu-install-boot.serial.log
grep -a 'olpc-boy\]\|nandroms\|REACHED\|olpc-menu\|jffs2\|JFFS2' "$B" | head -n 20 | sed 's/^/  | /'
grep -aq 'NAND boot: loading /nandmods/nandsim.ko' "$B" && ok "boot: the initramfs loaded the NAND driver from the external initrd" || nok "boot: driver not loaded from the initrd"
grep -aq 'NAND layout ok' "$B" && ok "boot: layout verified before the read-write mount" || nok "boot: no layout check"
grep -aq 'root mounted (jffs2 on /dev/mtdblock0)' "$B" && ok "boot: root mounted from JFFS2 (mtdblock0)" || nok "boot: JFFS2 root not mounted"
grep -aq 'boot dir is boot-0001' "$B" && ok "boot: boot dir boot-0001 on the NAND root (/data/olpclog/logs)" || nok "boot: no boot dir on the NAND"
grep -aq 'nandroms: /mnt/internal ready' "$B" && ok "boot: /data is bound on /mnt/internal (in /proc/mounts), ROM folders ready" || nok "boot: /mnt/internal not ready"
grep -aq 'REACHED_USERSPACE_OK' "$B" && ok "boot: REACHED_USERSPACE_OK" || nok "boot: userspace not reached"
grep -aq 'tty1: olpc-menu start' "$B" && ok "boot: the launcher (olpc-menu) started" || nok "boot: the menu did not start"
grep -aq 'ssh: \(listening\|skipped\)' "$B" && ok "boot: ssh service ran ($(grep -ao 'ssh: \(listening on tcp/22\|skipped\)' "$B" | head -n 1))" || nok "boot: no ssh line"
T_NAND_MENU=$(grep -a 'tty1: olpc-menu start' "$B" | head -n 1 | sed 's/^[^0-9]*\([0-9][0-9]*\).*/\1/')
# the QEMU-only pre-fill of the simulated NAND (initramfs, from the second disk) is not part of a real boot
T_FILL=$(awk '/initramfs: sim: writing/ { a = $1 } /initramfs: NAND layout ok/ { b = $1 } END { if (a != "" && b != "") printf "%d", b - a }' "$B")

fi

if want restore; then
# ======================================================================== 3. restore
echo "qemu-install: run 3: RESTORE from a fake backup stick (vfat OLPCNAND)"
BK=out/qemu-install-backup.img
mkbackup() {   # mkbackup CORRUPT
	rm -f "$BK"; truncate -s 64M "$BK"
	mformat -i "$BK" -v OLPCNAND -F ::
	head -c $((30 * 131072)) /dev/urandom >out/qemu-install-nandcopy.dat
	( cd out && sha256sum qemu-install-nandcopy.dat | sed 's/qemu-install-nandcopy.dat/NANDCOPY.DAT/' >qemu-install-sha256sums )
	[ "$1" = corrupt ] && printf 'X' | dd of=out/qemu-install-nandcopy.dat bs=1 seek=4096 conv=notrunc 2>/dev/null
	mmd -i "$BK" ::nandbackup-001
	mcopy -i "$BK" out/qemu-install-nandcopy.dat ::nandbackup-001/NANDCOPY.DAT
	mcopy -i "$BK" out/qemu-install-sha256sums ::nandbackup-001/SHA256SUMS
}
mkbackup good
cp --sparse=always "$RESTORE_IMG" out/qemu-install-restore.img
mapfile_args D3 usbdisk stick out/qemu-install-restore.img; mapfile_args D3b usbdisk backup "$BK" ro
run_qemu restore "$T_RESTORE" '\[install\] (RESULT:|state:)' "$(simcmd install/olpc-restore.fth olpcboy.restore)" "${D3[@]}" "${D3b[@]}"
R=out/qemu-install-restore.serial.log
T_REST=$RUNSECS
grep -a '\[install\]' "$R" | tail -n 14 | sed 's/^/  | /'
grep -aq '\[install\] restore: NANDCOPY.DAT verified' "$R" && ok "restore: NANDCOPY.DAT checked against SHA256SUMS" || nok "restore: no sha verification"
grep -aq 'PASS  restore: nandwrite of NANDCOPY.DAT' "$R" && ok "restore: erase -j + nandwrite -p done" || nok "restore: write missing"
grep -aq 'PASS  restore: read-back equals NANDCOPY.DAT' "$R" && ok "restore: the read-back equals NANDCOPY.DAT and the rest is erased" || nok "restore: read-back check missing/failed"
grep -aq '\[install\] RESULT: OK' "$R" && ok "restore: RESULT: OK" || nok "restore: no RESULT: OK"
grep -aq 'backup stick is /dev/sd' "$R" && grep -aq 'mount.*OLPCNAND\|restore: backup stick' "$R" && ok "restore: the backup stick was found by its label" || nok "restore: stick not found"
rm -rf out/qemu-install-restore-logs; mkdir -p out/qemu-install-restore-logs
mcopy -s -n -i out/qemu-install-restore.img@@1M ::logs/ out/qemu-install-restore-logs/ 2>/dev/null || true
find out/qemu-install-restore-logs -name RESTORE.TXT | grep -q . && ok "restore: RESTORE.TXT on the stick" || nok "restore: no RESTORE.TXT"

echo "qemu-install: run 4: RESTORE with a corrupted NANDCOPY.DAT (must abort before erasing)"
mkbackup corrupt
cp --sparse=always "$RESTORE_IMG" out/qemu-install-restore2.img
mapfile_args D4 usbdisk stick out/qemu-install-restore2.img; mapfile_args D4b usbdisk backup "$BK" ro
run_qemu restore2 300 '\[install\] state:' "$(simcmd install/olpc-restore.fth olpcboy.restore)" "${D4[@]}" "${D4b[@]}"
R2=out/qemu-install-restore2.serial.log
grep -a '\[install\]' "$R2" | tail -n 4 | sed 's/^/  | /'
grep -aq 'does not match SHA256SUMS' "$R2" && ok "restore: a corrupted NANDCOPY.DAT is refused" || nok "restore2: corruption not detected"
grep -aq 'state: the NAND was NOT written' "$R2" && ! grep -aq 'restore: erase mtd0' "$R2" && ok "restore: nothing was erased or written" || nok "restore2: the NAND was touched"

fi

# ======================================================================== timing
T_USB_MENU=""
if [ -f out/qemu-logs/logs/boot-0001/timeline.log ]; then T_USB_MENU=$(grep 'tty1: olpc-menu start' out/qemu-logs/logs/boot-0001/timeline.log | head -n 1 | cut -d' ' -f1 | cut -d. -f1); fi
echo "qemu-install: timing (QEMU/TCG, indicative only): boot to 'olpc-menu start': NAND ${T_NAND_MENU:-?} s of uptime (minus ${T_FILL:-0} s QEMU-only pre-fill of the sim NAND), USB image ${T_USB_MENU:-n/a (run make qemu first)} s; install run ${T_INST}s, restore run ${T_REST}s wall"
[ "$FAIL" = 0 ] && echo "qemu-install: PASS" || { echo "qemu-install: FAILED"; exit 1; }
