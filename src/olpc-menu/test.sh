#!/bin/sh
# olpc-menu host tests: unit checks (menulib), then the real program against a fake framebuffer with
# scripted key events: discovery on a temp tree, rendering (frames can be kept with
# FRAMES_DIR=dir: raw RGB565 1200x900), navigation, key repeat, chord flag, launching a stub
# olpc-run (success and failure), state file, idle CPU.  No hardware needed.
here=$(cd "$(dirname "$0")" && pwd)
CC=${CC:-gcc}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
bad() { echo "  FAIL: $*"; fail=1; }
ok()  { echo "  ok: $*"; }

HCF="-O2 -Wall -Wextra"
$CC $HCF -o "$tmp/selftest" "$here/selftest.c" "$here/menulib.c" || exit 1
$CC $HCF -o "$tmp/olpc-menu" "$here/olpc-menu.c" "$here/menulib.c" "$here/../olpc-run/input.c" "$here/../olpc-run/diag.c" 2>"$tmp/cc.log" \
	|| { cat "$tmp/cc.log"; exit 1; }
grep -E "olpc-menu\.c|menulib\.c" "$tmp/cc.log" | grep -i warning && bad "compiler warnings in olpc-menu"

"$tmp/selftest" || fail=1

# ---- fixture tree ----------------------------------------------------------------------------
T=$tmp/fs
R=$T/mnt/stick/roms
mkdir -p "$R/GB" "$R/GBC" "$R/nes" "$R/GBA" "$T/mnt/stick/covers/gb" "$T/mnt/stick/covers/gbc" "$T/run" "$tmp/noinput" "$tmp/nodev"
for n in "Super Mario Land (World) (Rev A)" "Kirby's Dream Land (USA, Europe)" "Tetris (World) (Rev 1)" \
	 "Pok\303\251mon - Edici\303\263n Roja (Spain)" "Alleyway (World)" "Balloon Kid (World)" "Metroid II - Return of Samus (World)" \
	 "Mega Man V (USA)" "Wario Land - Super Mario Land 3 (World)" "Final Fantasy Legend, The (USA)" "Dr. Mario (World) (Rev 1)" \
	 "Game & Watch Gallery (USA)" "Tennis (World)" "Golf (World)" "Baseball (Japan)" "Boxxle (USA)" "Castlevania - The Adventure (USA)" \
	 "Donkey Kong Land (USA)" "F-1 Race (World)" "Gargoyle's Quest (USA)" "Heiankyo Alien (USA)" "Mario's Picross (USA)" \
	 "Ninja Gaiden Shadow (USA)" "Operation C (USA)" "Probotector (Europe)" "QBert (USA)" "Radar Mission (USA)" "Solar Striker (World)" \
	 "Trip World (Japan)" "Ultima - Runes of Virtue (USA)"; do
	printf 'x' >"$R/GB/$n.gb"
done
printf 'x' >"$R/GB/zip game.zip"; printf 'x' >"$R/GB/readme.txt"
printf 'x' >"$R/GBC/Hamtaro - Ham-Hams Unite! (Europe) (En,Fr,De,Es,It).zip"
printf 'x' >"$R/GBC/Zelda - Link's Awakening DX (USA).gbc"
printf 'x' >"$R/nes/Super Mario Bros. 3 (USA).zip"; printf 'x' >"$R/nes/Contra (USA).nes"
printf 'x' >"$R/GBA/ignored.gba"
"$tmp/selftest" --mkcover "$T/mnt/stick/covers/gb/Alleyway (World).obc" 320 288
"$tmp/selftest" --mkcover "$T/mnt/stick/covers/gbc/Hamtaro - Ham-Hams Unite! (Europe) (En,Fr,De,Es,It).obc" 480 480

