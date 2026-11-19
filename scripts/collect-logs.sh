#!/usr/bin/env bash
# collect-logs.sh [device|mountpoint]      (HOST script)
#
# Copies logs/ from the OLPCLOG partition of the test stick into
# ./logs/<YYYYmmdd-HHMMSS>/ (never deletes anything on the stick) and prints a
# compact per-boot summary.
#   no argument : the USB disk whose serial is stored in .flash-device
#   /dev/sdX    : that disk (or a partition labelled OLPCLOG)
#   /some/dir   : an already mounted OLPCLOG (or the logs/ dir itself)
# Only USB disks are considered; the stick is mounted read-only (udisksctl, else sudo) and
# unmounted again only if this script mounted it.
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
# Only USB disks are ever considered (never the system disk), always read-only.

# mountpoint of a block device, parsed from lsblk (raw output escapes spaces as \x20)
mountpoint_of() {
    lsblk -nro MOUNTPOINTS "$1" 2>/dev/null | awk 'NF {print; exit}' | while IFS= read -r l; do printf '%b\n' "$l"; done
}
# transport of the disk that holds $1 (a disk or a partition)
tran_of() {
    local d=$1 p
    p=$(lsblk -dno PKNAME "$d" 2>/dev/null | head -n1)
    [ -n "$p" ] && d=/dev/$p
    lsblk -dno TRAN "$d" 2>/dev/null | head -n1 | tr -d '[:space:]'
}
# OLPCLOG partitions on USB disks:  "<partition> <serial>"
olpclog_candidates() {
    local disk tran serial part
    while read -r disk tran serial; do
        [ "$tran" = usb ] || continue
        part=$(lsblk -nrpo PATH,LABEL,TYPE "$disk" 2>/dev/null | awk '$2=="OLPCLOG" && $3=="part" {print $1; exit}')
        [ -n "$part" ] && echo "$part $serial"
    done < <(lsblk -dnrpo PATH,TRAN,SERIAL 2>/dev/null)
}

if [ -n "$ARG" ] && [ -d "$ARG" ]; then
    MNT=$ARG                                   # already a directory
else
    if [ -z "$ARG" ]; then
        SERIAL=""
        [ -f "$REPO/.flash-device" ] && SERIAL=$(sed -n 's/^serial=//p' "$REPO/.flash-device" | head -n1)
        CANDS=$(olpclog_candidates)
        [ -n "$CANDS" ] || die "no USB disk with a partition labelled OLPCLOG is plugged in"
        # 1st choice: the serial remembered by flash.sh; 2nd: the label alone if unambiguous
        if [ -n "$SERIAL" ]; then PART=$(printf '%s\n' "$CANDS" | awk -v s="$SERIAL" '$2==s {print $1; exit}'); fi
        if [ -z "$PART" ]; then
            [ "$(printf '%s\n' "$CANDS" | wc -l)" -eq 1 ] || die "several USB disks have an OLPCLOG partition and none matches serial '${SERIAL:-?}'; pass the device"
            PART=$(printf '%s\n' "$CANDS" | awk '{print $1}')
            echo "collect-logs: serial '${SERIAL:-none}' not found, using the only OLPCLOG partition on USB: $PART" >&2
        fi
    else
        [ -b "$ARG" ] || die "$ARG is neither a directory nor a block device"
        [ "$(tran_of "$ARG")" = usb ] || die "$ARG is not on a USB disk (refusing)"
        # the partition labelled OLPCLOG on that disk (or the node itself if it is one)
        if [ "$(lsblk -dno LABEL "$ARG" 2>/dev/null | head -n1)" = OLPCLOG ]; then PART=$ARG
        else PART=$(lsblk -nrpo PATH,LABEL,TYPE "$ARG" | awk '$2=="OLPCLOG" && $3=="part" {print $1; exit}'); fi
    fi
    [ -n "$PART" ] || die "no partition labelled OLPCLOG found"
    [ "$(tran_of "$PART")" = usb ] || die "$PART is not on a USB disk (refusing)"
    MNT=$(mountpoint_of "$PART")
    if [ -z "$MNT" ]; then
        if command -v udisksctl >/dev/null && udisksctl mount -b "$PART" -o ro >/dev/null 2>&1; then
            MNT=$(mountpoint_of "$PART")
            [ -n "$MNT" ] && MOUNTED_BY=udisks
        fi
        if [ -z "$MNT" ]; then
            TMPMNT=$(mktemp -d /tmp/olpclog.XXXXXX)
            if sudo mount -o ro "$PART" "$TMPMNT"; then MNT=$TMPMNT; MOUNTED_BY=sudo
            else rmdir "$TMPMNT"; die "could not mount $PART read-only"; fi
        fi
    fi
    [ -n "$MNT" ] || die "mounted $PART but could not determine its mount point"
fi
[ -d "$MNT/logs" ] && SRC=$MNT/logs || SRC=$MNT
[ -d "$SRC" ] || die "no logs/ directory under '$MNT' (partition ${PART:-?})"

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
PATTERN='invalid opcode|Oops|BUG:|Kernel panic|panic:|segfault|Call Trace|I/O error'
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
    # selftest results: only real result lines ("<uptime> PASS|FAIL|SKIP name ..."),
    # from selftest.log alone (summary copies are '# SUMMARY' lines, not counted)
    if [ -f "$b/selftest.log" ]; then
        np=$(grep -cE '^[0-9.]+ +PASS ' "$b/selftest.log" || true)
        nf=$(grep -cE '^[0-9.]+ +FAIL ' "$b/selftest.log" || true)
        ns=$(grep -cE '^[0-9.]+ +SKIP ' "$b/selftest.log" || true)
        echo "selftest: PASS=$np FAIL=$nf SKIP=$ns"
        grep -E '^[0-9.]+ +FAIL ' "$b/selftest.log" | sed 's/^/  /' || true
    else
        echo "selftest: (no selftest.log)"
    fi
    # buttons capture
    if [ -f "$b/buttons.log" ]; then
        echo "buttons: $(grep -c ' value=' "$b/buttons.log" || true) events; keys pressed:"
        sed -n '/^== keys pressed/,$p' "$b/buttons.log" | sed '1d; s/^/  /'
    fi
    # stage durations from timeline.log ("<uptime> <message>"): delta to the previous entry
    if [ -f "$b/timeline.log" ]; then
        echo "--- timeline (uptime, +seconds since previous entry)"
        awk '$1 ~ /^[0-9.]+$/ { d = (NR > 1 ? $1 - p : 0); p = $1;
             m = $0; sub(/^[0-9.]+ +/, "", m); printf "  %8.2f  +%6.2f  %s\n", $1, d, m }' "$b/timeline.log"
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
