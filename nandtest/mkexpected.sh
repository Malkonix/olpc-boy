#!/bin/sh
# mkexpected.sh -- build the reference data of the NAND write test from the NAND backup.
#   mkexpected.sh BACKUP_DIR OUT_DIR
# BACKUP_DIR (opened read-only) holds nand-data-ecc.bin and nand-raw-oob.bin of the NAND backup image.
# OUT_DIR receives
#   expected-blocks.txt   "INDEX SHA256" for every erase block of the ECC data dump (nbhash -B/-L format)
#   expected-oob.bin      the raw OOB of every page of the test window (from the raw dump)
#   sim/                  the same for the nandsim geometry (fake, all erased), used by the QEMU test
# Fails (exit 1) when the backup is missing, has the wrong size, or the window is not erased.
# Env: NBHASH NBTOOL (tools), MX_ERASE MX_WRITE MX_OOB MX_SIZE MX_BLK0 MX_BLKS (geometry; tests shrink it)
#      MX_SIM_BLKS MX_SIM_BLK0 (nandsim geometry) MX_SIM_CM (cleanmarker OOB offset of nandsim)
set -u
B=${1:-}; O=${2:-}
NBHASH=${NBHASH:-nbhash}; NBTOOL=${NBTOOL:-nbtool}
ERASE=${MX_ERASE:-131072} WRITE=${MX_WRITE:-2048} OOB=${MX_OOB:-64} SIZE=${MX_SIZE:-1073741824}
BLK0=${MX_BLK0:-7300} BLKS=${MX_BLKS:-64}
SIM_BLKS=${MX_SIM_BLKS:-2048} SIM_BLK0=${MX_SIM_BLK0:-100} SIM_CM=${MX_SIM_CM:-2}
die() { echo "mkexpected: ERROR: $*" >&2; exit 1; }
[ -n "$B" ] && [ -n "$O" ] || die "usage: mkexpected.sh BACKUP_DIR OUT_DIR"
[ -d "$B" ] || die "NAND backup directory not found: $B (set NAND_BACKUP_DIR)"
DATA=$B/nand-data-ecc.bin; RAW=$B/nand-raw-oob.bin
[ -r "$DATA" ] || die "missing $DATA"
[ -r "$RAW" ] || die "missing $RAW"
PPB=$(( ERASE / WRITE )); TOT=$(( SIZE / ERASE )); WPAGES=$(( BLKS * PPB ))
[ "$(wc -c <"$DATA")" = "$SIZE" ] || die "$DATA is $(wc -c <"$DATA") bytes, expected $SIZE"
[ "$(wc -c <"$RAW")" = "$(( SIZE / WRITE * (WRITE + OOB) ))" ] || die "$RAW has the wrong size"
mkdir -p "$O/sim" || die "cannot create $O"
"$NBHASH" -n -B "$ERASE" -L "$O/expected-blocks.txt" <"$DATA" || die "hashing $DATA failed"
[ "$(wc -l <"$O/expected-blocks.txt")" = "$TOT" ] || die "expected-blocks.txt has the wrong number of lines"
FFH=$("$NBTOOL" fill ff "$ERASE" | "$NBHASH" -n -o "$O/.ff" && cut -d' ' -f1 "$O/.ff"); rm -f "$O/.ff"
[ -n "$FFH" ] || die "cannot hash an erased block"
n=0; i=$BLK0
while [ "$i" -lt $(( BLK0 + BLKS )) ]; do
	h=$(sed -n "$(( i + 1 ))p" "$O/expected-blocks.txt")
	[ "$h" = "$i $FFH" ] || die "backup block $i (test window) is not erased: $h"
	i=$(( i + 1 ))
done
"$NBTOOL" oobx "$RAW" "$(( BLK0 * PPB ))" "$WPAGES" "$WRITE" "$OOB" >"$O/expected-oob.bin" || die "cannot extract the window OOB"
[ "$(wc -c <"$O/expected-oob.bin")" = "$(( WPAGES * OOB ))" ] || die "expected-oob.bin has the wrong size"
# the window pages of the raw dump must be erased too (data all 0xFF)
dd if="$RAW" bs="$(( WRITE + OOB ))" skip="$(( BLK0 * PPB ))" count="$WPAGES" 2>/dev/null \
	| "$NBTOOL" oob -w "$WRITE" -o "$OOB" -x "$O/expected-oob.bin" >"$O/.chk" || die "raw window check failed"
read -r pg dn db _ < "$O/.chk"; rm -f "$O/.chk"
[ "$pg" = "$WPAGES" ] && [ "$dn" = 0 ] && [ "$db" = 0 ] || die "raw dump of the window is not erased (pages $pg, non-FF $dn)"
# nandsim reference: fresh device = all erased; the cleanmarker lands at SIM_CM of page 0 of each block after erase -j
i=0; : >"$O/sim/expected-blocks.txt"
while [ "$i" -lt "$SIM_BLKS" ]; do echo "$i $FFH" >>"$O/sim/expected-blocks.txt"; i=$(( i + 1 )); done
{ "$NBTOOL" fill ff "$SIM_CM"; printf '\205\031\003 \010\000\000\000'; "$NBTOOL" fill ff "$(( OOB - SIM_CM - 8 ))"; } >"$O/.p0"
"$NBTOOL" fill ff "$(( (PPB - 1) * OOB ))" >"$O/.pn"
b=0
while [ "$b" -lt "$BLKS" ]; do cat "$O/.p0" "$O/.pn"; b=$(( b + 1 )); done >"$O/sim/expected-oob.bin"
rm -f "$O/.p0" "$O/.pn"
echo "mkexpected: $TOT blocks hashed, window $BLK0..$(( BLK0 + BLKS - 1 )) erased, $WPAGES window OOBs extracted"
exit 0
