#!/bin/sh
# Logic tests of olpc-nandbackup against a fake MTD (stub nanddump + fake sysfs), no hardware.
# Runs the script with busybox ash when available (the target shell).  Needs gcc, zstd+gzip, sha256sum.
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
bad() { echo "  FAIL: $*"; fail=1; }
ok()  { echo "  ok: $*"; }
ASH="sh"; command -v busybox >/dev/null && busybox ash -c : 2>/dev/null && ASH="busybox ash"
SCRIPT=$here/overlay/usr/local/sbin/olpc-nandbackup
echo "== backup/test.sh (shell: $ASH)"

# ---- nbhash vs sha256sum ----------------------------------------------------------------
${CC:-gcc} -O2 -Wall -Wextra -o "$tmp/nbhash" "$here/src/nbhash.c" || exit 1
for n in 0 1 55 56 63 64 65 119 120 121 4096 65536 65537 300001; do
	head -c $n /dev/urandom >"$tmp/h.bin"
	"$tmp/nbhash" -c "$tmp/h.cnt" -o "$tmp/h.res" <"$tmp/h.bin" >"$tmp/h.out"
	read -r s b <"$tmp/h.res"
	[ "$s" = "$(sha256sum <"$tmp/h.bin" | cut -d' ' -f1)" ] && [ "$b" = $n ] && cmp -s "$tmp/h.bin" "$tmp/h.out" && [ "$(cat "$tmp/h.cnt")" = $n ] \
		|| bad "nbhash $n bytes"
done
ok "nbhash: sha256, byte count and pass-through match sha256sum (14 sizes)"
head -c 20000 /dev/urandom >"$tmp/b.bin"
"$tmp/nbhash" -n -B 4224 -L "$tmp/b.list" <"$tmp/b.bin"; ok_b=1; i=0
while [ $i -lt 5 ]; do
	h=$(dd if="$tmp/b.bin" bs=4224 skip=$i count=1 2>/dev/null | sha256sum | cut -d' ' -f1)
	[ "$(sed -n "$((i + 1))p" "$tmp/b.list")" = "$i $h" ] || ok_b=0; i=$((i + 1))
done
[ $ok_b = 1 ] && [ "$(wc -l <"$tmp/b.list")" = 5 ] && ok "nbhash -B/-L: per-block SHA-256 list matches (4 full blocks + the partial last)" || bad "nbhash block list"
echo 0 | "$tmp/nbhash" -x 2>/dev/null; [ $? = 2 ] && ok "nbhash rejects unknown options"
{ head -c 100000 /dev/zero | "$tmp/nbhash" >/dev/full; } 2>/dev/null; [ $? != 0 ] && ok "nbhash fails when the output is full"

