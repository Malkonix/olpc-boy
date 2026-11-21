#!/bin/sh
# mkexpected.sh -- reference data of the NAND installer, from the NAND backup.
#   mkexpected.sh BACKUP_DIR OUT_DIR
# BACKUP_DIR (opened read-only) holds nand-data-ecc.bin, the ECC-corrected dump of the whole NAND.
# OUT_DIR receives
#   expected-blocks.txt   "INDEX SHA256" for every erase block of the dump (nbhash -B/-L format)
#   sim/expected-blocks.txt  the same for the nandsim geometry (a fresh simulated NAND is all 0xFF), QEMU test
#   expected-blocks-restored.txt  (only when BACKUP_DIR has NANDCOPY.DAT) the same list for the NAND as it is after
#      restoring NANDCOPY.DAT with olpc-install: its blocks packed into the good blocks 0..PART_BLKS-1 in order
#      (known bad blocks skipped as nandwrite -p does; they keep their old content), the other good blocks erased
#      (all 0xFF data), the bad block table blocks unchanged.  The installer accepts a NAND equal to either list.
# The installer refuses to write unless the NAND still hashes to expected-blocks.txt block by block.
# Fails (exit 1) when the backup is missing or has the wrong size.
# Env: NBHASH NBTOOL (tools)  MX_ERASE MX_SIZE (geometry; the unit tests shrink it)  MX_SIM_BLKS
#      MX_PART_BLKS (blocks of the writable partition, default 8188)  MX_BAD (known bad blocks, default the XO-1's 13)
set -u
B=${1:-}; O=${2:-}
NBHASH=${NBHASH:-nbhash}; NBTOOL=${NBTOOL:-nbtool}
PART=${MX_PART_BLKS:-8188} BAD=${MX_BAD-495 967 4039 4115 4644 4752 4791 4926 6373 6974 7748 7824 7863}
ERASE=${MX_ERASE:-131072} SIZE=${MX_SIZE:-1073741824} SIM_BLKS=${MX_SIM_BLKS:-2048}
die() { echo "mkexpected: ERROR: $*" >&2; exit 1; }
[ -n "$B" ] && [ -n "$O" ] || die "usage: mkexpected.sh BACKUP_DIR OUT_DIR"
[ -d "$B" ] || die "NAND backup directory not found: $B (set NAND_BACKUP_DIR)"
DATA=$B/nand-data-ecc.bin
[ -r "$DATA" ] || die "missing $DATA"
[ "$(wc -c <"$DATA")" = "$SIZE" ] || die "$DATA is $(wc -c <"$DATA") bytes, expected $SIZE"
mkdir -p "$O/sim" || die "cannot create $O"
"$NBHASH" -n -B "$ERASE" -L "$O/expected-blocks.txt" <"$DATA" || die "hashing $DATA failed"
[ "$(wc -l <"$O/expected-blocks.txt")" = "$(( SIZE / ERASE ))" ] || die "expected-blocks.txt has the wrong number of lines"
FFH=$("$NBTOOL" fill ff "$ERASE" | "$NBHASH" -n -o "$O/.ff" && cut -d' ' -f1 "$O/.ff"); rm -f "$O/.ff"
[ -n "$FFH" ] || die "cannot hash an erased block"
i=0; : >"$O/sim/expected-blocks.txt"
while [ "$i" -lt "$SIM_BLKS" ]; do echo "$i $FFH" >>"$O/sim/expected-blocks.txt"; i=$(( i + 1 )); done
NC=$B/NANDCOPY.DAT
if [ -r "$NC" ]; then
	NCSZ=$(wc -c <"$NC")
	[ "$NCSZ" -gt 0 ] && [ $(( NCSZ % ERASE )) = 0 ] || die "$NC has a bad size ($NCSZ)"
	NCB=$(( NCSZ / ERASE ))
	"$NBTOOL" fill ff "$ERASE" >"$O/.ffblk" || die "cannot make an erased block"
	{
		b=0; k=0
		while [ "$b" -lt "$(( SIZE / ERASE ))" ]; do
			isbad=0; for x in $BAD; do [ "$x" = "$b" ] && isbad=1; done
			if [ "$b" -ge "$PART" ] || [ "$isbad" = 1 ]; then dd if="$DATA" bs="$ERASE" skip="$b" count=1 2>/dev/null     # untouched
			elif [ "$k" -lt "$NCB" ]; then dd if="$NC" bs="$ERASE" skip="$k" count=1 2>/dev/null; k=$(( k + 1 ))
			else cat "$O/.ffblk"; fi
			b=$(( b + 1 ))
		done
		[ "$k" = "$NCB" ] || exit 3
	} | "$NBHASH" -n -B "$ERASE" -L "$O/expected-blocks-restored.txt" || die "NANDCOPY.DAT does not fit in the partition (or hashing failed)"
	rm -f "$O/.ffblk"
	[ "$(wc -l <"$O/expected-blocks-restored.txt")" = "$(( SIZE / ERASE ))" ] || die "expected-blocks-restored.txt has the wrong number of lines"
	echo "mkexpected: restored-layout list made from $NCB blocks of NANDCOPY.DAT"
fi
echo "mkexpected: $(( SIZE / ERASE )) blocks hashed"
exit 0
