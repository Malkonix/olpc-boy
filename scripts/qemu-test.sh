#!/usr/bin/env bash
# qemu-test.sh [timeout_seconds]        (HOST script)
#
# Smoke test: boots out/kernel/bzImage with out/olpc-boy.img attached as a USB
# stick in QEMU on a CPU model WITHOUT SSE (so stray SSE code dies with
# "invalid opcode"), then pulls logs/ from the FAT partition of a COPY of the
# image and checks for boot-0001 with STARTED + REACHED_USERSPACE_OK.
#
# Note: the olpc.fth is not executed (QEMU has no Open Firmware); instead the
# kernel gets the same command line olpc.fth sets, plus console=ttyS0 and
# olpcboy.nobuttons.
#
# Env: QEMU_CPU   (default "pentium,+mmx,+cmov": this QEMU has no pentium-mmx model)
#      KERNEL / IMAGE   override input paths   TIMEOUT  seconds (default 180)
#      STOP_ON  regex: end the run early once the serial log matches it
#      ANALYZE_ONLY=1  skip the boot, only analyse the existing out/qemu-test.img
# Exit: 0 pass, 1 test failure, 2 setup problem.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"
KERNEL=${KERNEL:-out/kernel/bzImage}
IMAGE=${IMAGE:-out/olpc-boy.img}
TIMEOUT=${1:-${TIMEOUT:-180}}
QEMU_CPU=${QEMU_CPU:-pentium,+mmx,+cmov}
COPY=out/qemu-test.img
SERIAL=out/qemu-serial.log
LOGDIR=out/qemu-logs
STOP_ON=${STOP_ON:-}
FTH=boot/olpc.fth

die() { echo "qemu-test: ERROR: $*" >&2; exit 2; }
command -v qemu-system-i386 >/dev/null || die "qemu-system-i386 not installed"
command -v mcopy >/dev/null            || die "mtools not installed"
[ -f "$FTH" ] || die "$FTH missing"

# Kernel command line = the one olpc.fth sets (the line ending in 'to boot-file').
CMDLINE=$(sed -n 's/^" \(.*\)" to boot-file[[:space:]]*$/\1/p' "$FTH" | tail -n1)
[ -n "$CMDLINE" ] || die "could not extract the command line from $FTH"
CMDLINE="$CMDLINE console=ttyS0 olpcboy.nobuttons"

mkdir -p out
if [ "${ANALYZE_ONLY:-0}" != 1 ]; then
    [ -s "$KERNEL" ] || die "missing $KERNEL (make kernel)"
    [ -s "$IMAGE" ]  || die "missing $IMAGE (make image)"
    cp --sparse=always "$IMAGE" "$COPY"        # the pristine image is never booted
    : > "$SERIAL"
    echo "qemu-test: cpu=$QEMU_CPU timeout=${TIMEOUT}s"
    echo "qemu-test: cmdline: $CMDLINE"
    set +e
    qemu-system-i386 -M pc -cpu "$QEMU_CPU" -m 256 -nographic -monitor none -no-reboot \
        -serial "file:$SERIAL" \
        -kernel "$KERNEL" -append "$CMDLINE" \
        -usb -drive if=none,id=stick,format=raw,file="$COPY" -device usb-storage,drive=stick \
        </dev/null >/dev/null 2>"$SERIAL.qemu-stderr" &
    QPID=$!
    # Wait for exit (poweroff/panic with -no-reboot), the timeout, or STOP_ON.
    SECS=0
    while kill -0 "$QPID" 2>/dev/null && [ "$SECS" -lt "$TIMEOUT" ]; do
        sleep 1; SECS=$((SECS + 1))
        if [ -n "$STOP_ON" ] && grep -Eq "$STOP_ON" "$SERIAL" 2>/dev/null; then
            echo "qemu-test: STOP_ON matched after ${SECS}s"; sleep 3; break
        fi
    done
    if kill -0 "$QPID" 2>/dev/null; then
        kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
        echo "qemu-test: stopped QEMU after ${SECS}s"
    else
        wait "$QPID"; echo "qemu-test: QEMU exited by itself after ${SECS}s (rc=$?)"
    fi
    set -e
    [ ! -s "$SERIAL.qemu-stderr" ] || { echo "qemu stderr:"; head -n 20 "$SERIAL.qemu-stderr"; }
fi

# ---- analysis ------------------------------------------------------------------
[ -f "$COPY" ] || die "no $COPY to analyse"
echo "qemu-test: serial log: $SERIAL ($(wc -l < "$SERIAL") lines); last 15:"
tail -n 15 "$SERIAL" | sed 's/^/  | /'

# Partition 1 starts at 1 MiB (see mkimage.sh); mtools offset syntax img@@bytes.
export MTOOLS_SKIP_CHECK=1
P1="$COPY@@1M"
rm -rf "$LOGDIR"; mkdir -p "$LOGDIR"
FAIL=0
if mcopy -s -n -i "$P1" ::logs/ "$LOGDIR/" 2>/dev/null || [ -n "$(ls -A "$LOGDIR" 2>/dev/null)" ]; then
    echo "qemu-test: logs/ from the stick:"; (cd "$LOGDIR" && find . -maxdepth 2 | sort | sed 's/^/  /')
else
    echo "qemu-test: FAIL: could not read logs/ from the FAT partition"; FAIL=1
fi
B=$(find "$LOGDIR" -maxdepth 2 -type d -name boot-0001 | head -n1)
if [ -z "$B" ]; then
    echo "qemu-test: FAIL: no boot-0001 directory"; FAIL=1
else
    for m in STARTED REACHED_USERSPACE_OK; do
        if [ -e "$B/$m" ]; then echo "qemu-test: marker $m present"
        else echo "qemu-test: FAIL: marker $m missing in boot-0001"; FAIL=1; fi
    done
fi
if grep -rqi 'invalid opcode' "$SERIAL" "$LOGDIR" 2>/dev/null; then
    echo "qemu-test: FAIL: 'invalid opcode' found (SSE or other unsupported instruction!):"
    grep -rniH 'invalid opcode' "$SERIAL" "$LOGDIR" | head -n 10 | sed 's/^/  /'
    FAIL=1
else
    echo "qemu-test: no 'invalid opcode' seen"
fi
if grep -Eq 'Kernel panic|Oops:|BUG:' "$SERIAL"; then
    echo "qemu-test: FAIL: kernel panic/oops on the serial console"; FAIL=1
fi

if [ "$FAIL" -eq 0 ]; then echo "qemu-test: PASS"; else echo "qemu-test: FAILED"; exit 1; fi