# ---- synthetic NAND images -------------------------------------------------------------
# mkfake SPEC RAW DATA EXPECT [ERASE WRITE OOB]: one char per erase block
#   R random data   F all 0xFF   B random data + BBT signature in the OOB (Bbt0, then 1tbB)
# EXPECT = the R blocks below the first B (what nbcompact must emit)
cat >"$tmp/mkfake.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int c, char **v)
{
	const char *s = v[1];
	long erase = c > 5 ? atol(v[5]) : 131072, wr = c > 6 ? atol(v[6]) : 2048, oob = c > 7 ? atol(v[7]) : 64;
	long ppb = erase / wr, n = strlen(s), first = n, sigs = 0;
	FILE *r = fopen(v[2], "wb"), *d = fopen(v[3], "wb"), *e = fopen(v[4], "wb");
	unsigned char *blk = malloc(erase), *o = malloc(oob);
	for (long i = 0; i < n; i++) if (s[i] == 'B' && i < first) first = i;
	srand(12345);
	for (long i = 0; i < n; i++) {
		if (s[i] == 'F') memset(blk, 0xff, erase);
		else { for (long k = 0; k < erase; k++) blk[k] = rand(); blk[0] = 0x85; blk[1] = 0x19; }
		fwrite(blk, 1, erase, d);
		if (s[i] == 'R' && i < first) fwrite(blk, 1, erase, e);
		for (long p = 0; p < ppb; p++) {
			memset(o, 0xff, oob);
			if (s[i] != 'F') for (long k = 0; k < oob; k++) o[k] = rand();
			if (s[i] == 'B' && p == 0) memcpy(o + 14, sigs++ ? "1tbB" : "Bbt0", 4);
			fwrite(blk + p * wr, 1, wr, r); fwrite(o, 1, oob, r);
		}
	}
	fclose(r); fclose(d); fclose(e);
	return 0;
}
C
${CC:-gcc} -O2 -o "$tmp/mkfake" "$tmp/mkfake.c" || exit 1
${CC:-gcc} -O2 -Wall -Wextra -o "$tmp/nbcompact" "$here/src/nbcompact.c" || exit 1
${CC:-gcc} -O2 -Wall -Wextra -o "$tmp/nbcmp" "$here/src/nbcmp.c" || exit 1
cat >"$tmp/flip.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* flip FILE OFF:MASK ... | every:STEP:MASK : copy FILE to stdout with bytes XORed */
int main(int c, char **v)
{
	FILE *f = fopen(v[1], "rb"); long n; unsigned char *b;
	fseek(f, 0, SEEK_END); n = ftell(f); rewind(f); b = malloc(n);
	if (fread(b, 1, n, f) != (size_t)n) return 1;
	for (int i = 2; i < c; i++) {
		long a, m;
		if (!strncmp(v[i], "every:", 6)) { long st = atol(v[i] + 6); m = strtol(strrchr(v[i], ':') + 1, NULL, 0); for (a = 0; a < n; a += st) b[a] ^= m; }
		else { a = atol(v[i]); m = strtol(strchr(v[i], ':') + 1, NULL, 0); b[a] ^= m; }
	}
	fwrite(b, 1, n, stdout);
	return 0;
}
C
${CC:-gcc} -O2 -o "$tmp/flip" "$tmp/flip.c" || exit 1
E=4096 W=2048 O=64
rep() { sed -n "s/^$2=//p" "$1"; }

# ---- nbcompact --------------------------------------------------------------------------
S1=$(printf 'RRFRFFRRFR%s%sFFFFFRFFBB' FFFFFFFFFF FFFFFFFFFF)       # 40 blocks, R at 35, BBT at 38,39
"$tmp/mkfake" "$S1" "$tmp/c.raw" "$tmp/c.dat" "$tmp/c.exp" $E $W $O
"$tmp/nbcompact" -r "$tmp/c.raw" -d "$tmp/c.dat" -R "$tmp/c.rep" -e $E -w $W -o $O >"$tmp/c.out"; rc=$?
[ $rc = 0 ] && cmp -s "$tmp/c.out" "$tmp/c.exp" && [ "$(rep "$tmp/c.rep" kept)" = 7 ] && [ "$(rep "$tmp/c.rep" limit)" = 38 ] \
	&& [ "$(rep "$tmp/c.rep" bbt_blocks)" = 38,39 ] && [ "$(rep "$tmp/c.rep" erased)" = 31 ] && [ "$(rep "$tmp/c.rep" beyond_limit_nonff)" = 0 ] \
	&& ok "nbcompact: drops erased blocks and BBT blocks, keeps the rest in order (7 of 40)" || { bad "nbcompact basic (rc=$rc)"; cat "$tmp/c.rep"; }
[ $(( $(stat -c %s "$tmp/c.out") % E )) = 0 ] && ok "nbcompact output is a multiple of the erase block size" || bad "nbcompact size multiple"
S2=$(printf 'RFFFFFFFFF%s%sFFFFRFFFFR' FFFFFFFFFF FFFFFFFFFF)          # 40 blocks, no BBT signature, R at 34 and 39
"$tmp/mkfake" "$S2" "$tmp/d.raw" "$tmp/d.dat" "$tmp/d.exp" $E $W $O
"$tmp/nbcompact" -r "$tmp/d.raw" -d "$tmp/d.dat" -R "$tmp/d.rep" -e $E -w $W -o $O >"$tmp/d.out"
[ "$(rep "$tmp/d.rep" limit)" = 38 ] && [ "$(rep "$tmp/d.rep" kept)" = 2 ] && [ "$(rep "$tmp/d.rep" beyond_limit_nonff)" = 1 ] && [ "$(rep "$tmp/d.rep" bbt_blocks)" = none ] \
	&& ok "nbcompact: no BBT found -> limit = last 2 blocks, data beyond it counted, not emitted" || { bad "nbcompact no-BBT"; cat "$tmp/d.rep"; }
