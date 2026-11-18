#!/usr/bin/env bash
# flash.sh <image> [device] [--remember] [--dry-run]   (HOST script, needs sudo for dd)
#
# Without a device: lists removable USB disks and exits.
# With a device: writes the image only if the target passes every safety check:
#   - is a whole disk (not a partition), TRAN=usb, RM=1, size <= 64 GiB
#   - none of its partitions is mounted as / or /home or listed in /etc/fstab
#   - its serial matches the one stored in .flash-device (repo root, git-ignored);
#     if that file does not exist yet, pass --remember to store this stick's serial.
# --dry-run runs every guard, prints the dd it would run, and exits (no sudo, no unmount).
# Afterwards: unmount, dd, sync, re-read partition table, sha256 read-back check.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
REMEMBER_FILE=${FLASH_REMEMBER_FILE:-$REPO/.flash-device}
MAX_BYTES=$(( 64 * 1024 * 1024 * 1024 ))

die() { echo "flash: REFUSED: $*" >&2; exit 1; }

# ---- args --------------------------------------------------------------------
REMEMBER=0; DRY=0; ARGS=()
for a in "$@"; do
    case $a in
        --remember) REMEMBER=1 ;;
        --dry-run)  DRY=1 ;;
        -h|--help)  sed -n '2,14p' "$0"; exit 0 ;;
        *)          ARGS+=("$a") ;;
    esac
