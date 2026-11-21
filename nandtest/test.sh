#!/bin/sh
# Logic tests of the NAND write test against a file-backed fake NAND (test/fakenand.c), no hardware.
# Covers: nbtool, the reference-data generator (mkexpected.sh), the install-image builder, and the
# olpc-nandtest flow: happy path, every abort path, fault injection.  Needs gcc, sha256sum; the
# JFFS2 image test needs mkfs.jffs2/sumtool (skipped without them).  Run with busybox ash when present.
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
bad() { echo "  FAIL: $*"; fail=1; }
ok()  { echo "  ok: $*"; }
ASH="sh"; command -v busybox >/dev/null && busybox ash -c : 2>/dev/null && ASH="busybox ash"
SCRIPT=$here/overlay/usr/local/sbin/olpc-nandtest
echo "== nandtest/test.sh (shell: $ASH)"

CC=${CC:-gcc}
$CC -O2 -Wall -Wextra -o "$tmp/nbhash" "$root/backup/src/nbhash.c" || exit 1
$CC -O2 -Wall -Wextra -o "$tmp/nbcmp" "$root/backup/src/nbcmp.c" || exit 1
$CC -O2 -Wall -Wextra -o "$tmp/nbtool" "$here/src/nbtool.c" || exit 1
$CC -O2 -Wall -Wextra -o "$tmp/fakenand" "$here/test/fakenand.c" || exit 1
mkdir -p "$tmp/bin"
for t in nanddump nandwrite flash_erase; do ln -s ../fakenand "$tmp/bin/$t"; done

# ---- nbtool -----------------------------------------------------------------------------
"$tmp/nbtool" gen 5 100000 | sha256sum >"$tmp/g1"; "$tmp/nbtool" gen 5 100000 | sha256sum >"$tmp/g2"; "$tmp/nbtool" gen 6 100000 | sha256sum >"$tmp/g3"
cmp -s "$tmp/g1" "$tmp/g2" && ! cmp -s "$tmp/g1" "$tmp/g3" && [ "$("$tmp/nbtool" gen 5 100003 | wc -c)" = 100003 ] \
	&& ok "nbtool gen: deterministic per seed, exact length" || bad "nbtool gen"