S3=$(printf 'RRRRRRRRRR%s%sFFRFFRFBBB' FFFFFFFFFF FFFFFFFFFF)
"$tmp/mkfake" "$S3" "$tmp/e.raw" "$tmp/e.dat" "$tmp/e.exp" $E $W $O
"$tmp/nbcompact" -r "$tmp/e.raw" -d "$tmp/e.dat" -R "$tmp/e.rep" -e $E -w $W -o $O >"$tmp/e.out"
[ "$(rep "$tmp/e.rep" limit)" = 37 ] && [ "$(rep "$tmp/e.rep" bbt_count)" = 3 ] && cmp -s "$tmp/e.out" "$tmp/e.exp" \
	&& ok "nbcompact: limit is the lowest BBT block" || { bad "nbcompact 3 BBT"; cat "$tmp/e.rep"; }
head -c 100000 "$tmp/c.raw" >"$tmp/short.raw"
"$tmp/nbcompact" -r "$tmp/short.raw" -d "$tmp/c.dat" -R "$tmp/s.rep" -e $E -w $W -o $O >"$tmp/s.out" 2>/dev/null; [ $? != 0 ] \
	&& ok "nbcompact rejects mismatching raw/data sizes" || bad "nbcompact size check"

# ---- nbcmp -------------------------------------------------------------------------------
head -c 300000 /dev/urandom >"$tmp/n.a"
"$tmp/nbcmp" -b 4096 -o "$tmp/n.res" "$tmp/n.a" <"$tmp/n.a"; [ "$(cat "$tmp/n.res")" = "0 0 0 300000 300000" ] && ok "nbcmp: identical streams -> 0 bytes" || bad "nbcmp identical"
"$tmp/flip" "$tmp/n.a" 10:1 5000:3 5001:0xff 299999:0x80 | "$tmp/nbcmp" -b 4096 -o "$tmp/n.res" "$tmp/n.a"
[ "$(cat "$tmp/n.res")" = "4 12 3 300000 300000" ] && ok "nbcmp: counts differing bytes (4), bits (12) and 4 KiB blocks (3)" || bad "nbcmp counts: $(cat "$tmp/n.res")"
head -c 299000 "$tmp/n.a" | "$tmp/nbcmp" -b 4096 -o "$tmp/n.res" "$tmp/n.a"; [ "$(cut -d' ' -f1,4,5 "$tmp/n.res")" = "1000 299000 300000" ] && ok "nbcmp: a short stream counts the missing bytes" || bad "nbcmp short: $(cat "$tmp/n.res")"
{ cat "$tmp/n.a"; printf 'xyz'; } | "$tmp/nbcmp" -b 4096 -o "$tmp/n.res" "$tmp/n.a"; [ "$(cut -d' ' -f1,4,5 "$tmp/n.res")" = "3 300003 300000" ] && ok "nbcmp: a long stream counts the extra bytes" || bad "nbcmp long: $(cat "$tmp/n.res")"

# ---- fake machine -----------------------------------------------------------------------
SIZE=$(( 40 * E )); ERASE=$E WR=$W OB=$O
PAGES=$(( SIZE / WR )); RAWSZ=$(( PAGES * (WR + OB) ))
cp "$tmp/c.raw" "$tmp/nand.raw"; cp "$tmp/c.dat" "$tmp/nand.data"; cp "$tmp/c.exp" "$tmp/nand.expect"
RAWSHA=$(sha256sum <"$tmp/nand.raw" | cut -d' ' -f1); DATSHA=$(sha256sum <"$tmp/nand.data" | cut -d' ' -f1)

