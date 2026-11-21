#!/bin/sh
# Logic tests of the NAND installer / restore (olpc-install) against a file-backed fake NAND
# (nandtest/test/fakenand.c), fake JFFS2 mounts and a fake backup stick.  No hardware.
# Covers: the reference-data generator, nbjffs, the install flow (happy path with bad blocks, every
# abort path, fault injection, re-install gate) and the restore flow.  Needs gcc, sha256sum, strings.
# Run with busybox ash when present (the target's shell).
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
bad() { echo "  FAIL: $*"; fail=1; }
ok()  { echo "  ok: $*"; }
ASH="sh"; command -v busybox >/dev/null && busybox ash -c : 2>/dev/null && ASH="busybox ash"
SCRIPT=$here/overlay/usr/local/sbin/olpc-install
echo "== install/test.sh (shell: $ASH)"

CC=${CC:-gcc}
$CC -O2 -Wall -Wextra -o "$tmp/nbhash" "$root/backup/src/nbhash.c" || exit 1
$CC -O2 -Wall -Wextra -o "$tmp/nbcmp" "$root/backup/src/nbcmp.c" || exit 1
$CC -O2 -Wall -Wextra -o "$tmp/nbtool" "$root/nandtest/src/nbtool.c" || exit 1
$CC -O2 -Wall -Wextra -o "$tmp/nbjffs" "$here/src/nbjffs.c" || exit 1
$CC -O2 -Wall -Wextra -o "$tmp/fakenand" "$root/nandtest/test/fakenand.c" || exit 1
mkdir -p "$tmp/bin"
for t in nanddump nandwrite flash_erase; do ln -s ../fakenand "$tmp/bin/$t"; done

# ---- static policy of the script ----------------------------------------------------------
n=$(grep -c '"\$FERASE"\|"\$NWRITE"' "$SCRIPT")
[ "$n" = 2 ] && ok "flash_erase / nandwrite are invoked in exactly two places (w_erase, w_write)" || bad "write tool invocations: $n"
grep '"\$FERASE"\|"\$NWRITE"' "$SCRIPT" | grep -q 'BMTD\|MTD1' && bad "a write tool mentions the bbt device" || ok "the write primitives address only \$PMTD (mtd0)"
grep -q '^PMTD=\${IT_MTD0:-/dev/mtd0}' "$SCRIPT" && ok "PMTD is mtd0" || bad "PMTD definition"
grep -q 'MOUNT -t vfat -o ro,' "$SCRIPT" && ! grep -q 'MOUNT -t vfat -o rw' "$SCRIPT" && ok "the backup stick is mounted read-only only" || bad "backup stick mount options"
$ASH -n "$SCRIPT" && ok "script parses with $ASH" || bad "syntax"

# ---- nbjffs --------------------------------------------------------------------------------
E=8192 W=2048 O=64 NBLK=64 PBLK=60 SZ=$(( 64 * 8192 )) BAD="3 20"
MARK=OLPCBOY-NAND-INSTALL-1
# mkimg NBLOCKS OUT [nosum]: fake JFFS2 image: marker in block 0, summary marker at the end of every block but the last
mkimg() {
	_n=$1; _o=$2; _i=0; : >"$_o"
	while [ "$_i" -lt "$_n" ]; do
		if [ "$_i" = 0 ]; then printf 'JJ.olpcboy-nand\n%s\n' "${IMGMARK:-$MARK}" >"$tmp/blk"; else : >"$tmp/blk"; fi
		_have=$(wc -c <"$tmp/blk")
		"$tmp/nbtool" gen "$(( _i + 50 ))" "$(( E - _have - 8 ))" >>"$tmp/blk"
		if [ "$_i" -lt "$(( _n - 1 ))" ] && [ "$3" != nosum ]; then printf '\001\000\000\000\205\030\205\002' >>"$tmp/blk"; else printf '\377\377\377\377\377\377\377\377' >>"$tmp/blk"; fi
		cat "$tmp/blk" >>"$_o"
		_i=$(( _i + 1 ))
	done
}
mkimg 5 "$tmp/img5"
[ "$("$tmp/nbjffs" sum $E <"$tmp/img5")" = "5 5 4 5" ] && ok "nbjffs sum: 5 blocks, 5 used, 4 with a summary marker, last used 5" || bad "nbjffs: $("$tmp/nbjffs" sum $E <"$tmp/img5")"
{ cat "$tmp/img5"; "$tmp/nbtool" fill ff $(( 2 * E )); } | "$tmp/nbjffs" sum $E | grep -q '^7 5 4 5$' && ok "nbjffs sum: trailing erased blocks are not 'used'" || bad "nbjffs trailing"

