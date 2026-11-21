#!/usr/bin/env bash
# qemu-nandtest-test.sh [timeout_seconds_per_run]        (HOST script)
# Boots out/olpc-boy-nandtest.img in QEMU (the image is copied; no real device is touched), twice:
#   1. as shipped (cmdline of nandtest/olpc.fth): QEMU has no CaFe NAND, so the flow must stop safely
#      with "no NAND found" before any write.
#   2. with olpcboy.nandsim and an mtdparts line for the simulated NAND (nandsim, 256 MiB, same
#      2048/64/128 KiB geometry, window = blocks 100..163): steps 0..6 end to end with the same
#      image, the real flash_erase/nandwrite/nanddump and the real JFFS2 driver.
# Exit: 0 pass, 1 failure, 2 setup problem.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd); cd "$REPO"
KERNEL=${KERNEL:-out/kernel/bzImage}
IMAGE=${IMAGE:-out/olpc-boy-nandtest.img}
T1=${1:-${TIMEOUT:-240}}
T2=${TIMEOUT_SIM:-1500}
QEMU_CPU=${QEMU_CPU:-pentium,+mmx,+cmov}
FTH=nandtest/olpc.fth
die() { echo "qemu-nandtest: ERROR: $*" >&2; exit 2; }
command -v qemu-system-i386 >/dev/null || die "qemu-system-i386 not installed"
command -v mcopy >/dev/null || die "mtools not installed"
[ -s "$KERNEL" ] && [ -s "$IMAGE" ] || die "missing $KERNEL or $IMAGE (make nandtest-image)"
CMDLINE=$(sed -n 's/^" \(.*\)" to boot-file[[:space:]]*$/\1/p' "$FTH" | tail -n1)
[ -n "$CMDLINE" ] || die "no command line in $FTH"
export MTOOLS_SKIP_CHECK=1
FAIL=0
ok()   { echo "qemu-nandtest: ok: $*"; }
nok()  { echo "qemu-nandtest: FAIL: $*"; FAIL=1; }

# run_qemu TAG CMDLINE TIMEOUT STOP_REGEX
run_qemu() {
	local tag=$1 cmd=$2 to=$3 stop=$4 img=out/qemu-nandtest-$1.img ser=out/qemu-nandtest-$1.serial.log
	cp --sparse=always "$IMAGE" "$img"
	: >"$ser"
	set +e
	qemu-system-i386 -M pc -cpu "$QEMU_CPU" -m 256 -nographic -monitor none -no-reboot \
	    -serial "file:$ser" -kernel "$KERNEL" -append "$cmd" \
	    -usb -drive if=none,id=stick,format=raw,file="$img" -device usb-storage,drive=stick \
	    </dev/null >/dev/null 2>"$ser.qemu-stderr" &
	local qpid=$! secs=0
	while kill -0 "$qpid" 2>/dev/null && [ "$secs" -lt "$to" ]; do
		sleep 2; secs=$((secs + 2))
		if grep -aEq "$stop" "$ser" 2>/dev/null; then echo "qemu-nandtest[$tag]: end marker seen after ${secs}s"; sleep 6; break; fi
	done
	if kill -0 "$qpid" 2>/dev/null; then kill "$qpid"; wait "$qpid" 2>/dev/null; else wait "$qpid" 2>/dev/null; fi
	set -e
	echo "qemu-nandtest[$tag]: ${secs}s; serial tail:"
	grep -a 'nandtest\]\|Kernel panic' "$ser" | tail -n 8 | sed 's/^/  | /'
	rm -rf "out/qemu-nandtest-$tag-logs"; mkdir -p "out/qemu-nandtest-$tag-logs"
	mcopy -s -n -i "$img@@1M" ::logs/ "out/qemu-nandtest-$tag-logs/" 2>/dev/null || true
	grep -aqi 'invalid opcode' "$ser" && nok "$tag: invalid opcode" || true
	grep -aEq 'Kernel panic|Oops:|BUG:' "$ser" && nok "$tag: kernel panic/oops" || true
}

# ---- 1. no NAND --------------------------------------------------------------------------
echo "qemu-nandtest: run 1: shipped command line (no NAND)"
run_qemu real "$CMDLINE console=ttyS0 loglevel=7" "$T1" 'nandtest\] FATAL'
S=out/qemu-nandtest-real.serial.log
grep -aq 'nandtest\] FATAL: no NAND found' "$S" && ok "no NAND: stopped with 'no NAND found'" || nok "no 'no NAND found' line"
grep -aq 'nandtest\] state: the test window was NOT written' "$S" && ok "no NAND: reported that nothing was written" || nok "no 'NOT written' line"
grep -aq 'RESULT: OK' "$S" && nok "no NAND: a RESULT: OK appeared" || ok "no NAND: no result line"
find out/qemu-nandtest-real-logs -name NANDTEST.txt | grep -q . && ok "NANDTEST.txt on the boot stick" || nok "no NANDTEST.txt"
grep -aq 'layout ok' "$S" && nok "layout was verified without a NAND" || true

# ---- 2. simulated NAND -------------------------------------------------------------------------
echo "qemu-nandtest: run 2: nandsim (simulated NAND), full flow"
SIMCMD=$(echo "$CMDLINE" | sed 's/ mtdparts=[^ ]*//; s/ olpcboy.nandtest//')
SIMCMD="$SIMCMD mtdparts=\"NAND 256MiB 3,3V 8-bit:12800k(pre)ro,8192k(test),-(post)ro\" olpcboy.nandtest olpcboy.nandsim console=ttyS0 loglevel=7"
run_qemu sim "$SIMCMD" "$T2" 'nandtest\] RESULT:'
S=out/qemu-nandtest-sim.serial.log
grep -aq 'nandtest\] layout ok' "$S" && ok "sim: layout verified (mtd0/mtd2 read-only, mtd1 writeable)" || nok "sim: no 'layout ok'"
grep -aq 'nandtest\] precheck ok' "$S" && ok "sim: precheck passed" || nok "sim: no precheck ok"
grep -aq 'nandtest\] RESULT: OK' "$S" && ok "sim: RESULT: OK" || nok "sim: no RESULT: OK"
for pat in 'erase: ECC read all 0xFF' 'random: ECC read-back identical' 'zero: ECC read-back identical' 'original data returned for 1 + 2 flipped bits' \
	'files verified after umount + remount' 'install: mounted' 'cleanmarkers and ECC area identical' 'final: all 2048 erase blocks equal the backup'; do
	grep -aq "PASS  .*$pat" "$S" && ok "sim: $pat" || nok "sim: check missing/failed: $pat"
done
grep -a 'nandtest\] FAIL' "$S" | head -n 5 | sed 's/^/  | /'
find out/qemu-nandtest-sim-logs -name NANDTEST.txt | grep -q . && ok "sim: NANDTEST.txt on the boot stick" || nok "sim: no NANDTEST.txt"
[ "$FAIL" = 0 ] && echo "qemu-nandtest: PASS" || { echo "qemu-nandtest: FAILED"; exit 1; }