cat >"$tmp/common.sh" <<'C'
BOOTDIR=$NB_BOOTDIR; mkdir -p "$BOOTDIR"
tl() { echo "$*" >>"$BOOTDIR/timeline.log"; }
olpc_sleep() { :; }
C
cat >"$tmp/nanddump" <<'S'
#!/bin/sh
# stub: raw (--noecc --oob) = the fake raw image; otherwise its data part.  Never touches anything.
raw=0; for a in "$@"; do case $a in --noecc) raw=1 ;; --version) echo "nanddump (stub)"; exit 0 ;; esac; done
n=0; [ -f "$STUB_STATE" ] && n=$(cat "$STUB_STATE"); n=$((n + 1)); echo $n >"$STUB_STATE"
[ "$STUB_FAIL" = 1 ] && { head -c 1000 "$STUB_RAW"; echo "stub: mtd_read failed" >&2; exit 1; }
if [ $raw = 1 ]; then
	case " ${STUB_RAWNOISE_ON:-3} " in *" $n "*) if [ -n "$STUB_RAWNOISE" ]; then "$STUB_FLIP" "$STUB_RAW" $STUB_RAWNOISE; else cat "$STUB_RAW"; fi ;; *) cat "$STUB_RAW" ;; esac
else
	[ "$STUB_ECC" = 1 ] && { echo "ECC: 2 corrected bitflip(s) at offset 0x00000800" >&2
		echo "ECC: 1 uncorrectable bitflip(s) at offset 0x00020000" >&2
		echo "ECC: 3 corrected bitflip(s) at offset 0x00020800" >&2; }
	case " $STUB_DNOISE " in *" $n "*) "$STUB_FLIP" "$STUB_DATA" 100:1 ;; *) cat "$STUB_DATA" ;; esac
fi
S
# blkid stub: the labelled volume shows up from call number $STUB_APPEAR on (decoys always there)
cat >"$tmp/blkids" <<'S'
#!/bin/sh
n=0; [ -f "$STUB_BLK" ] && n=$(cat "$STUB_BLK"); n=$((n + 1)); echo $n >"$STUB_BLK"
echo '/dev/sda1: LABEL="OLPCLOG" UUID="AAAA-AAAA" TYPE="vfat"'
echo '/dev/sda2: LABEL="OLPCROOT" UUID="1111" TYPE="ext4"'
echo '/dev/sdc1: LABEL="OLPCNAND" UUID="2222" TYPE="ext4"'
echo '/dev/sdd1: LABEL="OTHER" UUID="BBBB-BBBB" TYPE="vfat"'
[ "$n" -ge "${STUB_APPEAR:-1}" ] && echo '/dev/sdb1: LABEL="OLPCNAND" UUID="CCCC-CCCC" TYPE="vfat"'
exit 0
S
cat >"$tmp/mountstub" <<'S'
#!/bin/sh
echo "$*" >>"$STUB_MOUNTLOG"; [ "$STUB_MOUNTFAIL" = 1 ] && { echo "mount: wrong fs type" >&2; exit 32; }; exit 0
S
cat >"$tmp/umountstub" <<'S'
#!/bin/sh
echo "umount $*" >>"$STUB_MOUNTLOG"; exit 0
S
chmod +x "$tmp/nanddump" "$tmp/blkids" "$tmp/mountstub" "$tmp/umountstub"
cat >"$tmp/splash" <<S
#!/bin/sh
echo "\$*" >>"$tmp/splash.calls"
S
chmod +x "$tmp/splash"
echo 'console=tty0 mtdparts=cafe_nand:-(nand)ro olpcboy.nandbackup' >"$tmp/cmdline"