[ "$("$tmp/nbtool" fill 00 70000 | tr -d '\000' | wc -c)" = 0 ] && [ "$("$tmp/nbtool" fill 00 70000 | wc -c)" = 70000 ] && ok "nbtool fill 00" || bad "nbtool fill"
"$tmp/nbtool" fill ff 1000 | "$tmp/nbtool" ff | grep -q '^0 0 1000$' && ok "nbtool ff: all 0xFF" || bad "nbtool ff clean"
{ "$tmp/nbtool" fill ff 500; printf 'x'; "$tmp/nbtool" fill ff 10; } | "$tmp/nbtool" ff | grep -q '^1 500 511$' && ok "nbtool ff: counts and locates a non-FF byte" || bad "nbtool ff dirty"
head -c 2000 /dev/urandom >"$tmp/f.bin"; cp "$tmp/f.bin" "$tmp/f2.bin"; "$tmp/nbtool" flip "$tmp/f2.bin" 77 3
[ "$(cmp -l "$tmp/f.bin" "$tmp/f2.bin" | wc -l)" = 1 ] && cmp -l "$tmp/f.bin" "$tmp/f2.bin" | awk '{ exit !($1 == 78) }' && ok "nbtool flip: exactly one byte changed" || bad "nbtool flip"
# raw pages: 3 pages of 2048+64 ; page 1 carries ECC bytes, one OOB byte differs from the expectation
mkpg() { "$tmp/nbtool" fill ff 2048; if [ "$1" = ecc ]; then printf '\001\002\003'; "$tmp/nbtool" fill ff 61; else "$tmp/nbtool" fill ff 64; fi; }
{ mkpg ecc; mkpg ecc; mkpg ecc; } >"$tmp/r.raw"
"$tmp/nbtool" fill ff 192 >"$tmp/r.exp0"; { printf '\001\002\003'; "$tmp/nbtool" fill ff 61; printf '\001\002\003'; "$tmp/nbtool" fill ff 61; printf '\001\002\003'; "$tmp/nbtool" fill ff 61; } >"$tmp/r.exp"
"$tmp/nbtool" oob -E 0 3 -K 0 3 -x "$tmp/r.exp" -r "$tmp/r.rep" <"$tmp/r.raw" | grep -q '^3 0 0 0 0 0 0$' && ok "nbtool oob: identical OOB, ECC present, data FF" || bad "nbtool oob clean"
cp "$tmp/r.raw" "$tmp/r2.raw"; "$tmp/nbtool" flip "$tmp/r2.raw" $(( 2112 + 2048 + 1 )) 4; "$tmp/nbtool" flip "$tmp/r2.raw" $(( 2 * 2112 + 100 )) 0; "$tmp/nbtool" flip "$tmp/r2.raw" $(( 2 * 2112 + 2048 + 30 )) 2
"$tmp/nbtool" oob -E 0 3 -K 0 3 -x "$tmp/r.exp" -r "$tmp/r.rep" <"$tmp/r2.raw" >"$tmp/r.res"
[ "$(cat "$tmp/r.res")" = "3 1 2 2 2 0 1" ] && grep -q 'page 1 oob+1 got ' "$tmp/r.rep" && ok "nbtool oob: data, OOB (bytes/bits/pages), critical-range diffs counted and reported precisely" || bad "nbtool oob diff: $(cat "$tmp/r.res")"
{ mkpg; } | "$tmp/nbtool" oob -E 0 3 | grep -q '^1 0 0 0 0 1 0$' && ok "nbtool oob: page without ECC bytes counted" || bad "nbtool oob eccff"
"$tmp/nbtool" oobx "$tmp/r.raw" 1 2 | cmp -s - "$tmp/r.exp0" 2>/dev/null; "$tmp/nbtool" oobx "$tmp/r.raw" 1 2 | head -c 3 | cmp -s - "$tmp/r.exp" 2>/dev/null
[ "$("$tmp/nbtool" oobx "$tmp/r.raw" 1 2 | wc -c)" = 128 ] && ok "nbtool oobx: extracts the OOB of the requested pages" || bad "nbtool oobx"