done
USAGE="usage: $0 <image> [device] [--remember] [--dry-run]"
[ ${#ARGS[@]} -ge 1 ] && [ ${#ARGS[@]} -le 2 ] || { echo "$USAGE" >&2; exit 2; }
IMG=${ARGS[0]}
DEV=${ARGS[1]:-}
# Faked facts are for unit tests only: never let them reach a real write.
[ -z "${FLASH_FAKE:-}" ] || DRY=1

# ---- list mode ---------------------------------------------------------------
if [ -z "$DEV" ]; then
    echo "Removable USB disks:"
    lsblk -d -o NAME,SIZE,TRAN,RM,MODEL,SERIAL | awk 'NR==1 || ($3=="usb" && $4=="1")'
    echo; echo "Re-run:  $0 $IMG /dev/<name> [--remember] [--dry-run]"
    exit 0
fi

# ---- facts: the ONLY place that talks to lsblk (so guards are unit-testable) -------
# Sets F_TYPE F_TRAN F_RM F_SIZE F_MODEL F_SERIAL, F_MOUNTS (newline list of mount
# points of the disk and its partitions), F_IDS (newline list of /dev paths, UUID=,
# LABEL=, PARTUUID=, PARTLABEL= of the disk and its partitions).
# Test hook: FLASH_FAKE=<file> sources that file (plain F_*= assignments) instead.
field() { lsblk -dno "$1" "$2" 2>/dev/null | head -n1 | sed 's/^[[:space:]]*//; s/[[:space:]]*$//'; }
load_facts() {
    if [ -n "${FLASH_FAKE:-}" ]; then
        # shellcheck disable=SC1090
        . "$FLASH_FAKE"; return
    fi
    [ -b "$DEV" ] || die "$DEV is not a block device"
    DEV=$(readlink -f "$DEV")
    F_TYPE=$(field TYPE "$DEV"); F_TRAN=$(field TRAN "$DEV"); F_RM=$(field RM "$DEV")
    F_SIZE=$(lsblk -bdno SIZE "$DEV" | head -n1 | tr -d ' ')
    F_MODEL=$(field MODEL "$DEV"); F_SERIAL=$(field SERIAL "$DEV")
    F_MOUNTS=$(lsblk -nro MOUNTPOINTS "$DEV" | tr ' ' '\n' | sed '/^$/d')
    F_IDS=$(lsblk -nro PATH,UUID,LABEL,PARTUUID,PARTLABEL "$DEV" \
        | awk '{print $1; if ($2) print "UUID=" $2; if ($3) print "LABEL=" $3; if ($4) print "PARTUUID=" $4; if ($5) print "PARTLABEL=" $5}')
}

# ---- guards: pure functions of the F_* facts, IMG_SIZE and the fstab file ------------
guards() {
    [ "$F_TYPE" = disk ]  || die "$DEV is not a whole disk (type '$F_TYPE')"
    [ "$F_TRAN" = usb ]   || die "$DEV transport is '$F_TRAN', not usb"
    [ "$F_RM" = 1 ]       || die "$DEV is not flagged removable (RM='$F_RM')"
    [ "$F_SIZE" -le "$MAX_BYTES" ] || die "$DEV is $((F_SIZE / 1073741824)) GiB (> 64 GiB limit)"
    [ "$IMG_SIZE" -le "$F_SIZE" ]  || die "image ($IMG_SIZE B) larger than device ($F_SIZE B)"
    [ -n "$F_SERIAL" ]    || die "$DEV reports no serial number; cannot pin it in $REMEMBER_FILE"
    local m id spec
    while IFS= read -r m; do
        case $m in / | /home | /home/* | /boot | /boot/*) die "$DEV has a partition mounted at $m" ;; esac
    done <<< "$F_MOUNTS"
    local fstab=${FLASH_FSTAB:-/etc/fstab}
    if [ -r "$fstab" ]; then
        while read -r spec _; do
            case $spec in ''|\#*) continue ;; esac
            while IFS= read -r id; do
                [ -n "$id" ] && [ "$spec" = "$id" ] && die "$DEV ($id) is listed in $fstab"
            done <<< "$F_IDS"
        done < "$fstab"
    fi
    # serial pinning
    if [ -f "$REMEMBER_FILE" ]; then
        local known; known=$(sed -n 's/^serial=//p' "$REMEMBER_FILE" | head -n1)
        [ "$known" = "$F_SERIAL" ] || die "serial '$F_SERIAL' != remembered '$known' ($REMEMBER_FILE); delete that file to pair a new stick"
    elif [ "$REMEMBER" -ne 1 ]; then
        echo "flash: no $REMEMBER_FILE yet. If this is the right stick, re-run with --remember:" >&2
        echo "       $0 $IMG $DEV --remember" >&2
        die "stick not paired"
    fi
}

# ---- hard block: system disks (no flag can bypass this; it runs before everything) ---------
# Refuses /dev/sda and its partitions outright, and any disk backing /, /boot, /home or swap
# (ancestors included, so LVM/crypt on top of a system disk is caught).
disks_of() {   # whole disks underneath/behind a device node
    lsblk -nsrpo NAME,TYPE "$1" 2>/dev/null | awk '$2=="disk"{print $1}'
}
system_disks() {
    if [ -n "${FLASH_FAKE:-}" ]; then echo "${FLASH_FAKE_SYSDISKS:-}"; return; fi
    local src
    { for mp in / /boot /home; do findmnt -nro SOURCE --target "$mp" 2>/dev/null; done
      swapon --noheadings --raw --show=NAME 2>/dev/null | while read -r sw; do
          if [ -b "$sw" ]; then echo "$sw"; else findmnt -nro SOURCE --target "$sw" 2>/dev/null; fi   # swapfile -> its fs device
      done; } | sed 's/\[.*$//' | sort -u | while read -r src; do
        [ -b "$src" ] || continue
        echo "$src"; disks_of "$src"
    done
}
forbid_system_disks() {
    local t=$DEV td sd
    if [ -n "${FLASH_FAKE:-}" ]; then td=${FLASH_FAKE_TARGET_DISKS:-$t}
    else t=$(readlink -f "$DEV"); td="$t $(disks_of "$t")"; fi
    for d in $t $td; do
        case $d in /dev/sda | /dev/sda[0-9]* | /dev/sdap[0-9]*) die "$d is /dev/sda (or a partition of it): permanently blocked" ;; esac
        # nvme/mmc style names of the first disk are not special; only sda is hard-coded.
    done
    for sd in $(system_disks); do
        for d in $t $td; do
            [ "$d" = "$sd" ] && die "$t is, or sits on, $sd which backs /, /boot, /home or swap: permanently blocked"
        done
    done
    return 0    # the last test above is false for a safe target; do not trip set -e
}
forbid_system_disks

[ -f "$IMG" ] || die "image not found: $IMG"
IMG_SIZE=$(stat -c %s "$IMG")
load_facts
echo "Target: $DEV  type=$F_TYPE tran=$F_TRAN rm=$F_RM size=$((F_SIZE / 1048576)) MiB  model='$F_MODEL'  serial='$F_SERIAL'"
guards
echo "flash: all guards passed"

PAIRED=0; [ -f "$REMEMBER_FILE" ] && PAIRED=1
if [ "$DRY" -eq 1 ]; then
    [ "$PAIRED" -eq 1 ] || echo "flash: [dry-run] would store serial in $REMEMBER_FILE"
    echo "flash: [dry-run] would run: sudo dd if=$IMG of=$DEV bs=4M conv=fsync status=progress"
    echo "flash: [dry-run] nothing written, nothing unmounted."
    exit 0
fi
[ "$PAIRED" -eq 1 ] || { printf 'serial=%s\nmodel=%s\nsize_bytes=%s\n' "$F_SERIAL" "$F_MODEL" "$F_SIZE" > "$REMEMBER_FILE"
                         echo "flash: stored serial in $REMEMBER_FILE"; }

# ---- unmount, write, verify (sudo appears only below, after every guard) ----------------
echo "flash: unmounting partitions of $DEV"
while IFS= read -r m; do [ -n "$m" ] && sudo umount "$m"; done <<< "$F_MOUNTS"

echo "flash: writing $IMG ($IMG_SIZE bytes) -> $DEV"
sudo dd if="$IMG" of="$DEV" bs=4M conv=fsync status=progress
sync
sudo blockdev --flushbufs "$DEV" || true          # drop cached reads before verifying
sudo blockdev --rereadpt "$DEV" || sudo partprobe "$DEV" || echo "flash: warning: could not re-read partition table"
udevadm settle || true

echo "flash: verifying first $IMG_SIZE bytes (sha256)"
WANT=$(sha256sum "$IMG" | cut -d' ' -f1)
GOT=$(sudo dd if="$DEV" bs=4M iflag=count_bytes count="$IMG_SIZE" status=none | sha256sum | cut -d' ' -f1)
if [ "$WANT" != "$GOT" ]; then
    echo "flash: VERIFY FAILED  image=$WANT  device=$GOT" >&2
    exit 1
fi
echo "flash: OK  sha256=$WANT"
echo "flash: now eject the stick, plug it into the XO-1 and power on (no keyboard needed)."