# fresh scenario: mkscn NAME -> sets D (work dir); fake sysfs, an empty "backup stick" directory
mkscn() {
	D=$tmp/$1; rm -rf "$D"; mkdir -p "$D/sys" "$D/dest" "$D/run"
	echo 0x0 >"$D/sys/flags"; echo nand >"$D/sys/name"; echo nand >"$D/sys/type"
	echo $SIZE >"$D/sys/size"; echo $ERASE >"$D/sys/erasesize"; echo $WR >"$D/sys/writesize"; echo $OB >"$D/sys/oobsize"
	echo 0 >"$D/sys/bad_blocks"; echo 0 >"$D/sys/bbt_blocks"; echo 0 >"$D/sys/corrected_bits"; echo 0 >"$D/sys/ecc_failures"
	: >"$D/mtd0"
	rm -f "$tmp/state" "$tmp/blk.n" "$tmp/mount.log" "$tmp/splash.calls"
}
# runit [VAR=val ...]: runs the script with the destination directory standing in for the mounted stick
runit() {
	env NB_TEST=1 NB_NOLOAD=1 NB_NOPOWEROFF=1 NB_NOIDLE=1 OLPC_COMMON="$tmp/common.sh" NB_BOOTDIR="$D/boot" \
	  NB_MTD="$D/mtd0" NB_SYS="$D/sys" NB_NANDDUMP="$tmp/nanddump" NB_NBHASH="$tmp/nbhash" NB_NBCOMPACT="$tmp/nbcompact" NB_SPLASH="$tmp/splash" \
	  NB_DEST_DIR="$D/dest" NB_RUN="$D/run" NB_TTY="$D/tty" NB_CMDLINE="$tmp/cmdline" \
	  NB_SIZE=$SIZE NB_ERASE=$ERASE NB_WRITE=$WR NB_OOB=$OB NB_FREE=3000000000 \
	  STUB_RAW="$tmp/nand.raw" STUB_DATA="$tmp/nand.data" STUB_STATE="$tmp/state" STUB_FLIP="$tmp/flip" NB_NBCMP="$tmp/nbcmp" "$@" $ASH "$SCRIPT" >"$D/out" 2>&1
}
# runwait [VAR=val ...]: like runit but through the label search + mount path (stubbed blkid/mount)
runwait() {
	mkdir -p "$D/mnt"
	env NB_TEST=1 NB_NOLOAD=1 NB_NOPOWEROFF=1 NB_NOIDLE=1 OLPC_COMMON="$tmp/common.sh" NB_BOOTDIR="$D/boot" \
	  NB_MTD="$D/mtd0" NB_SYS="$D/sys" NB_NANDDUMP="$tmp/nanddump" NB_NBHASH="$tmp/nbhash" NB_NBCOMPACT="$tmp/nbcompact" NB_SPLASH="$tmp/splash" \
	  NB_DEST_MNT="$D/mnt" NB_BLKIDS="$tmp/blkids" NB_MOUNT="$tmp/mountstub" NB_UMOUNT="$tmp/umountstub" STUB_MOUNTLOG="$tmp/mount.log" STUB_BLK="$tmp/blk.n" \
	  NB_RUN="$D/run" NB_TTY="$D/tty" NB_CMDLINE="$tmp/cmdline" \
	  NB_SIZE=$SIZE NB_ERASE=$ERASE NB_WRITE=$WR NB_OOB=$OB NB_FREE=3000000000 \
	  STUB_RAW="$tmp/nand.raw" STUB_DATA="$tmp/nand.data" STUB_STATE="$tmp/state" STUB_FLIP="$tmp/flip" NB_NBCMP="$tmp/nbcmp" "$@" $ASH "$SCRIPT" >"$D/out" 2>&1
}
noset() { [ -z "$(ls "$D"/dest/nandbackup-*/*.bin "$D"/dest/nandbackup-*/*.DAT 2>/dev/null)" ]; }
NEEDR=$(( RAWSZ + RAWSZ / 100 + 16777216 )); NEEDD=$(( SIZE + SIZE / 100 + 16777216 ))

# ---- happy path, twice (no overwrite) ---------------------------------------------------
mkscn happy
runit STUB_ECC=1; rc=$?
[ $rc = 0 ] && ok "run 1 exits 0" || { bad "run 1 rc=$rc"; sed 's/^/     /' "$D/out" | tail -n 20; }
! grep -q "can.t open" "$D/out" && ok "no shell errors about missing counter files" || bad "counter race errors: $(grep "can.t open" "$D/out" | head -n 2)"
grep -q "^init " "$tmp/splash.calls" && ok "graphic splash restored after the wait screen" || bad "splash init"
S=$D/dest/nandbackup-001
cmp -s "$S/nand-raw-oob.bin" "$tmp/nand.raw" && ok "raw dump (data+oob) identical to the NAND image" || bad "raw dump content"
cmp -s "$S/nand-data-ecc.bin" "$tmp/nand.data" && ok "data dump identical" || bad "data dump content"
cmp -s "$S/NANDCOPY.DAT" "$tmp/nand.expect" && ok "NANDCOPY.DAT = compacted blocks (BBT/erased dropped, order kept)" || bad "NANDCOPY.DAT content"
(cd "$S" && sha256sum -c SHA256SUMS >/dev/null 2>&1) && ok "SHA256SUMS verifies (raw, data, NANDCOPY.DAT, MANIFEST)" || bad "SHA256SUMS"
grep -q "$RAWSHA  nand-raw-oob.bin" "$S/STREAMS.sha256" && grep -q "$DATSHA  nand-data-ecc.bin" "$S/STREAMS.sha256" && ok "STREAMS.sha256 has the stream hashes" || bad "STREAMS.sha256"
grep -q "sha256=$RAWSHA" "$S/MANIFEST.txt" && grep -q "raw read 3 vs stored: 0 bytes / 0 bits in 0 blocks" "$S/MANIFEST.txt" && grep -q "warm-up read 1 vs stored: identical" "$S/MANIFEST.txt" && grep -q "stable (pass 2 identical" "$S/MANIFEST.txt" \
	&& ok "manifest: raw hash, raw noise 0, data stability" || bad "manifest"
