#!/usr/bin/env bash
# collect-logs.sh [device|mountpoint]      (HOST script)
#
# Copies logs/ from the OLPCLOG partition of the test stick into
# ./logs/<YYYYmmdd-HHMMSS>/ (never deletes anything on the stick) and prints a
# compact per-boot summary.
#   no argument : the USB disk whose serial is stored in .flash-device
#   /dev/sdX    : that disk (or a partition labelled OLPCLOG)
#   /some/dir   : an already mounted OLPCLOG (or the logs/ dir itself)
# The stick is mounted read-only (udisksctl if available, else sudo mount) and
# unmounted again if this script mounted it.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
die() { echo "collect-logs: ERROR: $*" >&2; exit 1; }

ARG=${1:-}
MNT=""; MOUNTED_BY=""; TMPMNT=""; PART=""
cleanup() {
    case $MOUNTED_BY in
        udisks) udisksctl unmount -b "$PART" >/dev/null 2>&1 || echo "collect-logs: warning: could not unmount $PART" >&2 ;;
        sudo)   sudo umount "$TMPMNT" 2>/dev/null && rmdir "$TMPMNT" || echo "collect-logs: warning: could not unmount $TMPMNT" >&2 ;;
    esac
}
trap cleanup EXIT

# ---- 1. locate the source ---------------------------------------------------------
if [ -n "$ARG" ] && [ -d "$ARG" ]; then
    MNT=$ARG                                   # already a directory
else
    if [ -z "$ARG" ]; then
        [ -f "$REPO/.flash-device" ] || die "no device given and no $REPO/.flash-device (flash a stick with --remember first)"
        SERIAL=$(sed -n 's/^serial=//p' "$REPO/.flash-device" | head -n1)
        DISK=$(lsblk -dnpo PATH,TRAN,SERIAL | awk -v s="$SERIAL" '$2=="usb" && $3==s {print $1; exit}')
        [ -n "$DISK" ] || die "stick with serial '$SERIAL' is not plugged in"
    else
        [ -b "$ARG" ] || die "$ARG is neither a directory nor a block device"
        DISK=$ARG
    fi
    # the partition labelled OLPCLOG on that disk (or the node itself if it is one)
    PART=$(lsblk -nrpo PATH,LABEL "$DISK" | awk '$2=="OLPCLOG" {print $1; exit}')
    [ -n "$PART" ] || die "no partition labelled OLPCLOG on $DISK"
    MNT=$(findmnt -nro TARGET -S "$PART" 2>/dev/null | head -n1 || true)
    if [ -z "$MNT" ]; then
        if command -v udisksctl >/dev/null && out=$(udisksctl mount -b "$PART" -o ro 2>&1); then
            MNT=$(printf '%s\n' "$out" | sed -n 's/.* at \(.*\)\.$/\1/p')
            MOUNTED_BY=udisks
        else
            TMPMNT=$(mktemp -d /tmp/olpclog.XXXXXX)
            sudo mount -o ro "$PART" "$TMPMNT" || die "could not mount $PART"
            MNT=$TMPMNT; MOUNTED_BY=sudo
        fi
    fi
fi
[ -d "$MNT/logs" ] && SRC=$MNT/logs || SRC=$MNT
[ -d "$SRC" ] || die "no logs/ directory under $MNT"

# ---- 2. copy (read-only on the stick, never --delete) -------------------------------------
TS=$(date +%Y%m%d-%H%M%S)
DEST=$REPO/logs/$TS
mkdir -p "$DEST"
if command -v rsync >/dev/null; then
    rsync -rt --no-perms --no-owner --no-group "$SRC"/ "$DEST"/
else
    cp -r --no-preserve=mode,ownership "$SRC"/. "$DEST"/
fi
echo "collect-logs: copied $SRC -> $DEST"
cleanup; MOUNTED_BY=""                         # release the stick as early as possible

# ---- 3. summary -----------------------------------------------------------------------------
PATTERN='invalid opcode|Oops|BUG:|panic|segfault|Call Trace|I/O error'
shopt -s nullglob
BOOTS=("$DEST"/boot-*/)
[ ${#BOOTS[@]} -gt 0 ] || { echo "collect-logs: no boot-NNNN directories found (the system never wrote logs?)"; exit 0; }

for b in "${BOOTS[@]}"; do
    b=${b%/}; name=$(basename "$b")
    echo
    echo "=================== $name"
    # markers: empty marker files in the boot dir
    printf 'markers:'
    for m in STARTED REACHED_USERSPACE_OK PREVIOUS_BOOT_INCOMPLETE WATCHDOG_TIMEOUT; do
        if [ -e "$b/$m" ]; then printf ' %s=yes' "$m"; else printf ' %s=no' "$m"; fi
    done
    echo
    # selftest results: any selftest* file, lines carrying PASS / FAIL
    st=("$b"/selftest*)
    if [ ${#st[@]} -gt 0 ]; then
        np=$(cat "${st[@]}" 2>/dev/null | grep -cw 'PASS' || true)
        nf=$(cat "${st[@]}" 2>/dev/null | grep -cw 'FAIL' || true)
        echo "selftest: PASS=$np FAIL=$nf"
        cat "${st[@]}" 2>/dev/null | grep -w 'FAIL' | sed 's/^/  /' || true
    else
        echo "selftest: (no selftest* file)"
    fi
    # kernel log tail + interesting lines
    if [ -f "$b/kmsg.log" ]; then
        echo "--- kmsg.log (last 20 of $(wc -l < "$b/kmsg.log"))"
        tail -n 20 "$b/kmsg.log" | sed 's/^/  /'
    else
        echo "--- kmsg.log: missing"
    fi
    echo "--- suspicious lines"
    hits=$(grep -rEHn --include='*.log' --include='*.txt' "$PATTERN" "$b" 2>/dev/null | sed "s#^$b/##" | head -n 40 || true)
    if [ -n "$hits" ]; then echo "$hits" | sed 's/^/  /'; else echo "  (none)"; fi
    # core files
    cores=$(find "$b" \( -name 'core' -o -name 'core.*' -o -name '*.core' \) -type f 2>/dev/null | head -n 20)
    if [ -n "$cores" ]; then echo "core files: YES"; echo "$cores" | sed "s#^$b/#  #"; else echo "core files: none"; fi
done
echo
echo "collect-logs: done -> $DEST"
