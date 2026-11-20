#!/usr/bin/env bash
# qemu-backup-test.sh [timeout_seconds]        (HOST script)
# Boots out/olpc-boy-nandbackup.img in QEMU as the boot stick, with a second, sparse FAT32 disk
# image labelled OLPCNAND attached as the backup stick (both are files: no real device is touched).
# QEMU has no CaFe NAND, so the flow must find and mount the OLPCNAND volume, then stop cleanly
# with "no NAND found", writing nothing but an empty nandbackup-001/ folder and the logs.
# Exit: 0 pass, 1 failure, 2 setup problem.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd); cd "$REPO"
KERNEL=${KERNEL:-out/kernel/bzImage}
IMAGE=${IMAGE:-out/olpc-boy-nandbackup.img}
TIMEOUT=${1:-${TIMEOUT:-240}}
QEMU_CPU=${QEMU_CPU:-pentium,+mmx,+cmov}
COPY=out/qemu-backup-test.img
NANDBK=out/qemu-backup-stick.img
SERIAL=out/qemu-backup-serial.log
LOGDIR=out/qemu-backup-logs
FTH=backup/olpc.fth
die() { echo "qemu-backup-test: ERROR: $*" >&2; exit 2; }
command -v qemu-system-i386 >/dev/null || die "qemu-system-i386 not installed"
command -v mcopy >/dev/null || die "mtools not installed"
command -v mkfs.vfat >/dev/null || die "mkfs.vfat not installed"
[ -s "$KERNEL" ] && [ -s "$IMAGE" ] || die "missing $KERNEL or $IMAGE (make backup-image)"
CMDLINE=$(sed -n 's/^" \(.*\)" to boot-file[[:space:]]*$/\1/p' "$FTH" | tail -n1)
[ -n "$CMDLINE" ] || die "no command line in $FTH"
CMDLINE="$CMDLINE console=ttyS0 loglevel=7"
export MTOOLS_SKIP_CHECK=1
cp --sparse=always "$IMAGE" "$COPY"
rm -f "$NANDBK"; truncate -s 1300M "$NANDBK"          # sparse; free space just above the raw-dump need
mkfs.vfat -F 32 -n OLPCNAND "$NANDBK" >/dev/null
: >"$SERIAL"
set +e
qemu-system-i386 -M pc -cpu "$QEMU_CPU" -m 256 -nographic -monitor none -no-reboot \
    -serial "file:$SERIAL" -kernel "$KERNEL" -append "$CMDLINE" \
    -usb -drive if=none,id=stick,format=raw,file="$COPY" -device usb-storage,drive=stick \
    -drive if=none,id=bk,format=raw,file="$NANDBK" -device usb-storage,drive=bk \
    </dev/null >/dev/null 2>"$SERIAL.qemu-stderr" &
QPID=$!
SECS=0
while kill -0 "$QPID" 2>/dev/null && [ "$SECS" -lt "$TIMEOUT" ]; do
    sleep 1; SECS=$((SECS + 1))
    if grep -q 'nandbackup\] FAILED' "$SERIAL" 2>/dev/null; then echo "qemu-backup-test: failure line seen after ${SECS}s"; sleep 4; break; fi
done
kill -0 "$QPID" 2>/dev/null && { kill "$QPID"; wait "$QPID" 2>/dev/null; } || wait "$QPID" 2>/dev/null
set -e
echo "qemu-backup-test: serial tail:"; grep -a 'nandbackup\|Kernel panic' "$SERIAL" | tail -n 12 | sed 's/^/  | /'
FAIL=0
P1="$COPY@@1M"
rm -rf "$LOGDIR"; mkdir -p "$LOGDIR"
mcopy -s -n -i "$P1" ::logs/ "$LOGDIR/" 2>/dev/null || true
grep -aq 'nandbackup\] backup stick: /dev/sd[a-z]*[0-9]* (label OLPCNAND)' "$SERIAL" \
    && echo "qemu-backup-test: ok: found the OLPCNAND volume by label" || { echo "qemu-backup-test: FAIL: OLPCNAND not found"; FAIL=1; }
grep -aq 'nandbackup\] destination: .*nandbackup-001' "$SERIAL" \
    && echo "qemu-backup-test: ok: mounted it and passed the space check" || { echo "qemu-backup-test: FAIL: no destination line"; FAIL=1; }
grep -aq 'nandbackup\] FAILED: no NAND found' "$SERIAL" && echo "qemu-backup-test: ok: stopped cleanly with 'no NAND found' after the mount" \
    || { echo "qemu-backup-test: FAIL: no clean 'no NAND found' line"; FAIL=1; }
if mdir -b -s -i "$NANDBK" :: 2>/dev/null | grep -iq '\.bin\|NANDCOPY\|MANIFEST'; then echo "qemu-backup-test: FAIL: a dump file exists without a NAND"; FAIL=1
else echo "qemu-backup-test: ok: no dump files written"; fi
mdir -b -i "$NANDBK" :: 2>/dev/null | grep -qi nandbackup-001 && echo "qemu-backup-test: ok: empty nandbackup-001/ created on the OLPCNAND stick" || { echo "qemu-backup-test: FAIL: no nandbackup-001"; FAIL=1; }
find "$LOGDIR" -name 'nandbackup.log' | grep -q . && echo "qemu-backup-test: ok: logs/boot-NNNN/nandbackup.log on the boot stick" || { echo "qemu-backup-test: FAIL: no nandbackup.log"; FAIL=1; }
grep -aqi 'invalid opcode' "$SERIAL" && { echo "qemu-backup-test: FAIL: invalid opcode"; FAIL=1; } || echo "qemu-backup-test: no 'invalid opcode' seen"
grep -aEq 'Kernel panic|Oops:|BUG:' "$SERIAL" && { echo "qemu-backup-test: FAIL: kernel panic/oops"; FAIL=1; }
[ "$FAIL" = 0 ] && echo "qemu-backup-test: PASS" || { echo "qemu-backup-test: FAILED"; exit 1; }