grep -q 'nd-copy-nand u:\\NANDCOPY.DAT' "$S/MANIFEST.txt" && ok "manifest documents the OFW restore command" || bad "manifest restore text"
grep -q "pass 1 = 1 1 2 5, pass 2 = 1 1 2 5" "$S/MANIFEST.txt" && ok "manifest: ECC counts parsed" || { bad "ECC counts"; grep ECC "$S/MANIFEST.txt"; }
grep -q "BACKUP OK" "$D/tty" && ok "final screen says BACKUP OK" || bad "final screen"
[ -f "$D/boot/nandbackup.log" ] && [ -f "$D/boot/nandbackup/MANIFEST.txt" ] && ok "log + manifest copy under the boot dir" || bad "boot dir logs"
grep -q "progress 100 " "$tmp/splash.calls" && ok "splash progress reached 100" || bad "splash progress"
[ "$(cat "$tmp/state")" = 5 ] && ok "nanddump called 5 times (raw warm-up, raw stored, raw compare, data, data compare)" || bad "nanddump calls: $(cat "$tmp/state")"
rm -f "$tmp/state"
runit; rc=$?
[ $rc = 0 ] && [ -f "$D/dest/nandbackup-002/NANDCOPY.DAT" ] && cmp -s "$S/nand-raw-oob.bin" "$tmp/nand.raw" && ok "second run: new nandbackup-002, nandbackup-001 untouched" || bad "no-overwrite (rc=$rc)"

# ---- refusals: nothing may be read ------------------------------------------------------
mkscn rw; echo 0x400 >"$D/sys/flags"
runit; rc=$?
[ $rc = 1 ] && grep -q "WRITEABLE" "$D/tty" && noset && [ ! -e "$tmp/state" ] && ok "writeable MTD: refused, no nanddump run" || bad "writeable MTD not refused"
for kv in size:524288 oobsize:128 writesize:4096 erasesize:65536 name:cafe_nand type:nor; do
	mkscn geo; echo "${kv#*:}" >"$D/sys/${kv%%:*}"
	runit; rc=$?
	[ $rc = 1 ] && noset && [ ! -e "$tmp/state" ] && ok "${kv%%:*}=${kv#*:} refused" || bad "${kv} not refused"
done
mkscn nocmd; echo 'console=tty0' >"$tmp/cmdline.bad"
runit NB_CMDLINE="$tmp/cmdline.bad"; rc=$?
[ $rc = 1 ] && grep -q "mtdparts" "$D/tty" && [ ! -e "$tmp/state" ] && ok "no mtdparts=...ro on the cmdline: refused before loading" || bad "cmdline guard"
mkscn bbt; mkdir -p "$D/modsys/parameters" "$tmp/fakebin"; echo 1 >"$D/modsys/parameters/allow_bbt_write"
printf '#!/bin/sh\nexit 0\n' >"$tmp/fakebin/modprobe"; chmod +x "$tmp/fakebin/modprobe"
runit NB_NOLOAD= NB_MODSYS="$D/modsys" PATH="$tmp/fakebin:$PATH"; rc=$?
[ $rc = 1 ] && grep -q "allow_bbt_write" "$D/tty" && [ ! -e "$tmp/state" ] && ok "allow_bbt_write=1 refused" || bad "allow_bbt_write guard"