cat >"$tmp/run-ok" <<EOS
#!/bin/sh
echo "\$@" >"$T/run.args"
exit 0
EOS
cat >"$tmp/run-fail" <<EOS
#!/bin/sh
echo "\$@" >>"$T/run.args"
echo "olpc-run: core: cannot load libretro core gambatte_libretro.so: No such file" >&2
echo "olpc-run: this is a long second line, long enough to need wrapping across the screen width: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" >&2
echo "olpc-run: last line, rc 3" >&2
exit 3
EOS
chmod +x "$tmp/run-ok" "$tmp/run-fail"

# run_menu <script> [env...]: one run against the fixture, fresh state
run_menu() {
	sc=$1; shift
	env OLPC_MENU_FAKEFB="$tmp/fb.raw" OLPC_MENU_SCAN="$T/mnt" OLPC_MENU_RUNDIR="$T/run" OLPC_MENU_INPUTDIR="$tmp/noinput" \
		OLPC_MENU_DEVDIR="$tmp/nodev" OLPC_MENU_RUN="$tmp/run-ok" OLPC_MENU_COVERS_BIN="$tmp/none" OLPC_MENU_WIFI_BIN="$tmp/wifi-stub" \
		OLPC_MENU_POWEROFF="$tmp/poweroff-stub" OLPC_MENU_REBOOT="$tmp/poweroff-stub" "$@" \
		"$tmp/olpc-menu" --script "$sc" 2>"$tmp/menu.err"
}
cat >"$tmp/wifi-stub" <<'EOS'
#!/bin/sh
echo "not connected (wpa_state=none)"; echo "ssid:   "; echo "ip:     none"
EOS
cat >"$tmp/poweroff-stub" <<EOS
#!/bin/sh
echo poweroff >>"$T/power.log"
EOS
chmod +x "$tmp/wifi-stub" "$tmp/poweroff-stub"
D=$tmp/dump
mkdir -p "$D"
keep() { [ -n "$FRAMES_DIR" ] && { mkdir -p "$FRAMES_DIR"; cp "$D"/*.raw "$FRAMES_DIR"/ 2>/dev/null; }; }

echo "== program: list, covers, placeholder, system switch, options, empty state"
rm -f "$T/run/menu.state"
cat >"$tmp/s1" <<EOS
300 dump $D/01-list-first.raw
350 key DOWN tap
900 dump $D/02-list-second.raw
1000 key DOWN tap
1100 key DOWN tap
1200 key DOWN tap
1700 dump $D/03-list-cover.raw
1800 key RIGHT tap
2300 dump $D/04-gbc-long.raw
2400 key RIGHT tap
2900 dump $D/05-nes.raw
3000 key START tap
3200 dump $D/06-options.raw
3300 key DOWN tap
3500 key A tap
3700 dump $D/07-options-covers-missing.raw
3800 key B tap
4300 dump $D/08-back.raw
4400 end
EOS
run_menu "$tmp/s1"; rc=$?
[ $rc = 0 ] || { bad "script run rc=$rc"; cat "$tmp/menu.err"; }
grep -q "30 games\|32 games\|games (" "$tmp/menu.err" && sed -n 1p "$tmp/menu.err" | sed 's/^/  /'
for f in 01-list-first 02-list-second 03-list-cover 04-gbc-long 05-nes 06-options 07-options-covers-missing 08-back; do
	[ "$(stat -c %s "$D/$f.raw" 2>/dev/null)" = 2160000 ] || bad "frame $f missing or wrong size"
done
cmp -s "$D/01-list-first.raw" "$D/02-list-second.raw" && bad "moving the selection did not change the frame"
cmp -s "$D/03-list-cover.raw" "$D/02-list-second.raw" && bad "cover frame equals placeholder frame"
cmp -s "$tmp/fb.raw" "$D/08-back.raw" 2>/dev/null; # fake fb after exit = last flushed frame
cmp -s "$D/08-back.raw" "$tmp/fb.raw" || bad "fake framebuffer differs from the back buffer (flush bug)"
cmp -s "$D/06-options.raw" "$D/08-back.raw" && bad "options menu not drawn / not closed"
[ -f "$T/run/menu.state" ] && ok "state file written on exit: $(tr '\n' ' ' <"$T/run/menu.state")"
keep

# empty state
E=$tmp/empty; mkdir -p "$E/mnt/sd/roms/gb" "$E/run"
cat >"$tmp/s2" <<EOS
300 dump $D/09-empty.raw
350 key A tap
400 key LEFT tap
450 key RIGHT tap
500 key DOWN tap
900 dump $D/09-empty2.raw
EOS
run_menu "$tmp/s2" OLPC_MENU_SCAN="$E/mnt" OLPC_MENU_RUNDIR="$E/run"; rc=$?
[ $rc = 0 ] || bad "empty-state run rc=$rc"
cmp -s "$D/09-empty.raw" "$D/09-empty2.raw" && ok "empty state ignores A/LEFT/RIGHT/DOWN" || bad "empty state changed on input"
keep

echo "== chord flag: D-pad and buttons ignored while the chord is held"
rm -f "$T/run/menu.state" "$T/run.args" "$T/run/chord"
cat >"$tmp/s3" <<EOS
200 dump $D/10-a.raw
250 chord 1
300 key DOWN tap
400 key DOWN tap
500 key A tap
700 dump $D/10-b.raw
750 chord 0
900 key DOWN tap
1300 dump $D/10-c.raw
EOS
run_menu "$tmp/s3" OLPC_MENU_CHORD="$T/run/chord"; rc=$?
[ $rc = 0 ] || bad "chord run rc=$rc"
cmp -s "$D/10-a.raw" "$D/10-b.raw" && ok "no movement while the chord flag is 1" || bad "moved while the chord flag was 1"
cmp -s "$D/10-b.raw" "$D/10-c.raw" && bad "no movement after the chord ended"
[ -f "$T/run.args" ] && bad "launched while the chord flag was 1"
# two buttons together (the real chords) must not fire either; a single one does
rm -f "$T/run.args"
cat >"$tmp/s3b" <<EOS
200 key START down
230 key A down
400 key A up
430 key START up
700 dump $D/11-a.raw
EOS
run_menu "$tmp/s3b"; [ -f "$T/run.args" ] && bad "A+START launched / opened options"
cat >"$tmp/s3c" <<EOS
200 key A down
600 key A up
900 dump $D/11-b.raw
EOS
run_menu "$tmp/s3c"; [ -f "$T/run.args" ] && ok "single A press launches: $(cat "$T/run.args")" || bad "single A press did not launch"

echo "== key repeat in the program and the selection state"
rm -f "$T/run/menu.state"
cat >"$tmp/s4" <<EOS
100 key DOWN down
1100 key DOWN up
1300 end
EOS
run_menu "$tmp/s4"
gbidx() { ls "$R/GB" | grep -v -e readme | sed 's/\.[^.]*$//' | LC_ALL=C sort -f | grep -n -F -x -- "${1%.*}" | cut -d: -f1; }
f=$(sed -n 's/^file=//p' "$T/run/menu.state"); idx=$(gbidx "$f")
echo "  held DOWN for 1 s: selected \"$f\" (entry $idx of the sorted GB list, model: 11, tolerance 9..12 for scheduling jitter)"
[ -n "$idx" ] && [ "$idx" -ge 9 ] && [ "$idx" -le 12 ] || bad "repeat landed on entry ${idx:-?}"
cat >"$tmp/s4b" <<EOS
100 key DOWN down
2700 key DOWN up
2900 end
EOS
rm -f "$T/run/menu.state"; run_menu "$tmp/s4b"
f=$(sed -n 's/^file=//p' "$T/run/menu.state"); idx=$(gbidx "$f")
echo "  held DOWN for 2.6 s: entry $idx (page steps after 1.5 s; list has 31 entries)"
[ "$idx" = 31 ] || [ "${idx:-0}" -ge 28 ] || bad "page acceleration did not reach the end (entry ${idx:-?})"

echo "== launch a stub olpc-run, return, held keys do not act"
rm -f "$T/run/menu.state" "$T/run.args"
cat >"$tmp/s5" <<EOS
100 key DOWN tap
300 key RIGHT tap
500 key LEFT tap
700 key DOWN down
800 key A tap
1500 dump $D/12-after-launch.raw
2700 key DOWN up
2900 end
EOS
run_menu "$tmp/s5"; rc=$?
[ $rc = 0 ] || bad "launch run rc=$rc"
args=$(cat "$T/run.args" 2>/dev/null)
echo "  stub received: $args"
case "$args" in "gb $R/GB/"*.gb|"gb $R/GB/"*.zip) ok "olpc-run <system> <rom path>";; *) bad "wrong olpc-run arguments";; esac
f=$(sed -n 's/^file=//p' "$T/run/menu.state")
case "$args" in *"/$f") ok "selection after return unchanged while DOWN stayed held ($f)";; *) bad "key held across the launch acted: state=$f args=$args";; esac
[ -s "$R/../olpc-menu.state" ] && ok "best-effort state copy next to the roms dir" || bad "no state copy next to the roms dir"
cmp -s "$D/12-after-launch.raw" "$D/12-after-launch.raw" # file exists
echo "== olpc-run failure: error screen"
rm -f "$T/run.args"
cat >"$tmp/s6" <<EOS
100 key A tap
800 dump $D/13-error.raw
900 key B tap
1300 dump $D/14-dismissed.raw
1400 end
EOS
run_menu "$tmp/s6" OLPC_MENU_RUN="$tmp/run-fail"; rc=$?
[ $rc = 0 ] || bad "error run rc=$rc"
cmp -s "$D/13-error.raw" "$D/14-dismissed.raw" && bad "error screen not dismissed by a button"
grep -q "rc 3" "$T/run/menu-run.err" && ok "stderr of olpc-run captured in menu-run.err"
keep

echo "== options: power off needs a second A"
rm -f "$T/power.log"
cat >"$tmp/s8" <<EOS
100 key START tap
200 key DOWN tap
300 key DOWN tap
400 key DOWN tap
500 key A tap
700 dump $D/15-confirm.raw
800 key B tap
900 end
EOS
run_menu "$tmp/s8"
[ -f "$T/power.log" ] && bad "power off ran on the first A" || ok "first A only asks for confirmation"
sed -i 's/^800 key B tap/800 key A tap/' "$tmp/s8"
run_menu "$tmp/s8"
grep -q poweroff "$T/power.log" 2>/dev/null && ok "second A runs poweroff" || bad "second A did not run poweroff"
keep

echo "== idle: no CPU while nothing changes"
cat >"$tmp/s7" <<EOS
100 key DOWN tap
3300 end
EOS
OLPC_MENU_FAKEFB="$tmp/fb.raw" OLPC_MENU_SCAN="$T/mnt" OLPC_MENU_RUNDIR="$T/run" OLPC_MENU_INPUTDIR="$tmp/noinput" OLPC_MENU_DEVDIR="$tmp/nodev" \
	"$tmp/olpc-menu" --script "$tmp/s7" 2>/dev/null &
pid=$!
sleep 2.5
t1=$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null); sleep 0.6; t2=$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null)
wait $pid
echo "  cpu ticks at 2.5 s: ${t1:-?}, 0.6 s later: ${t2:-?}"
[ -n "$t1" ] && [ "$t2" = "$t1" ] && ok "idle process used no CPU in 0.6 s" || bad "idle CPU not zero"

echo "== no framebuffer: clean exit code 3"
env -u OLPC_MENU_FAKEFB OLPC_MENU_SCAN="$T/mnt" OLPC_MENU_RUNDIR="$T/run" "$tmp/olpc-menu" 2>"$tmp/nofb.err"
[ $? = 3 ] && ok "exit 3 without the Geode framebuffer: $(head -n1 "$tmp/nofb.err")" || bad "no-fb exit code"

echo "== olpc-sdmount (stubbed blkid/mount)"
SD=$here/../../rootfs/overlay/usr/local/sbin/olpc-sdmount
mkdir -p "$tmp/sd"
printf 'tl() { echo "$*" >>"%s/sd.log"; }\n' "$tmp" >"$tmp/common-stub.sh"
printf '#!/bin/sh\necho "$1: UUID=\\"x\\" TYPE=\\"%s\\""\n' '${FSTYPE:-vfat}' >"$tmp/blkid-stub"
printf '#!/bin/sh\necho "$@" >>"%s/mount.log"\n[ -n "$MOUNT_FAIL_UTF8" ] && case "$*" in *utf8*) exit 1;; esac\nexit 0\n' "$tmp" >"$tmp/mount-stub"
printf '#!/bin/sh\necho "umount $@" >>"%s/mount.log"\n' "$tmp" >"$tmp/umount-stub"
chmod +x "$tmp/blkid-stub" "$tmp/mount-stub" "$tmp/umount-stub"
: >"$tmp/mounts-empty"
sdrun() { env OLPC_COMMON="$tmp/common-stub.sh" SD_DEV="$1" SD_MNT="$tmp/sd" SD_BLKID="$tmp/blkid-stub" SD_MOUNT="$tmp/mount-stub" \
	SD_UMOUNT="$tmp/umount-stub" SD_MOUNTS="$2" sh "$SD"; }
rm -f "$tmp/mount.log"
sdrun /dev/does-not-exist "$tmp/mounts-empty"; [ ! -f "$tmp/mount.log" ] && ok "no card: nothing mounted, exit 0" || bad "mounted without a card"
: >"$tmp/blockdev"; # a regular file is not -b: emulate presence with a block device node when possible
BD=$(ls /dev/loop0 /dev/loop1 /dev/nvme0n1p1 /dev/ram0 2>/dev/null | head -n1)
if [ -n "$BD" ]; then
	sdrun "$BD" "$tmp/mounts-empty"; grep -q "vfat.*iocharset=utf8.*$BD $tmp/sd" "$tmp/mount.log" 2>/dev/null && ok "vfat mounted rw,noatime,utf8 at the mount point" || bad "vfat mount line: $(cat "$tmp/mount.log" 2>/dev/null)"
	grep -q "noatime" "$tmp/mount.log" || bad "noatime missing"
	rm -f "$tmp/mount.log"; MOUNT_FAIL_UTF8=1 sdrun "$BD" "$tmp/mounts-empty"; [ "$(wc -l <"$tmp/mount.log")" = 2 ] && ok "falls back without iocharset" || bad "no iocharset fallback"
	echo "x $tmp/sd vfat rw 0 0" >"$tmp/mounts-full"; rm -f "$tmp/mount.log"
	sdrun "$BD" "$tmp/mounts-full"; [ ! -f "$tmp/mount.log" ] && ok "already mounted: untouched" || bad "remounted"
else
	echo "  (no block device node available for the present-card cases: skipped)"
fi
echo "x $tmp/sd vfat rw 0 0" >"$tmp/mounts-full"; rm -f "$tmp/mount.log"
sdrun /dev/does-not-exist "$tmp/mounts-full"; grep -q "umount -l $tmp/sd" "$tmp/mount.log" && ok "card removed: stale mount released" || bad "stale mount kept"
grep -q '/dev/mtd' "$SD" && bad "olpc-sdmount mentions an MTD node"

if [ $fail = 0 ]; then echo "olpc-menu tests: ALL OK"; else echo "olpc-menu tests: FAILED"; fi
exit $fail