# ---- the fake NAND world -------------------------------------------------------------------
export FAKE_ERASE=$E FAKE_WRITE=$W FAKE_OOB=$O FAKE_PRE=$(( PBLK * E )) FAKE_TEST=0 FAKE_SIZE=$SZ FAKE_BAD="$BAD" FAKE_ECC_OFF=0 FAKE_CM_OFF=14 FAKE_LAYOUT_INSTALL=1
# mkworld NAME [IMGBLOCKS]: a fresh NAND, its backup directory, reference data, installer data and a stick
mkworld() {
	D=$tmp/$1; rm -rf "$D"; mkdir -p "$D/bk" "$D/exp" "$D/sys" "$D/dev" "$D/run" "$D/mnt" "$D/stick/roms/gb" "$D/stick/roms/gbc" "$D/stick/roms/saves" "$D/stick/roms/states" "$D/stick/covers/gb" "$D/bkmnt" "$D/bkstick"
	export FAKE_NAND=$D/nand.img FAKE_LOG=$D/fake.log FAKE_SYS=$D/sys FAKE_COUNT=$D/count FAKE_STORE=$D/jffs2.tar
	: >"$FAKE_LOG"
	"$tmp/fakenand" mkimage 7 0 0 || exit 1
	"$tmp/bin/nanddump" -q --bb=dumpbad "$D/dev/mtdall" >"$D/bk/nand-data-ecc.bin" 2>/dev/null
	cp "$FAKE_NAND" "$D/nand.orig"
	"$tmp/nbtool" gen 77 $(( 10 * E )) >"$D/bk/NANDCOPY.DAT"      # the same bytes mkrestore 10 puts on the fake backup stick
	MX_PART_BLKS=$PBLK MX_BAD="$BAD" MX_ERASE=$E MX_SIZE=$SZ MX_SIM_BLKS=$NBLK NBHASH="$tmp/nbhash" NBTOOL="$tmp/nbtool" sh "$here/mkexpected.sh" "$D/bk" "$D/exp" >"$D/mkexp.out" 2>&1 \
		|| { bad "mkexpected in mkworld: $(cat "$D/mkexp.out")"; return 1; }
	# the install image and the tree it stands for
	mkimg "${2:-6}" "$D/exp/nand-root.jffs2"
	mkdir -p "$D/tree/sub" "$D/tree/boot" "$D/tree/sbin" "$D/tree/data/olpclog"
	printf '%s\nkernel x\n' "$MARK" >"$D/tree/.olpcboy-nand"
	printf '\\ OLPC boot script\n' >"$D/tree/boot/olpc.fth"; echo k >"$D/tree/boot/bzImage"; echo i >"$D/tree/boot/initrd.img"
	printf '#!/bin/sh\n' >"$D/tree/sbin/init"; chmod 755 "$D/tree/sbin/init"
	echo hello >"$D/tree/a.txt"; "$tmp/nbtool" gen 3 5000 >"$D/tree/sub/b.bin"
	( cd "$D/tree" && find . -type f | sort | xargs sha256sum ) >"$D/exp/nand-root.sha256"
	tar -C "$D/tree" -cf "$D/tree.tar" .
	for n in 0 1; do mkdir -p "$D/sys/mtd$n"; echo 0 >"$D/sys/mtd$n/ecc_failures"; echo 0 >"$D/sys/mtd$n/corrected_bits"; echo 2 >"$D/sys/mtd$n/bad_blocks"; : >"$D/dev/mtd$n"; done
	setsys 0 olpcboy 0xc00 $(( PBLK * E )) 0; setsys 1 bbt 0x800 $(( SZ - PBLK * E )) $(( PBLK * E ))
	echo "console=tty0 mtdparts=cafe_nand:$(( PBLK * E / 1024 ))k(olpcboy),-(bbt)ro olpcboy.install" >"$D/cmdline"
	# the stick: opaque WIFI.CFG, saves/states/covers/ROMs
	echo "dummy" >"$D/stick/WIFI.CFG"
	echo s1 >"$D/stick/roms/saves/a.sav"; echo st1 >"$D/stick/roms/states/a.st"; echo c1 >"$D/stick/covers/gb/a.obc"
	echo r1 >"$D/stick/roms/gb/game.gb"; echo r2 >"$D/stick/roms/gbc/game.gbc"
	: >"$D/mounts"
	cp "$D/nand.orig" "$D/nand.v0"
}
reset() { cp "$D/nand.v0" "$D/nand.img"; cp "$D/nand.v0" "$D/nand.orig"; : >"$D/mounts"; }
setsys() {   # setsys N NAME FLAGS SIZE OFFSET
	echo "$2" >"$D/sys/mtd$1/name"; echo nand >"$D/sys/mtd$1/type"; echo "$3" >"$D/sys/mtd$1/flags"; echo "$4" >"$D/sys/mtd$1/size"
	echo "$5" >"$D/sys/mtd$1/offset"; echo $E >"$D/sys/mtd$1/erasesize"; echo $W >"$D/sys/mtd$1/writesize"; echo $O >"$D/sys/mtd$1/oobsize"
}
cat >"$tmp/common.sh" <<'C'
BOOTDIR=$IT_BOOTDIR; mkdir -p "$BOOTDIR"
tl() { echo "$*" >>"$BOOTDIR/timeline.log"; }
olpc_sleep() { :; }
cmdline_has() { for _w in $IT_FLAGS; do [ "$_w" = "$1" ] && return 0; done; return 1; }
_cs() { read -r _cs_up _ </proc/uptime; _cs_s=${_cs_up%.*}; _cs_f=${_cs_up#*.}; _cs_f=${_cs_f#0}; CS=$((_cs_s * 100 + ${_cs_f:-0})); }
C
cat >"$tmp/mountstub" <<'S'
#!/bin/sh
# fake mounts: jffs2 = the install tree (or what the last umount stored); vfat = the backup stick directory
case $0 in
*umountstub)
	echo "umount $*" >>"$STUB_LOG"
	if [ "$1" = "$STUB_BKMNT" ]; then
		grep -v " $1 " "$STUB_MOUNTS" >"$STUB_MOUNTS.n"; mv "$STUB_MOUNTS.n" "$STUB_MOUNTS"; rm -rf "$1"/* 2>/dev/null; exit 0
	fi
	[ "$STUB_UMOUNTFAIL" = 1 ] && { echo "umount: busy" >&2; exit 1; }
	tar -C "$1" -cf "$STUB_STORE" . && rm -rf "$1"/* "$1"/.[!.]* 2>/dev/null; exit 0 ;;
esac
echo "mount $*" >>"$STUB_LOG"
if [ "$1" = -o ]; then [ "$STUB_REMOUNTFAIL" = 1 ] && exit 32; exit 0; fi
type=$2; dev=$5; mnt=$6
case $type in
vfat)
	[ "$STUB_BKMOUNTFAIL" = 1 ] && { echo "mount: bad superblock" >&2; exit 32; }
	cp -R "$STUB_BKSTICK"/. "$mnt"/
	o=ro; [ "$STUB_BKRW" = 1 ] && o=rw
	echo "$dev $mnt vfat $o,noatime 0 0" >>"$STUB_MOUNTS"; exit 0 ;;
jffs2)
	[ "$STUB_MOUNTFAIL" = 1 ] && { echo "mount: wrong fs type" >&2; exit 32; }
	if "$FAKE_BIN/nanddump" -q --bb=skipbad -l 2048 "$FAKE_DEV0" 2>/dev/null | cmp -s -n 2048 - "$STUB_IMG"; then
		if [ -f "$STUB_STORE" ]; then tar -C "$mnt" -xf "$STUB_STORE"; else tar -C "$mnt" -xf "$STUB_TREE"; fi
	elif [ "$STUB_MOUNT_ANY" = 1 ]; then tar -C "$mnt" -xf "$STUB_TREE"
	else echo "mount: wrong fs type" >&2; exit 32; fi
	[ "$STUB_JFFS2_CORRUPT" = 1 ] && [ -f "$mnt/sub/b.bin" ] && echo x >>"$mnt/sub/b.bin"
	exit 0 ;;
esac
exit 1
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
[ "$STUB_NANDSTICK_LATE" = 1 ] && [ -f "$STUB_LOG.late" ] && echo '/dev/sdb1: LABEL="OLPCNAND" UUID="CCCC-CCCC" TYPE="vfat"'
exit 0
S
chmod +x "$tmp/splash" "$tmp/blkids"
# run_it [VAR=val ...]: the script against the current world ($D)
run_it() {
	: >"$FAKE_LOG"; rm -f "$D/count".* "$D/jffs2.tar" "$D/run"/* "$tmp/splash.calls" "$D/mount.log"; rm -rf "$D/boot" "$D/mnt"/* "$D/mnt"/.[!.]* "$D/bkmnt"/*
	env IT_TEST=1 IT_NOLOAD=1 IT_NOPOWEROFF=1 OLPC_COMMON="$tmp/common.sh" IT_BOOTDIR="$D/boot" \
	  IT_MTD0="$D/dev/mtd0" IT_MTD1="$D/dev/mtd1" IT_SYSD="$D/sys" \
	  IT_NANDDUMP="$tmp/bin/nanddump" IT_NANDWRITE="$tmp/bin/nandwrite" IT_FLASH_ERASE="$tmp/bin/flash_erase" \
	  IT_NBHASH="$tmp/nbhash" IT_NBCMP="$tmp/nbcmp" IT_NBTOOL="$tmp/nbtool" IT_NBJFFS="$tmp/nbjffs" IT_SPLASH="$tmp/splash" IT_RUN="$D/run" IT_MNT="$D/mnt" IT_BKMNT="$D/bkmnt" IT_TTY="$D/tty" \
	  IT_CMDLINE="$D/cmdline" IT_EXPD="$D/exp" IT_INSTD="$D/exp" IT_BLKIDS="$tmp/blkids" IT_MOUNT="$tmp/mountstub" IT_UMOUNT="$tmp/umountstub" IT_MOUNTS="$D/mounts" IT_STICK="$D/stick" \
	  IT_SIZE=$SZ IT_ERASE=$E IT_WRITE=$W IT_OOB=$O IT_PART_BLKS=$PBLK IT_BBT_BLKS=$(( NBLK - PBLK )) IT_BADBLKS="$BAD" IT_RESTORE_WAIT=3 \
	  STUB_LOG="$D/mount.log" STUB_STORE="$D/jffs2.tar" STUB_IMG="$D/exp/nand-root.jffs2" STUB_TREE="$D/tree.tar" STUB_MOUNTS="$D/mounts" STUB_BKMNT="$D/bkmnt" STUB_BKSTICK="$D/bkstick" \
	  FAKE_BIN="$tmp/bin" FAKE_DEV0="$D/dev/mtd0" \
	  "$@" $ASH "$SCRIPT" >"$D/out" 2>&1
}
nowrites() { ! grep -q '^\(nandwrite\|flash_erase\)' "$FAKE_LOG"; }
only_mtd0() { ! grep '^\(nandwrite\|flash_erase\)' "$FAKE_LOG" | grep -vq 'mtd0'; }
untouched() { cmp -s "$D/nand.img" "$D/nand.orig"; }
bbt_untouched() { _o=$(( PBLK * E / W * (W + O) + 1 )); [ "$(tail -c +$_o "$D/nand.img" | sha256sum)" = "$(tail -c +$_o "$D/nand.orig" | sha256sum)" ]; }
shownt() { sed 's/^/     | /' "$D/out" | tail -n 25; }
# expect_abort DESCRIPTION REGEX [writes-ok]: the run failed, said REGEX, and (unless writes-ok) wrote nothing
expect_abort() {
	_rc=$?
	if [ "$_rc" != 0 ] && grep -q "$2" "$D/out" && grep -q 'INSTALL FAILED\|RESTORE FAILED\|NAND INSTALL FAILED\|NAND RESTORE FAILED' "$D/tty" 2>/dev/null; then
		if [ "$3" = writes-ok ] || { nowrites && untouched; }; then ok "$1"; else bad "$1 (wrote something)"; shownt; fi
	else bad "$1 (rc=$_rc)"; shownt; fi
}

# ---- generator: failure modes --------------------------------------------------------------
mkworld gen || exit 1
env MX_ERASE=$E MX_SIZE=$SZ MX_SIM_BLKS=$NBLK NBHASH="$tmp/nbhash" NBTOOL="$tmp/nbtool" sh "$here/mkexpected.sh" "$D/nonexistent" "$tmp/o1" >"$tmp/o1.out" 2>&1; [ $? = 1 ] && grep -q 'not found' "$tmp/o1.out" && ok "mkexpected: missing backup directory fails clearly" || bad "mkexpected missing dir"
head -c 1000 "$D/bk/nand-data-ecc.bin" >"$tmp/short.bin"; mkdir -p "$tmp/shortbk"; mv "$tmp/short.bin" "$tmp/shortbk/nand-data-ecc.bin"
env MX_ERASE=$E MX_SIZE=$SZ NBHASH="$tmp/nbhash" NBTOOL="$tmp/nbtool" sh "$here/mkexpected.sh" "$tmp/shortbk" "$tmp/o2" >"$tmp/o2.out" 2>&1; [ $? = 1 ] && grep -q 'bytes, expected' "$tmp/o2.out" && ok "mkexpected: a backup of the wrong size fails" || bad "mkexpected wrong size"
[ "$(wc -l <"$D/exp/expected-blocks-restored.txt")" = "$NBLK" ] && ok "mkexpected: restored-layout list has one line per erase block" || bad "restored list length"
[ "$(wc -l <"$D/exp/expected-blocks.txt")" = "$NBLK" ] && [ "$(wc -l <"$D/exp/sim/expected-blocks.txt")" = "$NBLK" ] && ok "mkexpected: one hash line per erase block (real and sim lists)" || bad "mkexpected list lengths"

# ---- install: happy path ----------------------------------------------------------------------
echo "-- install"
mkworld ok || exit 1
run_it; rc=$?
if [ $rc = 0 ] && grep -q 'RESULT: OK' "$D/out" && grep -q 'INSTALLED' "$D/tty"; then ok "happy path: RESULT: OK, INSTALLED screen"; else bad "happy path rc=$rc"; shownt; fi
only_mtd0 && ok "only mtd0 was erased/written" || bad "write outside mtd0"
bbt_untouched && ok "the bad block table blocks (mtd1) are byte-identical to the original" || bad "bbt blocks changed"
grep -q '^flash_erase -j' "$FAKE_LOG" && [ "$(grep -c '^flash_erase' "$FAKE_LOG")" = 1 ] && [ "$(grep -c '^nandwrite' "$FAKE_LOG")" = 1 ] && ok "exactly one flash_erase -j and one nandwrite" || bad "write calls: $(cat "$FAKE_LOG")"
# the image sits in the first good blocks, skipping the bad block 3
IMGSZ=$(wc -c <"$D/exp/nand-root.jffs2")
"$tmp/bin/nanddump" -q --bb=skipbad -l "$IMGSZ" "$D/dev/mtd0" 2>/dev/null | cmp -s - "$D/exp/nand-root.jffs2" && ok "the image is in the first good blocks (bad block 3 skipped)" || bad "image not on the NAND"
"$tmp/bin/nanddump" -q --bb=dumpbad -s $(( 3 * E )) -l $E "$D/dev/mtd0" 2>/dev/null | cmp -s -n 100 - "$D/bk/nand-data-ecc.bin" 2>/dev/null; true
set -- $("$tmp/bin/nanddump" -q --bb=skipbad -s $(( 20 * E )) -l $(( 3 * E )) "$D/dev/mtd0" 2>/dev/null | "$tmp/nbtool" ff); [ "$1" = 0 ] && ok "blocks after the image are erased" || bad "tail not erased ($1)"
grep -q 'precheck ok: the NAND equals the backup (original layout)' "$D/out" && ok "gate: the backup precheck ran and passed" || bad "no precheck"
grep -q 'PASS  files: ' "$D/out" && grep -q 'PASS  readback: the written region equals the image' "$D/out" && ok "readback and file checks passed" || bad "readback/files checks"
[ -f "$D/boot/INSTALL.TXT" ] && [ -f "$D/boot/install/INSTALL.TXT" ] && ok "log written (INSTALL.TXT)" || bad "no INSTALL.TXT"
tar -tf "$D/jffs2.tar" | grep -q 'data/olpclog/WIFI.CFG' && ok "WIFI.CFG copied into /data/olpclog" || bad "WIFI.CFG not copied"
tar -tf "$D/jffs2.tar" | grep -q 'data/roms/saves/a.sav' && tar -tf "$D/jffs2.tar" | grep -q 'data/roms/states/a.st' && tar -tf "$D/jffs2.tar" | grep -q 'data/covers/gb/a.obc' && ok "saves, states and covers copied" || bad "saves/states/covers"
tar -tf "$D/jffs2.tar" | grep -q 'data/roms/gb/game.gb' && tar -tf "$D/jffs2.tar" | grep -q 'data/roms/gbc/game.gbc' && ok "ROMs copied (they fit)" || bad "ROMs"
grep -qi 'dummy' "$D/out" "$D/boot/INSTALL.TXT" && bad "WIFI.CFG content leaked into the log" || ok "WIFI.CFG content is not in the log"
grep -q 'umount' "$D/mount.log" && [ "$(tail -n 1 "$D/mount.log")" = "umount $D/mnt" ] && ok "the new root is unmounted at the end" || bad "final umount: $(tail -n 2 "$D/mount.log" | tr '\n' ';')"
grep -q 'progress 4 ' "$tmp/splash.calls" && grep -q 'progress 100 ' "$tmp/splash.calls" && ok "splash progress reached 100" || bad "splash progress"

# ---- install: aborts before any write -------------------------------------------------------------
mkworld ab || exit 1
# a block that changed since the backup (block 30, not a bad block): flip one data byte
flipblk() { _o=$(( $1 * E / W * (W + O) + 100 )); "$tmp/nbtool" flip "$D/nand.img" "$_o" 0; "$tmp/nbtool" flip "$D/nand.img" "$(( _o + 1 ))" 0; }   # 2 bits in one ECC chunk: uncorrectable
flipblk 30; cp "$D/nand.img" "$D/nand.orig"
run_it; expect_abort "a NAND that differs from the backup (block 30): aborted, nothing written" 'matches neither the backup (differs in blocks: 30)'
reset
flipblk 20; cp "$D/nand.img" "$D/nand.orig"
run_it; rc=$?
[ $rc = 0 ] && grep -q '1 differ inside them' "$D/out" && ok "a hash difference inside a known bad block (20) is reported, not fatal" || { bad "bad-block tolerance rc=$rc"; shownt; }
 reset
echo "console=tty0 olpcboy.install" >"$D/cmdline"; run_it; expect_abort "cmdline without mtdparts: aborted" 'kernel command line lacks'
echo "mtdparts=cafe_nand:$(( PBLK * E / 1024 ))k(olpcboy),-(bbt)ro mtdparts=cafe_nand:-(x) olpcboy.install" >"$D/cmdline"; run_it; expect_abort "a second mtdparts= is refused" 'kernel command line lacks'
echo "mtdparts=cafe_nand:$(( PBLK * E / 1024 + 8 ))k(olpcboy),-(bbt)ro olpcboy.install" >"$D/cmdline"; run_it; expect_abort "a different partition size on the command line is refused" 'kernel command line lacks'
echo "mtdparts=cafe_nand:$(( PBLK * E / 1024 ))k(olpcboy),-(bbt) olpcboy.install" >"$D/cmdline"; run_it; expect_abort "a writeable bbt partition on the command line is refused" 'kernel command line lacks'
echo "console=tty0 mtdparts=cafe_nand:$(( PBLK * E / 1024 ))k(olpcboy),-(bbt)ro olpcboy.install" >"$D/cmdline"
# sysfs layout
setsys 0 olpcboy 0x800 $(( PBLK * E )) 0; run_it; expect_abort "mtd0 read-only in sysfs: aborted" 'is NOT writeable'; setsys 0 olpcboy 0xc00 $(( PBLK * E )) 0
setsys 1 bbt 0xc00 $(( SZ - PBLK * E )) $(( PBLK * E )); run_it; expect_abort "a writeable bbt partition in sysfs: aborted" 'IS WRITEABLE'; setsys 1 bbt 0x800 $(( SZ - PBLK * E )) $(( PBLK * E ))
setsys 0 olpcboy 0xc00 $(( (PBLK - 1) * E )) 0; run_it; expect_abort "a wrong mtd0 size: aborted" 'mtd0.size='; setsys 0 olpcboy 0xc00 $(( PBLK * E )) 0
echo wrong >"$D/sys/mtd0/name"; run_it; expect_abort "a wrong mtd0 name: aborted" 'mtd0.name='; echo olpcboy >"$D/sys/mtd0/name"
mkdir -p "$D/sys/mtd2"; run_it; expect_abort "a third mtd device: aborted" '3 mtd devices instead of 2'; rm -rf "$D/sys/mtd2"
echo 4096 >"$D/sys/mtd0/writesize"; run_it; expect_abort "a wrong page size: aborted" 'writesize='; echo $W >"$D/sys/mtd0/writesize"
# installer data
mv "$D/exp/nand-root.jffs2" "$D/exp/x.jffs2"; run_it; expect_abort "image missing: aborted" 'installer data missing'; mv "$D/exp/x.jffs2" "$D/exp/nand-root.jffs2"
cp "$D/exp/nand-root.jffs2" "$D/exp/keep.jffs2"; head -c $(( 2 * E + 100 )) "$D/keep.jffs2" 2>/dev/null; head -c $(( 2 * E + 100 )) "$D/exp/keep.jffs2" >"$D/exp/nand-root.jffs2"
run_it; expect_abort "image of a bad size: aborted" 'bad size'; cp "$D/exp/keep.jffs2" "$D/exp/nand-root.jffs2"
mkimg 6 "$D/exp/nand-root.jffs2" nosum; run_it; expect_abort "image without summary nodes: aborted" 'no summary nodes'
IMGMARK=WRONG-MARKER-0000000 mkimg 6 "$D/exp/nand-root.jffs2"; run_it; expect_abort "image without the marker: aborted" 'lacks the olpc-boy marker'
mkimg 6 "$D/exp/nand-root.jffs2"
head -n $(( NBLK - 1 )) "$D/exp/expected-blocks.txt" >"$D/exp/x.txt"; mv "$D/exp/expected-blocks.txt" "$D/exp/eb.keep"; mv "$D/exp/x.txt" "$D/exp/expected-blocks.txt"
run_it; expect_abort "an expected-block list of the wrong length: aborted" 'wrong length'; mv "$D/exp/eb.keep" "$D/exp/expected-blocks.txt"
run_it STUB_NANDSTICK=1; rc=$?
grep -q 'OLPCNAND' "$D/out" && [ $rc = 0 ] && ! grep -q 'vfat' "$D/mount.log" && ok "an OLPCNAND stick present is noted and never mounted by the installer" || { bad "OLPCNAND note rc=$rc"; shownt; }
reset
echo "/dev/sdb1 /x vfat ro 0 0" >"$D/mounts"; run_it STUB_NANDSTICK=1; expect_abort "the OLPCNAND stick already mounted: aborted" 'backup stick is mounted'; reset

# ---- install: faults after the gate -----------------------------------------------------------------
# erase fails
reset
run_it FAKE_FAIL_ERASE=1; rc=$?
[ $rc != 0 ] && grep -q 'FATAL: flash_erase failed' "$D/out" && [ "$(grep -c '^flash_erase\|^nandwrite' "$FAKE_LOG")" = 1 ] && grep -q 'ERASED' "$D/out" && ok "a failing flash_erase stops the run; one write attempt; the state says ERASED" || { bad "fail erase rc=$rc"; shownt; }
reset
run_it FAKE_FAIL_WRITE=1; rc=$?
[ $rc != 0 ] && grep -q 'FATAL: nandwrite failed' "$D/out" && [ "$(grep -c '^flash_erase\|^nandwrite' "$FAKE_LOG")" = 2 ] && ok "a failing nandwrite stops the run; no further writes" || { bad "fail write rc=$rc"; shownt; }
grep -q 'REINSTALL' "$D/tty" 2>/dev/null; reset
run_it FAKE_WRITE_BAD=1; rc=$?
[ $rc != 0 ] && grep -q 'does not match what was written' "$D/out" && [ ! -f "$D/mount.log" ] && ok "data stored with an uncorrectable error is caught by the readback (no mount attempted)" || { bad "readback fault rc=$rc"; shownt; }
reset
run_it STUB_MOUNTFAIL=1; rc=$?
[ $rc != 0 ] && grep -q 'does not mount as JFFS2' "$D/out" && ok "a JFFS2 that does not mount: FATAL, state reported" || { bad "mount fail rc=$rc"; shownt; }
reset
run_it STUB_JFFS2_CORRUPT=1; rc=$?
[ $rc != 0 ] && grep -q 'files on the NAND are not the expected ones' "$D/out" && ok "a file that does not match the SHA-256 manifest: FATAL" || { bad "manifest rc=$rc"; shownt; }
reset
run_it STUB_REMOUNTFAIL=1; rc=$?
[ $rc = 0 ] && grep -q 'WARN  could not remount' "$D/out" && grep -q 'INSTALLED' "$D/tty" && ok "a failed read-write remount is a warning: INSTALLED with the warning shown" || { bad "remount fail rc=$rc"; shownt; }
reset
run_it STUB_UMOUNTFAIL=1; rc=$?
[ $rc != 0 ] && grep -q 'cannot unmount the new NAND root' "$D/out" && ok "a failing final unmount is FATAL" || { bad "umount fail rc=$rc"; shownt; }
reset
rm -f "$D/stick/WIFI.CFG"; run_it; rc=$?
[ $rc = 0 ] && grep -q 'WIFI.CFG among them: no' "$D/out" && ok "no WIFI.CFG on the stick: installs fine" || { bad "no wifi rc=$rc"; shownt; }

# ---- install: re-install gate ------------------------------------------------------------------------
echo "-- re-install gate"
mkworld re || exit 1
run_it; [ $? = 0 ] || bad "re-install world: first install failed"
cp "$D/nand.img" "$D/nand.inst"
run_it; expect_abort "an installed NAND without the REINSTALL flag: aborted (matches neither list)" 'matches neither the backup' writes-ok
cp "$D/nand.inst" "$D/nand.img"; cp "$D/nand.inst" "$D/nand.orig"
nowrites && ok "(that abort wrote nothing)" || bad "abort wrote"
touch "$D/stick/reinstall"; run_it; rc=$?
[ $rc = 0 ] && grep -q 'carries the olpc-boy marker: re-install allowed' "$D/out" && ! grep -q 'precheck ok' "$D/out" && ok "REINSTALL file + marker in block 0: re-installed, backup precheck skipped" || { bad "reinstall rc=$rc"; shownt; }
rm -f "$D/stick/reinstall"
cp "$D/nand.inst" "$D/nand.img"; cp "$D/nand.inst" "$D/nand.orig"
run_it IT_FLAGS="olpcboy.install olpcboy.reinstall"; rc=$?
[ $rc = 0 ] && grep -q 're-install allowed' "$D/out" && ok "the olpcboy.reinstall command line flag works too" || { bad "reinstall cmdline rc=$rc"; shownt; }
# flag set but erase block 0 is somebody else's data: the flag does not apply, the precheck decides (and fails)
reset; flipblk 30; cp "$D/nand.img" "$D/nand.orig"
run_it IT_FLAGS="olpcboy.install olpcboy.reinstall"; expect_abort "REINSTALL on a foreign NAND (no marker) that differs from the backup: aborted" 'does not apply'
# interrupted install: erase block 0 erased, rest of the NAND garbage
cp "$D/nand.inst" "$D/nand.img"; "$tmp/bin/flash_erase" -q -j "$D/dev/mtd0" 0 0 >/dev/null 2>&1; cp "$D/nand.img" "$D/nand.orig"
run_it IT_FLAGS="olpcboy.install olpcboy.reinstall"; rc=$?
[ $rc = 0 ] && grep -q 'is erased (an interrupted install)' "$D/out" && ok "REINSTALL over a fully erased NAND (interrupted install): allowed" || { bad "interrupted reinstall rc=$rc"; shownt; }

# ---- restore --------------------------------------------------------------------------------------------
echo "-- restore"
# mkrestore: NANDCOPY.DAT of NB blocks (random) + SHA256SUMS on the fake backup stick
mkrestore() {   # mkrestore NB
	rm -rf "$D/bkstick"/*; mkdir -p "$D/bkstick/nandbackup-001"
	"$tmp/nbtool" gen 77 $(( $1 * E )) >"$D/bkstick/nandbackup-001/NANDCOPY.DAT"
	( cd "$D/bkstick/nandbackup-001" && sha256sum NANDCOPY.DAT >SHA256SUMS; echo "0000  MANIFEST.txt" >>SHA256SUMS )
	echo "console=tty0 mtdparts=cafe_nand:$(( PBLK * E / 1024 ))k(olpcboy),-(bbt)ro olpcboy.restore" >"$D/cmdline"
}
mkworld rs || exit 1
mkrestore 10
# start from an installed NAND (the restore must destroy the install)
run_it; [ $? = 0 ] || bad "restore world: install failed"; cp "$D/nand.img" "$D/nand.inst"
RR="STUB_NANDSTICK=1 IT_MODE=restore"
run_it $RR; rc=$?
if [ $rc = 0 ] && grep -q 'RESULT: OK' "$D/out" && grep -q 'RESTORED' "$D/tty"; then ok "restore: RESULT: OK, RESTORED screen"; else bad "restore happy path rc=$rc"; shownt; fi
only_mtd0 && bbt_untouched && ok "restore: only mtd0 written; bad block table blocks untouched" || bad "restore wrote outside mtd0"
"$tmp/bin/nanddump" -q --bb=skipbad -l $(( 10 * E )) "$D/dev/mtd0" 2>/dev/null | cmp -s - "$D/bkstick/nandbackup-001/NANDCOPY.DAT" && ok "restore: NANDCOPY.DAT sits in the first good blocks (bad block skipped)" || bad "restore image not on the NAND"
set -- $("$tmp/bin/nanddump" -q --bb=skipbad -s $(( 14 * E )) -l $(( 20 * E )) "$D/dev/mtd0" 2>/dev/null | "$tmp/nbtool" ff); [ "$1" = 0 ] && ok "restore: the rest of the partition is erased" || bad "restore tail not erased ($1)"
grep -q 'mount -t vfat -o ro,' "$D/mount.log" && [ "$(grep -c vfat "$D/mount.log")" = 1 ] && ok "restore: the backup stick was mounted read-only (once)" || bad "restore stick mount: $(cat "$D/mount.log")"
[ ! -s "$D/mounts" ] && ok "restore: the backup stick is unmounted at the end" || bad "backup stick still mounted"
[ -f "$D/boot/RESTORE.TXT" ] && ok "restore: log RESTORE.TXT written" || bad "no RESTORE.TXT"
grep -q 'flash_erase -j' "$FAKE_LOG" && ok "restore: erase with cleanmarkers (flash_erase -j)" || bad "restore erase without -j"
# aborts
cp "$D/nand.inst" "$D/nand.img"; cp "$D/nand.inst" "$D/nand.orig"
run_it IT_MODE=restore; expect_abort "restore: no backup stick: aborted after waiting, nothing written" 'backup stick (volume OLPCNAND) was not found'
run_it $RR STUB_BKRW=1; expect_abort "restore: a backup stick that mounts read-write is refused" 'did not mount read-only'
run_it $RR STUB_BKMOUNTFAIL=1; expect_abort "restore: a backup stick that does not mount: aborted" 'cannot mount the backup stick'
echo "/dev/sdb1 /x vfat ro 0 0" >"$D/mounts"; run_it $RR; expect_abort "restore: the backup stick already mounted elsewhere: aborted" 'already mounted'; : >"$D/mounts"
printf '\125' | dd of="$D/bkstick/nandbackup-001/NANDCOPY.DAT" bs=1 seek=100 conv=notrunc 2>/dev/null
run_it $RR; expect_abort "restore: NANDCOPY.DAT that does not match SHA256SUMS: aborted, nothing written" 'does not match SHA256SUMS'
mkrestore 10; rm -f "$D/bkstick/nandbackup-001/SHA256SUMS"; run_it $RR; expect_abort "restore: no SHA256SUMS: aborted" 'no SHA256SUMS'
mkrestore 10; grep -v NANDCOPY "$D/bkstick/nandbackup-001/SHA256SUMS" >"$D/s2"; cp "$D/s2" "$D/bkstick/nandbackup-001/SHA256SUMS"; run_it $RR; expect_abort "restore: SHA256SUMS without a NANDCOPY.DAT line: aborted" 'no line for NANDCOPY.DAT'
mkrestore 10; mkdir -p "$D/bkstick/nandbackup-002"; cp "$D/bkstick/nandbackup-001/"* "$D/bkstick/nandbackup-002/"; run_it $RR; expect_abort "restore: two backup sets on the stick: aborted" '2 NANDCOPY.DAT files'
mkrestore 10; printf 'xxxxx' >>"$D/bkstick/nandbackup-001/NANDCOPY.DAT"; run_it $RR; expect_abort "restore: NANDCOPY.DAT of a bad size: aborted" 'bad size'
mkrestore $(( PBLK - 1 )); run_it $RR; expect_abort "restore: NANDCOPY.DAT bigger than the good blocks: aborted" 'does not fit'
mkrestore 10; run_it $RR FAKE_FAIL_ERASE=1; rc=$?
[ $rc != 0 ] && grep -q 'FATAL: flash_erase failed' "$D/out" && [ "$(grep -c '^flash_erase\|^nandwrite' "$FAKE_LOG")" = 1 ] && ok "restore: a failing flash_erase stops the run" || { bad "restore erase fail rc=$rc"; shownt; }
cp "$D/nand.inst" "$D/nand.img"
run_it $RR FAKE_FAIL_WRITE=1; rc=$?
[ $rc != 0 ] && grep -q 'FATAL: nandwrite failed' "$D/out" && grep -q 'ERASED' "$D/out" && ok "restore: a failing nandwrite stops the run; the state says ERASED" || { bad "restore write fail rc=$rc"; shownt; }
cp "$D/nand.inst" "$D/nand.img"
run_it $RR FAKE_WRITE_BAD=1; rc=$?
[ $rc != 0 ] && grep -q 'RESTORE FAILED' "$D/tty" && grep -q 'FAIL  restore: read-back' "$D/out" && ok "restore: a corrupted read-back is reported as FAILED" || { bad "restore readback fault rc=$rc"; shownt; }
cp "$D/nand.inst" "$D/nand.img"
mkrestore 10; mv "$D/bkstick/nandbackup-001/NANDCOPY.DAT" "$D/bkstick/nandbackup-001/nandcopy.dat"; mv "$D/bkstick/nandbackup-001/SHA256SUMS" "$D/bkstick/nandbackup-001/sha256sums"
run_it $RR; rc=$?
[ $rc = 0 ] && grep -q 'RESULT: OK' "$D/out" && ok "restore: lower-case file names (nandcopy.dat, sha256sums) are accepted" || { bad "restore lower-case rc=$rc"; shownt; }
mkrestore 10; cp "$D/nand.inst" "$D/nand.img"
run_it $RR STUB_MOUNT_ANY=1; rc=$?
[ $rc = 0 ] && grep -q 'mounted read-only, /boot/olpc.fth present' "$D/out" && ok "restore: the mount sanity check of the restored NAND is reported" || { bad "restore mount check rc=$rc"; shownt; }
cp "$D/nand.inst" "$D/nand.img"
run_it IT_MODE=none; rc=$?
[ $rc != 0 ] && grep -q 'no mode' "$D/out" && ok "no mode on the command line: refused" || { bad "no mode rc=$rc"; shownt; }


# ---- restore drill, then install: the gate accepts the restored layout ---------------------------------------
echo "-- restore then install"
mkworld rd || exit 1
mkrestore 10
run_it $RR; [ $? = 0 ] || { bad "drill: restore failed"; shownt; }
cp "$D/nand.img" "$D/nand.rest"
echo "console=tty0 mtdparts=cafe_nand:$(( PBLK * E / 1024 ))k(olpcboy),-(bbt)ro olpcboy.install" >"$D/cmdline"
cp "$D/nand.rest" "$D/nand.orig"
run_it; rc=$?
[ $rc = 0 ] && grep -q 'precheck ok: the NAND equals the backup (restored layout)' "$D/out" && ok "restore then install: the gate passes on the restored layout and the install completes" || { bad "restored-layout gate rc=$rc"; shownt; }
# tampered block after a restore (2 bits in one ECC chunk, block 30: an erased block), and in a data block (block 2)
for tb in 30 2; do
	cp "$D/nand.rest" "$D/nand.img"; flipblk $tb; cp "$D/nand.img" "$D/nand.orig"
	run_it; expect_abort "restored layout with a tampered block ($tb): aborted, nothing written" "matches neither the backup .*restored layout"
done
# a grown bad block during the restore: the data shifts by one block, so the restored list no longer matches (fail safe)
cp "$D/nand.v0" "$D/nand.img"; cp "$D/nand.v0" "$D/nand.orig"
FAKE_BAD="$BAD 5" "$tmp/bin/flash_erase" -q -j "$D/dev/mtd0" 0 0 2>/dev/null
FAKE_BAD="$BAD 5" "$tmp/bin/nandwrite" -q -p "$D/dev/mtd0" "$D/bkstick/nandbackup-001/NANDCOPY.DAT" 2>/dev/null
cp "$D/nand.img" "$D/nand.orig"
run_it; expect_abort "a bad block that grew during the restore: the restored layout does not match, aborted (fail safe)" 'a bad block grew during the restore'

[ "$fail" = 0 ] && echo "install tests: PASS" || echo "install tests: FAILED"
exit "$fail"