# ---- raw reads: warm-up, stored, confirming; differences are measured, never a failure -----
mkscn warmup
runit STUB_RAWNOISE="10:1 5000:3" STUB_RAWNOISE_ON=1; rc=$?
M=$D/dest/nandbackup-001/MANIFEST.txt
[ $rc = 0 ] && grep -q "warm-up read 1 vs stored: 2 erase blocks differ (first: 0 1)" "$M" && grep -q "raw read 3 vs stored: 0 bytes" "$M" && ! grep -q "WARNING: the stored raw" "$M" \
	&& cmp -s "$D/dest/nandbackup-001/nand-raw-oob.bin" "$tmp/nand.raw" \
	&& ok "odd warm-up pass: stored file is the clean pass 2, 2 differing blocks listed, no warning" || { bad "warm-up noise (rc=$rc)"; grep "raw" "$M"; }
mkscn rawnoise
runit STUB_RAWNOISE="10:1 5000:3"; rc=$?
M=$D/dest/nandbackup-001/MANIFEST.txt
[ $rc = 0 ] && grep -q "raw read 3 vs stored: 2 bytes / 3 bits in 2 blocks" "$M" && grep -q "WARNING: the stored raw read is NOT confirmed by read 3" "$M" \
	&& cmp -s "$D/dest/nandbackup-001/nand-raw-oob.bin" "$tmp/nand.raw" && [ "$(cat "$tmp/state")" = 5 ] \
	&& ok "noisy confirming pass: OK with counts and a not-confirmed warning, stored file kept, no 4th raw read" || { bad "raw noise (rc=$rc calls=$(cat "$tmp/state"))"; grep raw "$M"; }
mkscn rawhuge
runit STUB_RAWNOISE="every:20:0xff"; rc=$?
[ $rc = 0 ] && grep -q "WARNING: raw read noise is above 1%" "$D/dest/nandbackup-001/MANIFEST.txt" && grep -q "WARNING" "$D/tty" \
	&& ok "raw noise above 1% of the bytes: still OK, prominent WARNING" || bad "huge raw noise (rc=$rc)"

# ---- ECC-corrected reads are the integrity anchor ----------------------------------------
mkscn eccflip2
runit STUB_DNOISE=5; rc=$?
[ $rc = 0 ] && [ "$(cat "$tmp/state")" = 6 ] && grep -q "UNSTABLE: pass 2 did not match.*pass 3 identical to the stored file" "$D/dest/nandbackup-001/MANIFEST.txt" \
	&& grep -q "ECC reads UNSTABLE" "$D/tty" && cmp -s "$D/dest/nandbackup-001/nand-data-ecc.bin" "$tmp/nand.data" \
	&& ok "ECC pass 2 differs -> pass 3 matches the stored file: OK with a note" || bad "ecc flip2 (rc=$rc calls=$(cat "$tmp/state"))"
mkscn eccflip23
runit STUB_DNOISE="5 6"; rc=$?
[ $rc = 1 ] && grep -q "ECC-corrected reads unstable" "$D/tty" && cmp -s "$D/dest/nandbackup-001/nand-data-ecc.bin" "$tmp/nand.data" && [ -f "$D/dest/nandbackup-001/nand-raw-oob.bin" ] \
	&& ok "ECC passes 2 and 3 both differ -> BACKUP FAILED, files kept" || bad "ecc flip23 (rc=$rc)"

# ---- space ------------------------------------------------------------------------------
mkscn nospace
runit NB_FREE=$(( NEEDR - 1 )); rc=$?
[ $rc = 1 ] && grep -q "not enough free space" "$D/tty" && noset && [ ! -e "$tmp/state" ] && ok "backup stick too small: refused before any read" || bad "space refusal"
mkscn rawonly
runit NB_FREE_AFTER_RAW=$(( NEEDD - 1 )); rc=$?
[ $rc = 0 ] && [ -f "$D/dest/nandbackup-001/nand-raw-oob.bin" ] && [ ! -e "$D/dest/nandbackup-001/nand-data-ecc.bin" ] && [ ! -e "$D/dest/nandbackup-001/NANDCOPY.DAT" ] \
	&& grep -q "SKIPPED" "$D/dest/nandbackup-001/MANIFEST.txt" && ok "no room after the raw dump: data dump skipped and said so" || bad "raw-only (rc=$rc)"