# ---- the fake NAND world ------------------------------------------------------------------
E=8192 W=2048 O=64 NBLK=64 SZ=$(( 64 * 8192 )) B0=10 BN=8 BAD="3 20"
export FAKE_ERASE=$E FAKE_WRITE=$W FAKE_OOB=$O FAKE_PRE=$(( B0 * E )) FAKE_TEST=$(( BN * E )) FAKE_SIZE=$SZ FAKE_BAD="$BAD" FAKE_ECC_OFF=0 FAKE_CM_OFF=14
# mkworld NAME: a fresh NAND image, its backup directory and the reference data derived from it
mkworld() {
	D=$tmp/$1; rm -rf "$D"; mkdir -p "$D/bk" "$D/exp" "$D/sys" "$D/dev" "$D/run" "$D/mnt"
	export FAKE_NAND=$D/nand.img FAKE_LOG=$D/fake.log FAKE_SYS=$D/sys FAKE_COUNT=$D/count FAKE_STORE=$D/jffs2.tar
	: >"$FAKE_LOG"
	"$tmp/fakenand" mkimage 7 $B0 $BN || exit 1
	"$tmp/bin/nanddump" -q --bb=dumpbad "$D/dev/mtdall" >"$D/bk/nand-data-ecc.bin" 2>/dev/null
	"$tmp/bin/nanddump" -q --bb=dumpbad --oob --noecc "$D/dev/mtdall" >"$D/bk/nand-raw-oob.bin"
	cp "$FAKE_NAND" "$D/nand.orig"
	MX_ERASE=$E MX_WRITE=$W MX_OOB=$O MX_SIZE=$SZ MX_BLK0=$B0 MX_BLKS=$BN MX_SIM_BLKS=$NBLK NBHASH="$tmp/nbhash" NBTOOL="$tmp/nbtool" \
		sh "$here/mkexpected.sh" "$D/bk" "$D/exp" >"$D/mkexp.out" 2>&1 || { bad "mkexpected in mkworld: $(cat "$D/mkexp.out")"; return 1; }
	# fake JFFS2: a tiny tree stands in for the content of the install image
	mkdir -p "$D/tree/sub"; echo hello >"$D/tree/a.txt"; "$tmp/nbtool" gen 3 5000 >"$D/tree/sub/b.bin"
	( cd "$D/tree" && find . -type f | sort | xargs sha256sum ) >"$D/exp/install.sha256"
	tar -C "$D/tree" -cf "$D/tree.tar" .
	"$tmp/nbtool" gen 9 $(( 2 * E )) >"$D/exp/install.jffs2"
	for n in 0 1 2; do mkdir -p "$D/sys/mtd$n"; echo 0 >"$D/sys/mtd$n/ecc_failures"; echo 0 >"$D/sys/mtd$n/corrected_bits"; : >"$D/dev/mtd$n"; done
	setsys 0 pre 0x800 $FAKE_PRE 0; setsys 1 test 0xc00 $FAKE_TEST $FAKE_PRE; setsys 2 post 0x800 $(( SZ - FAKE_PRE - FAKE_TEST )) $(( FAKE_PRE + FAKE_TEST ))
	echo "console=tty0 mtdparts=cafe_nand:$(( FAKE_PRE / 1024 ))k(pre)ro,$(( FAKE_TEST / 1024 ))k(test),-(post)ro olpcboy.nandtest" >"$D/cmdline"
}
setsys() {   # setsys N NAME FLAGS SIZE OFFSET
	echo "$2" >"$D/sys/mtd$1/name"; echo nand >"$D/sys/mtd$1/type"; echo "$3" >"$D/sys/mtd$1/flags"; echo "$4" >"$D/sys/mtd$1/size"
	echo "$5" >"$D/sys/mtd$1/offset"; echo $E >"$D/sys/mtd$1/erasesize"; echo $W >"$D/sys/mtd$1/writesize"; echo $O >"$D/sys/mtd$1/oobsize"
}
cat >"$tmp/common.sh" <<'C'
BOOTDIR=$NT_BOOTDIR; mkdir -p "$BOOTDIR"
tl() { echo "$*" >>"$BOOTDIR/timeline.log"; }
olpc_sleep() { :; }
cmdline_has() { return 1; }
_cs() { read -r _cs_up _ </proc/uptime; _cs_s=${_cs_up%.*}; _cs_f=${_cs_up#*.}; _cs_f=${_cs_f#0}; CS=$((_cs_s * 100 + ${_cs_f:-0})); }
C
cat >"$tmp/mountstub" <<'S'
#!/bin/sh
# fake jffs2: umount snapshots the directory, mount restores it; a window that starts with the install image mounts the install tree
case $0 in
*umountstub) echo "umount $*" >>"$STUB_LOG"; tar -C "$1" -cf "$STUB_STORE" . && rm -rf "$1"/* "$1"/.[!.]* 2>/dev/null; exit 0 ;;
esac
echo "mount $*" >>"$STUB_LOG"
[ "$STUB_MOUNTFAIL" = 1 ] && { echo "mount: wrong fs type" >&2; exit 32; }
mnt=$4
if "$FAKE_BIN/nanddump" -q -l 2048 "$FAKE_DEV1" 2>/dev/null | cmp -s -n 2048 - "$STUB_IMG"; then tar -C "$mnt" -xf "$STUB_TREE"
elif [ -f "$STUB_STORE" ]; then tar -C "$mnt" -xf "$STUB_STORE"; fi
[ "$STUB_JFFS2_CORRUPT" = 1 ] && [ -f "$mnt/big.bin" ] && echo x >>"$mnt/big.bin"
exit 0
S
ln -sf mountstub "$tmp/umountstub"; chmod +x "$tmp/mountstub"
cat >"$tmp/splash" <<S
#!/bin/sh
echo "\$*" >>"$tmp/splash.calls"
S
cat >"$tmp/blkids" <<'S'
#!/bin/sh
echo '/dev/sda1: LABEL="OLPCLOG" UUID="AAAA-AAAA" TYPE="vfat"'
echo '/dev/sda2: LABEL="OLPCROOT" UUID="1111" TYPE="ext4"'
[ "$STUB_NANDSTICK" = 1 ] && echo '/dev/sdb1: LABEL="OLPCNAND" UUID="CCCC-CCCC" TYPE="vfat"'
exit 0
S
chmod +x "$tmp/splash" "$tmp/blkids"
# run_nt [VAR=val ...]: the script against the current world ($D)
run_nt() {
	: >"$FAKE_LOG"; rm -f "$D/count".* "$D/jffs2.tar" "$D/run"/* "$tmp/splash.calls"; rm -rf "$D/boot"
	env NT_TEST=1 NT_NOLOAD=1 NT_NOPOWEROFF=1 OLPC_COMMON="$tmp/common.sh" NT_BOOTDIR="$D/boot" \
	  NT_MTD0="$D/dev/mtd0" NT_MTD1="$D/dev/mtd1" NT_MTD2="$D/dev/mtd2" NT_SYSD="$D/sys" \
	  NT_NANDDUMP="$tmp/bin/nanddump" NT_NANDWRITE="$tmp/bin/nandwrite" NT_FLASH_ERASE="$tmp/bin/flash_erase" \
	  NT_NBHASH="$tmp/nbhash" NT_NBCMP="$tmp/nbcmp" NT_NBTOOL="$tmp/nbtool" NT_SPLASH="$tmp/splash" NT_RUN="$D/run" NT_MNT="$D/mnt" NT_TTY="$D/tty" \
	  NT_CMDLINE="$D/cmdline" NT_EXPD="$D/exp" NT_INSTD="$D/exp" NT_BLKIDS="$tmp/blkids" NT_MOUNT="$tmp/mountstub" NT_UMOUNT="$tmp/umountstub" \
	  NT_BLK0=$B0 NT_BLKS=$BN NT_PRE_KIB=$(( FAKE_PRE / 1024 )) NT_TEST_KIB=$(( FAKE_TEST / 1024 )) NT_SIZE=$SZ NT_ERASE=$E NT_WRITE=$W NT_OOB=$O \
	  NT_BADBLKS="$BAD" NT_ECC_OFF=0 NT_ECC_LEN=14 NT_CM_OFF=14 \
	  STUB_LOG="$D/mount.log" STUB_STORE="$D/jffs2.tar" STUB_IMG="$D/exp/install.jffs2" STUB_TREE="$D/tree.tar" FAKE_BIN="$tmp/bin" FAKE_DEV1="$D/dev/mtd1" \
	  "$@" $ASH "$SCRIPT" >"$D/out" 2>&1
}
nowrites() { ! grep -q '^\(nandwrite\|flash_erase\)' "$FAKE_LOG"; }
only_mtd1() { ! grep '^\(nandwrite\|flash_erase\)' "$FAKE_LOG" | grep -vq 'mtd1'; }
restored() { cmp -s "$D/nand.img" "$D/nand.orig"; }
shownt() { sed 's/^/     | /' "$D/out" | tail -n 25; }

# ---- static policy of the script ----------------------------------------------------------
n=$(grep -c '"\$FERASE"\|"\$NWRITE"' "$SCRIPT")
[ "$n" = 2 ] && ok "flash_erase / nandwrite are invoked in exactly two places (w_erase, w_write)" || bad "write tool invocations: $n"
grep '"\$FERASE"\|"\$NWRITE"' "$SCRIPT" | grep -q 'MTD0\|MTD2' && bad "a write tool mentions mtd0/mtd2" || ok "the write primitives address only \$TMTD"
grep -q '^TMTD=\$MTD1' "$SCRIPT" && ok "TMTD is mtd1" || bad "TMTD definition"
$ASH -n "$SCRIPT" && ok "script parses with $ASH" || bad "syntax"

# ---- generator: failure modes --------------------------------------------------------------
mkworld gen || exit 1
MXE="MX_ERASE=$E MX_WRITE=$W MX_OOB=$O MX_SIZE=$SZ MX_BLK0=$B0 MX_BLKS=$BN MX_SIM_BLKS=$NBLK NBHASH=$tmp/nbhash NBTOOL=$tmp/nbtool"
env $MXE sh "$here/mkexpected.sh" "$D/nonexistent" "$tmp/o1" >"$tmp/o1.out" 2>&1; [ $? = 1 ] && grep -q 'not found' "$tmp/o1.out" && ok "mkexpected: missing backup directory fails clearly" || bad "mkexpected missing dir"
mkdir -p "$tmp/bk2"; cp "$D/bk/nand-data-ecc.bin" "$tmp/bk2/"; env $MXE sh "$here/mkexpected.sh" "$tmp/bk2" "$tmp/o2" >"$tmp/o2.out" 2>&1; [ $? = 1 ] && grep -q 'nand-raw-oob.bin' "$tmp/o2.out" && ok "mkexpected: missing raw dump fails" || bad "mkexpected missing raw"
head -c 1000 "$D/bk/nand-data-ecc.bin" >"$tmp/bk2/nand-data-ecc.bin"; cp "$D/bk/nand-raw-oob.bin" "$tmp/bk2/"; env $MXE sh "$here/mkexpected.sh" "$tmp/bk2" "$tmp/o3" >"$tmp/o3.out" 2>&1; [ $? = 1 ] && grep -q 'expected' "$tmp/o3.out" && ok "mkexpected: wrong data size fails" || bad "mkexpected size"
cp "$D/bk/nand-data-ecc.bin" "$tmp/bk2/"; "$tmp/nbtool" flip "$tmp/bk2/nand-data-ecc.bin" $(( (B0 + 2) * E + 5 )) 1
env $MXE sh "$here/mkexpected.sh" "$tmp/bk2" "$tmp/o4" >"$tmp/o4.out" 2>&1; [ $? = 1 ] && grep -q 'not erased' "$tmp/o4.out" && ok "mkexpected: a non-erased window block in the backup fails the build" || bad "mkexpected dirty window"
h=$(sed -n 6p "$D/exp/expected-blocks.txt")
[ "$h" = "5 $(dd if="$D/bk/nand-data-ecc.bin" bs=$E skip=5 count=1 2>/dev/null | sha256sum | cut -d' ' -f1)" ] && [ "$(wc -l <"$D/exp/expected-blocks.txt")" = $NBLK ] \
	&& ok "mkexpected: per-block hashes match sha256sum of the backup blocks" || bad "expected-blocks content"
[ "$(wc -c <"$D/exp/sim/expected-oob.bin")" = $(( BN * (E / W) * O )) ] && [ "$(head -c 24 "$D/exp/sim/expected-oob.bin" | tail -c 8 | od -An -tx1 | tr -d ' \n')" = "ffffffffffffffff" ] \
	&& ok "mkexpected: sim reference data generated" || bad "sim expected"

# ---- install image (needs mkfs.jffs2 + sumtool) -------------------------------------------
if command -v mkfs.jffs2 >/dev/null && command -v sumtool >/dev/null; then
	NBTOOL="$tmp/nbtool" sh "$here/mkinstall.sh" "$tmp/inst" >"$tmp/inst.out" 2>&1 && [ $(( $(wc -c <"$tmp/inst/install.jffs2") % 131072 )) = 0 ] && [ "$(wc -l <"$tmp/inst/install.sha256")" -gt 10 ] \
		&& ok "mkinstall: JFFS2 image (erase-block multiple) and hash list built" || bad "mkinstall: $(cat "$tmp/inst.out")"
	command -v jffs2dump >/dev/null && { jffs2dump -c "$tmp/inst/install.jffs2" 2>&1 | grep -q 'Inode' && ok "mkinstall: image contains JFFS2 nodes (jffs2dump)" || bad "jffs2dump"; }
else echo "  skip: mkfs.jffs2/sumtool not installed (install image test)"; fi

# ---- happy path ------------------------------------------------------------------------------
mkworld happy || exit 1
run_nt; rc=$?
[ $rc = 0 ] && ok "happy path: exit 0" || { bad "happy path rc=$rc"; shownt; }
grep -q 'RESULT: OK' "$D/out" && ok "RESULT: OK" || { bad "no RESULT: OK"; shownt; }
restored && ok "the NAND image is byte-identical to the original afterwards (data, ECC, OOB, cleanmarkers)" || bad "NAND not restored: $(cmp "$D/nand.img" "$D/nand.orig" 2>&1 | head -n 1)"
only_mtd1 && ok "every erase/write call addressed mtd1" || bad "a write went elsewhere: $(grep '^\(nandwrite\|flash_erase\)' "$FAKE_LOG" | grep -v mtd1 | head -n 2)"
grep -q 'flash_erase -j .*mtd1' "$FAKE_LOG" && grep -c 'nandwrite' "$FAKE_LOG" | grep -q '^[4-9]$' && ok "erase -j, random/zero/raw-page/install writes were all issued" || bad "write sequence: $(cat "$FAKE_LOG" | tr '\n' ';')"
[ -s "$D/boot/NANDTEST.txt" ] && grep -q 'RESULT: OK' "$D/boot/NANDTEST.txt" && grep -q 'nandtest: RESULT: OK' "$D/boot/timeline.log" && ok "NANDTEST.txt and timeline lines written" || bad "log files"
for pat in 'random: ECC read-back identical' 'zero: ECC read-back identical' 'original data returned for 1 + 2 flipped bits' 'nanddump reports the corrections' 'files verified after umount + remount' 'install: mounted' 'cleanmarkers and ECC area identical' 'whole NAND\|all 64 erase blocks equal the backup'; do
	grep -q "PASS  .*$pat" "$D/out" && ok "check passed: $pat" || { bad "check missing/failed: $pat"; grep -h 'FAIL' "$D/out" | head -n 4; }
done
grep -q 'precheck ok' "$D/out" && ok "precheck ran first" || bad "precheck line"
grep -q '^progress 100' "$tmp/splash.calls" && ok "splash progress reached 100" || bad "splash progress"
grep -q 'NAND TEST: OK' "$D/tty" && ok "big OK on the console" || bad "tty screen"

# ---- abort paths: nothing may be written ---------------------------------------------------------------
mkworld abort1 || exit 1
"$tmp/nbtool" flip "$D/nand.img" $(( 5 * (E / W) * (W + O) + 100 )) 2      # two data bits in one chunk of block 5 (not a bad block): uncorrectable
"$tmp/nbtool" flip "$D/nand.img" $(( 5 * (E / W) * (W + O) + 101 )) 2
run_nt; rc=$?
[ $rc != 0 ] && nowrites && grep -q 'differs from the backup (blocks: 5)' "$D/out" && grep -q 'ABORTED before any write' "$D/out" && ok "precheck: a changed block outside the bad blocks aborts, names the block, writes nothing" || { bad "precheck abort rc=$rc"; shownt; }
grep -q 'NAND TEST FAILED' "$D/tty" && ok "FAILED screen shown" || bad "failed screen"
mkworld abort2 || exit 1
"$tmp/nbtool" flip "$D/nand.img" $(( 3 * (E / W) * (W + O) + 100 )) 2      # block 3 is a known bad block: tolerated, only reported
run_nt; rc=$?
[ $rc = 0 ] && grep -q 'RESULT: OK' "$D/out" && grep -q '1 inside them' "$D/out" && ok "a hash difference inside a known bad block is reported, not fatal" || { bad "bad-block tolerance rc=$rc"; shownt; }
mkworld abort3 || exit 1
"$tmp/nbtool" flip "$D/nand.img" $(( (B0 + 1) * (E / W) * (W + O) + 7 )) 3      # the window is no longer erased
run_nt; rc=$?
[ $rc != 0 ] && nowrites && grep -q 'differs from the backup' "$D/out" && ok "a dirty window is caught by the precheck, no write" || { bad "dirty window rc=$rc"; shownt; }
mkworld abort4 || exit 1
setsys 0 pre 0xc00 $FAKE_PRE 0
run_nt; rc=$?
[ $rc != 0 ] && nowrites && grep -q 'mtd0 IS WRITEABLE' "$D/out" && ok "layout: a writeable mtd0 aborts before any write" || { bad "mtd0 writeable rc=$rc"; shownt; }
setsys 0 pre 0x800 $FAKE_PRE 0; setsys 2 post 0xc00 $(( SZ - FAKE_PRE - FAKE_TEST )) $(( FAKE_PRE + FAKE_TEST ))
run_nt; [ $? != 0 ] && nowrites && grep -q 'mtd2 IS WRITEABLE' "$D/out" && ok "layout: a writeable mtd2 aborts" || { bad "mtd2 writeable"; shownt; }
setsys 2 post 0x800 $(( SZ - FAKE_PRE - FAKE_TEST )) $(( FAKE_PRE + FAKE_TEST )); setsys 1 test 0x800 $FAKE_TEST $FAKE_PRE
run_nt; [ $? != 0 ] && nowrites && grep -q 'mtd1 is NOT writeable' "$D/out" && ok "layout: a read-only mtd1 aborts" || { bad "mtd1 ro"; shownt; }
setsys 1 test 0xc00 $FAKE_TEST $FAKE_PRE; echo 4096 >"$D/sys/mtd1/erasesize"
run_nt; [ $? != 0 ] && nowrites && grep -q 'mtd1.erasesize=4096' "$D/out" && ok "layout: wrong erase size aborts" || { bad "erasesize"; shownt; }
echo $E >"$D/sys/mtd1/erasesize"; echo $(( FAKE_TEST + E )) >"$D/sys/mtd1/size"
run_nt; [ $? != 0 ] && nowrites && grep -q 'mtd1.size=' "$D/out" && ok "layout: wrong window size aborts" || { bad "size"; shownt; }
echo $FAKE_TEST >"$D/sys/mtd1/size"; echo $(( FAKE_PRE + E )) >"$D/sys/mtd1/offset"
run_nt; [ $? != 0 ] && nowrites && grep -q 'mtd1.offset=' "$D/out" && ok "layout: wrong window offset aborts" || { bad "offset"; shownt; }
echo $FAKE_PRE >"$D/sys/mtd1/offset"; mkdir -p "$D/sys/mtd3"
run_nt; [ $? != 0 ] && nowrites && grep -q '4 mtd devices instead of 3' "$D/out" && ok "layout: an unexpected extra mtd device aborts" || { bad "extra mtd"; shownt; }
rmdir "$D/sys/mtd3"
echo "console=tty0 mtdparts=cafe_nand:-(nand)ro" >"$D/cmdline"
run_nt; [ $? != 0 ] && nowrites && grep -q 'kernel command line lacks' "$D/out" && ok "cmdline: the read-only partition spec is required" || { bad "cmdline"; shownt; }
echo "console=tty0 mtdparts=cafe_nand:$(( FAKE_PRE / 1024 ))k(pre)ro,$(( FAKE_TEST / 1024 ))k(test),-(post)ro mtdparts=cafe_nand:-(nand)" >"$D/cmdline"
run_nt; [ $? != 0 ] && nowrites && grep -q 'kernel command line lacks' "$D/out" && ok "cmdline: a second mtdparts= is refused" || { bad "cmdline dup"; shownt; }
echo "console=tty0 mtdparts=cafe_nand:$(( FAKE_PRE / 1024 ))k(pre)ro,$(( FAKE_TEST / 1024 ))k(test),-(post)ro olpcboy.nandtest" >"$D/cmdline"
rm -rf "$D/sys/mtd2"; rm -f "$D/dev/mtd2"
run_nt; [ $? != 0 ] && nowrites && grep -q 'no NAND found' "$D/out" && ok "no NAND: stops safely with 'no NAND found', nothing written" || { bad "no NAND"; shownt; }
mkworld abort5 || exit 1
run_nt STUB_NANDSTICK=1 ; [ $? = 0 ] && grep -q 'OLPCNAND.*not used and not touched' "$D/out" && ! grep -q '/dev/sdb1' "$D/mount.log" 2>/dev/null && ok "an OLPCNAND stick that is merely present is noted and never mounted" || { bad "OLPCNAND present"; shownt; }
mkworld abort6 || exit 1
printf '/dev/sdb1 /mnt vfat rw 0 0\n' >"$D/mounts"
# the script reads /proc/mounts: emulate by a blkids stub that labels the device found in the real /proc/mounts
dev=$(awk '$1 ~ /^\/dev\// { print $1; exit }' /proc/mounts)
if [ -n "$dev" ]; then
	cat >"$tmp/blkids2" <<S
#!/bin/sh
echo '$dev: LABEL="OLPCNAND" TYPE="vfat"'
S
	chmod +x "$tmp/blkids2"
	run_nt NT_BLKIDS="$tmp/blkids2"; [ $? != 0 ] && nowrites && grep -q 'OLPCNAND backup stick is mounted' "$D/out" && ok "the OLPCNAND stick mounted (e.g. as the log device) aborts" || { bad "OLPCNAND mounted"; shownt; }
else echo "  skip: no /dev mount to emulate a mounted OLPCNAND stick"; fi

# ---- fault injection --------------------------------------------------------------------------------------
mkworld fault1 || exit 1
run_nt FAKE_FAIL_WRITE=1; rc=$?                                       # the very first nandwrite fails
[ $rc != 0 ] && grep -q 'FATAL: nandwrite failed' "$D/out" && ok "a failing nandwrite stops the run (FATAL)" || { bad "fail write rc=$rc"; shownt; }
[ "$(grep -c '^flash_erase\|^nandwrite' "$FAKE_LOG")" = 2 ] && grep -q 'was written and NOT restored' "$D/out" && ok "no further writes after the failure; state reported as not restored" || { bad "writes after fatal: $(tr '\n' ';' <"$FAKE_LOG")"; }
mkworld fault2 || exit 1
run_nt FAKE_FAIL_ERASE=1; rc=$?                                       # the very first erase (step 1) fails
[ $rc != 0 ] && grep -q 'FATAL: flash_erase failed' "$D/out" && [ "$(grep -c '^flash_erase\|^nandwrite' "$FAKE_LOG")" = 1 ] && ok "a failing flash_erase stops the run with a single write attempt" || { bad "fail erase rc=$rc"; shownt; }
mkworld fault3 || exit 1
run_nt FAKE_WRITE_BAD=1; rc=$?                                        # data stored with an uncorrectable error
[ $rc != 0 ] && grep -q 'FAIL  random: ECC read-back identical' "$D/out" && grep -q 'RESULT: FAILED' "$D/out" && ok "corrupted writes are reported as FAIL" || { bad "corrupt write rc=$rc"; shownt; }
restored && ok "...and the window is still restored; the final compare passes" || bad "fault3 not restored"
mkworld fault4 || exit 1
run_nt FAKE_NO_CM=1; rc=$?                                            # erase -j writes no cleanmarker
[ $rc != 0 ] && grep -q 'FAIL  restore: cleanmarkers and ECC area identical' "$D/out" && ok "a missing cleanmarker after the restore is a FAIL" || { bad "no cleanmarker rc=$rc"; shownt; }
mkworld fault5 || exit 1
run_nt STUB_MOUNTFAIL=1; rc=$?
[ $rc != 0 ] && grep -q 'FAIL  jffs2: mount' "$D/out" && grep -q 'FAIL  install: mount' "$D/out" && restored && ok "mount failures are FAILs; the window is still restored" || { bad "mount fail rc=$rc"; shownt; }
mkworld fault6 || exit 1
run_nt STUB_JFFS2_CORRUPT=1; rc=$?
[ $rc != 0 ] && grep -q 'FAIL  jffs2: files verified after remount' "$D/out" && ok "a hash mismatch after the JFFS2 remount is a FAIL" || { bad "jffs2 corrupt rc=$rc"; shownt; }

[ "$fail" = 0 ] && echo "nandtest/test.sh: PASS" || { echo "nandtest/test.sh: FAILED"; exit 1; }