mkscn nocpy
runit NB_FREE_AFTER_DATA=$(( NEEDD - 1 )); rc=$?
[ $rc = 0 ] && [ -f "$D/dest/nandbackup-001/nand-data-ecc.bin" ] && [ ! -e "$D/dest/nandbackup-001/NANDCOPY.DAT" ] && grep -q "restore.*SKIPPED" "$D/dest/nandbackup-001/MANIFEST.txt" \
	&& grep -q "Restore file NOT built" "$D/tty" && ok "no room for the restore file: skipped and said so" || bad "restore skip (rc=$rc)"

# ---- bad block warnings in the manifest ---------------------------------------------------
mkscn manybad; echo 300 >"$D/sys/bad_blocks"
runit; rc=$?
grep -q "nd-copy-nand would run out" "$D/dest/nandbackup-001/MANIFEST.txt" && ok "many flash-BBT bad blocks: restore warning in the manifest" || bad "bad-block warning (rc=$rc)"

# ---- nanddump failure -------------------------------------------------------------------
mkscn ndfail
runit STUB_FAIL=1; rc=$?
[ $rc = 1 ] && noset && grep -q "BACKUP FAILED" "$D/tty" && ok "nanddump failure: FAILED, no partial dump left" || bad "nanddump failure (rc=$rc)"

# ---- waiting for the OLPCNAND stick -----------------------------------------------------
mkscn wait
runwait STUB_APPEAR=4; rc=$?
[ $rc = 0 ] && [ "$(cat "$tmp/blk.n")" = 4 ] && grep -qx -- "-t vfat -o rw,noatime,flush /dev/sdb1 $D/mnt" "$tmp/mount.log" \
	&& ok "waits for the label (decoys ignored), mounts /dev/sdb1 as vfat rw" || { bad "label wait (rc=$rc polls=$(cat "$tmp/blk.n" 2>/dev/null))"; cat "$tmp/mount.log" 2>/dev/null; }
[ "$(grep -c . "$tmp/mount.log")" = 2 ] && grep -q "^umount $D/mnt" "$tmp/mount.log" && ok "exactly one mount and one umount, of the backup stick" || bad "mount log: $(cat "$tmp/mount.log")"
[ -f "$D/mnt/nandbackup-001/NANDCOPY.DAT" ] && ok "dumps written under the mounted stick" || bad "dumps not under the mount"
grep -q "INSERT THE BACKUP STICK" "$D/boot/nandbackup.log" && ok "the insert-stick message was shown while waiting" || bad "insert message"
mkscn nolabel
runwait STUB_APPEAR=99 NB_WAIT_ITER=5; rc=$?
[ $rc = 1 ] && grep -q "no FAT volume labelled OLPCNAND" "$D/tty" && [ ! -e "$tmp/state" ] && [ ! -e "$tmp/mount.log" ] && [ "$(cat "$tmp/blk.n")" -ge 5 ] \
	&& ok "label never appears: FAILED cleanly, nothing mounted, NAND not read" || bad "label timeout (rc=$rc)"
mkscn mountfail
runwait STUB_MOUNTFAIL=1; rc=$?
[ $rc = 1 ] && grep -q "cannot mount" "$D/tty" && [ ! -e "$tmp/state" ] && ok "mount failure: FAILED, NAND not read" || bad "mount failure (rc=$rc)"

# ---- write-tool policy ------------------------------------------------------------------
if grep -vE '^[[:space:]]*#' "$SCRIPT" | grep -nE 'nandwrite|flash_erase|flash_eraseall|flashcp|mtd_debug|nandtest|nandflipbits|mtdblock|jffs2|ubi|mkfs|fdisk|sfdisk|parted|wipefs|dd |of=/dev'; then
	bad "script mentions a write tool, a formatter or a raw device write"
else ok "script has no write/erase tool, formatter or raw device write, and mounts nothing from the NAND"; fi
$ASH -n "$SCRIPT" && ok "syntax ok ($ASH -n)" || bad "syntax"

[ $fail = 0 ] && echo "backup tests: ALL PASS" || { echo "backup tests: FAILED"; exit 1; }
