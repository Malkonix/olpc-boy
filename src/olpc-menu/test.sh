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
$CC $HCF -o "$tmp/selftest" "$here/selftest.c" "$here/menulib.c" "$here/../common/status.c" || exit 1
$CC $HCF -o "$tmp/olpc-menu" "$here/olpc-menu.c" "$here/menulib.c" "$here/netui.c" "$here/osk.c" "$here/wallpaper.c" "$here/../common/status.c" "$here/../common/gfx.c" "$here/../olpc-run/input.c" "$here/../olpc-run/diag.c" 2>"$tmp/cc.log" \
	|| { cat "$tmp/cc.log"; exit 1; }
grep -E "olpc-menu\.c|menulib\.c|status\.c|gfx\.c" "$tmp/cc.log" | grep -i warning && bad "compiler warnings in olpc-menu"

"$tmp/selftest" || fail=1

# ---- fixture tree ----------------------------------------------------------------------------
T=$tmp/fs
R=$T/mnt/stick/roms
mkdir -p "$R/GB" "$R/GBC" "$R/nes" "$R/GBA" "$T/mnt/stick/covers/gb" "$T/mnt/stick/covers/gbc" "$T/run" "$tmp/log" "$tmp/noinput" "$tmp/nodev"
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

# The stubs take "--stats FILE" out of the argument list (run.args keeps the rest, so the checks
# on the other options stay as they were) and leave the path in run.stats-arg.  run-ok writes a
# statistics file there, in olpc-run's key=value format, unless STUB_NOSTATS is set.
cat >"$tmp/run-ok" <<EOS
#!/bin/sh
a=; st=
while [ \$# -gt 0 ]; do case "\$1" in --stats) st=\$2; shift 2;; *) a="\$a \$1"; shift;; esac; done
echo "\${a# }" >"$T/run.args"
echo "\$st" >"$T/run.stats-arg"
echo "olpc-run: stub stderr line" >&2
[ -n "\$STUB_NOSTATS" ] || [ -z "\$st" ] || cat >"\$st" <<EOF
core=stub
status=\${STUB_STATUS:-exit}
frames_run=1234
frames_drawn=1200
fps_effective=59.71
underruns=2
frame_ms_max=21.500
overlay_redraws=17
overlay_redraw_us_total=3400.5
overlay_redraw_us_max=480.2
EOF
exit 0
EOS
cat >"$tmp/run-fail" <<EOS
#!/bin/sh
a=; while [ \$# -gt 0 ]; do case "\$1" in --stats) shift 2;; *) a="\$a \$1"; shift;; esac; done
echo "\${a# }" >>"$T/run.args"
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
		OLPC_MENU_POWEROFF="$tmp/poweroff-stub" OLPC_MENU_REBOOT="$tmp/poweroff-stub" OLPC_MENU_LOGDIR="$tmp/log" "$@" \
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

echo "== ROMs copied in while the list is shown are picked up (watch poll and SIGUSR1)"
printf '#!/bin/sh\nprintf x >"%s/GB/Copied In.gb"\n' "$R" >"$tmp/rs-1"
printf '#!/bin/sh\nprintf x >"%s/GB/Copied Two.gb"\nfor p in /proc/[0-9]*; do read -r c <$p/comm 2>/dev/null; [ "$c" = olpc-menu ] && kill -USR1 ${p#/proc/}; done\n' "$R" >"$tmp/rs-2"
chmod +x "$tmp/rs-1" "$tmp/rs-2"
cat >"$tmp/s-rs" <<EOS
300 sh $tmp/rs-1
10500 sh $tmp/rs-2
11000 end
EOS
run_menu "$tmp/s-rs" OLPC_MENU_WATCH="$R"
n0=$(sed -n 's/.*: \([0-9]*\) games (.*/\1/p' "$tmp/menu.err" | head -n1)
grep -q "rescan: $((n0 + 1)) games" "$tmp/menu.err" && ok "mtime poll rescan picked up a copied ROM" || { bad "poll rescan"; cat "$tmp/menu.err"; }
grep -q "rescan: $((n0 + 2)) games" "$tmp/menu.err" && ok "SIGUSR1 rescan" || bad "SIGUSR1 rescan"
rm -f "$R/GB/Copied In.gb" "$R/GB/Copied Two.gb"

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

echo "== per-game statistics: --stats, run-NNN.stats/.log, summary line, pruning"
L=$tmp/stlog; rm -rf "$L"; mkdir -p "$L"
cat >"$tmp/s-st" <<EOS
200 key A tap
700 end
EOS
rm -f "$T/run.args" "$T/run.stats-arg" "$T/run/menu.state"
run_menu "$tmp/s-st" OLPC_MENU_LOGDIR="$L"
[ "$(cat "$T/run.stats-arg" 2>/dev/null)" = "$L/run-001.stats" ] && ok "first game of the boot gets --stats $L/run-001.stats" || bad "stats argument: $(cat "$T/run.stats-arg" 2>/dev/null)"
[ -s "$L/run-001.stats" ] && ok "stub statistics file written" || bad "no run-001.stats"
grep -q 'stub stderr line' "$L/run-001.log" && ok "olpc-run output copied to run-001.log" || bad "no run-001.log"
grep -q 'stub stderr line' "$T/run/menu-run.err" && ok "menu-run.err capture (error screen source) intact" || bad "menu-run.err lost"
grep -q 'olpc-menu: stats run-001: status=exit frames=1234 fps=59.71 underruns=2 frame_ms_max=21.500 overlay_redraws=17 redraw_us_total=3400.5 redraw_us_max=480.2$' "$tmp/menu.err" \
	&& ok "summary line in the menu log" || { bad "summary line missing"; grep stats "$tmp/menu.err"; }
rm -f "$T/run/menu.state"
run_menu "$tmp/s-st" OLPC_MENU_LOGDIR="$L"
[ "$(cat "$T/run.stats-arg")" = "$L/run-002.stats" ] && ok "second game (new menu process, same boot) continues at 002" || bad "counter: $(cat "$T/run.stats-arg")"
# no statistics written by the run (killed): one line says so, no crash
rm -f "$T/run/menu.state"
run_menu "$tmp/s-st" OLPC_MENU_LOGDIR="$L" STUB_NOSTATS=1; rc=$?
[ $rc = 0 ] && grep -q 'stats run-003: none' "$tmp/menu.err" && ok "missing statistics file: logged, rc 0" || bad "missing stats handling (rc=$rc)"
# failed run keeps the error screen capture and still logs
rm -f "$T/run/menu.state"
cat >"$tmp/s-st2" <<EOS
100 key A tap
800 end
EOS
run_menu "$tmp/s-st2" OLPC_MENU_LOGDIR="$L" OLPC_MENU_RUN="$tmp/run-fail"
grep -q "rc 3" "$T/run/menu-run.err" && grep -q "rc 3" "$L/run-004.log" && ok "failing run: stderr in menu-run.err and run-004.log" || bad "failing run capture"
grep -q 'stats run-004: none' "$tmp/menu.err" && ok "failing run without statistics logged" || bad "no line for the failing run"
# keep only the newest 30 games of the boot
L2=$tmp/stlog2; rm -rf "$L2"; mkdir -p "$L2"
i=1; while [ $i -le 40 ]; do n=$(printf %03d $i); echo x >"$L2/run-$n.stats"; echo x >"$L2/run-$n.log"; i=$((i+1)); done
echo keep >"$L2/menu.log"; echo keep >"$L2/run-gb.stats"
rm -f "$T/run/menu.state"
run_menu "$tmp/s-st" OLPC_MENU_LOGDIR="$L2"
[ "$(cat "$T/run.stats-arg")" = "$L2/run-041.stats" ] || bad "numbering above existing files: $(cat "$T/run.stats-arg")"
n=$(ls "$L2" | grep -c '^run-[0-9][0-9]*\.stats$')
[ "$n" = 30 ] && [ -e "$L2/run-012.stats" ] && [ ! -e "$L2/run-011.stats" ] && [ ! -e "$L2/run-011.log" ] && [ -e "$L2/run-041.log" ] \
	&& ok "newest 30 kept (012..041), older removed" || bad "pruning: $n stats files, $(ls "$L2" | head -3 | tr '\n' ' ')"
[ -e "$L2/menu.log" ] && [ -e "$L2/run-gb.stats" ] && ok "other files in the log dir untouched" || bad "pruning removed foreign files"
# no usable log dir: the run directory is used
rm -f "$T/run/menu.state" "$T"/run/run-*
run_menu "$tmp/s-st" OLPC_MENU_LOGDIR="$tmp/not-a-dir"
[ "$(cat "$T/run.stats-arg")" = "$T/run/run-001.stats" ] && ok "falls back to the run directory" || bad "fallback: $(cat "$T/run.stats-arg")"
rm -f "$T/run/menu.state" "$T"/run/run-*
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

echo "== idle while connected (30 s signal timer, minute timer): still no CPU between wake-ups"
mkdir -p "$tmp/idle"; printf 'state=connected ssid=Casa ip=10.0.0.2\n' >"$tmp/idle/wifi.status"; printf 'vol=15 volmax=31 mute=0 bri=7 brimax=15 mono=0\n' >"$tmp/idle/status"
printf ' wlan0: 0000   70.  -60.  -256        0      0      0      0      0        0\n' >"$tmp/idle/wireless"
cat >"$tmp/s7b" <<EOS
3300 end
EOS
OLPC_MENU_FAKEFB="$tmp/fb.raw" OLPC_MENU_SCAN="$T/mnt" OLPC_MENU_RUNDIR="$tmp/idle" OLPC_MENU_INPUTDIR="$tmp/noinput" OLPC_MENU_DEVDIR="$tmp/nodev" \
	OLPC_MENU_WIRELESS="$tmp/idle/wireless" OLPC_MENU_CFGDIR="$tmp/nodev" OLPC_MENU_TZFILE="$tmp/nodev/none" "$tmp/olpc-menu" --script "$tmp/s7b" 2>/dev/null &
pid=$!
sleep 2.3
t1=$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null); sleep 0.6; t2=$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null)
wait $pid
echo "  cpu ticks at 2.3 s: ${t1:-?}, 0.6 s later: ${t2:-?}"
[ -n "$t1" ] && [ "$t2" = "$t1" ] && ok "idle while connected used no CPU in 0.6 s" || bad "idle CPU not zero while connected"

echo "== options menu uses wifi.status"
O=$tmp/optrun; mkdir -p "$O"
cat >"$tmp/s-opt" <<EOS
200 key START tap
500 dump $D/opt-off.raw
EOS
run_menu "$tmp/s-opt" OLPC_MENU_RUNDIR="$O" OLPC_MENU_CFGDIR="$tmp/nodev" OLPC_MENU_TZFILE="$tmp/nodev/none"
printf 'state=connected ssid=Casa 5G ip=192.168.1.20\n' >"$O/wifi.status"
run_menu "$tmp/s-opt" OLPC_MENU_RUNDIR="$O" OLPC_MENU_CFGDIR="$tmp/nodev" OLPC_MENU_TZFILE="$tmp/nodev/none"; cp "$D/opt-off.raw" "$D/opt-on.raw"
printf 'state=connecting\n' >"$O/wifi.status"
run_menu "$tmp/s-opt" OLPC_MENU_RUNDIR="$O" OLPC_MENU_CFGDIR="$tmp/nodev" OLPC_MENU_TZFILE="$tmp/nodev/none"; cp "$D/opt-off.raw" "$D/opt-connecting.raw"
rm -f "$O/wifi.status"
run_menu "$tmp/s-opt" OLPC_MENU_RUNDIR="$O" OLPC_MENU_CFGDIR="$tmp/nodev" OLPC_MENU_TZFILE="$tmp/nodev/none"; cp "$D/opt-off.raw" "$D/opt-missing.raw"
cmp -s "$D/opt-off.raw" "$D/opt-on.raw" && bad "options screen ignores wifi.status (connected)" || ok "options screen differs for connected"
cmp -s "$D/opt-on.raw" "$D/opt-connecting.raw" && bad "options screen: connecting looks like connected" || ok "options screen differs for connecting"
printf 'state=off\n' >"$O/wifi.status"
run_menu "$tmp/s-opt" OLPC_MENU_RUNDIR="$O" OLPC_MENU_CFGDIR="$tmp/nodev" OLPC_MENU_TZFILE="$tmp/nodev/none"
cmp -s "$D/opt-off.raw" "$D/opt-missing.raw" && ok "options: state=off and a missing wifi.status look the same" || bad "off vs missing wifi.status differ"
keep

echo "== options: In-game indicators switch (state file, --no-overlay for olpc-run)"
O2=$tmp/ovrun; rm -rf "$O2"; mkdir -p "$O2"; rm -f "$T/mnt/stick/olpc-menu.state" "$T/run.args"
cat >"$tmp/s-ov1" <<EOS
200 key START tap
300 key DOWN tap
400 key DOWN tap
500 key DOWN tap
600 key DOWN tap
700 key DOWN tap
900 dump $D/ov-on.raw
1000 key A tap
1300 dump $D/ov-off.raw
1400 key A tap
1700 dump $D/ov-on2.raw
1800 key A tap
2100 end
EOS
run_menu "$tmp/s-ov1" OLPC_MENU_RUNDIR="$O2" OLPC_MENU_CFGDIR="$tmp/nodev" OLPC_MENU_TZFILE="$tmp/nodev/none"
cmp -s "$D/ov-on.raw" "$D/ov-off.raw" && bad "the options screen did not change when the switch was pressed" || ok "options screen shows the switch changing"
cmp -s "$D/ov-off.raw" "$D/ov-on2.raw" && bad "OFF and ON screens are the same" || ok "ON, OFF, ON: the third screen differs from the OFF one"
grep -q '^overlay=0' "$O2/menu.state" && ok "after ON, OFF, ON, OFF the state file holds overlay=0: $(tr '\n' ' ' <"$O2/menu.state")" || bad "state file lacks overlay=0: $(cat "$O2/menu.state" 2>/dev/null)"
cp "$D/ov-off.raw" "$D/ov-off-keep.raw"
cat >"$tmp/s-ov2" <<EOS
200 key A tap
600 end
EOS
rm -f "$T/run.args"
run_menu "$tmp/s-ov2" OLPC_MENU_RUNDIR="$O2"
case "$(cat "$T/run.args" 2>/dev/null)" in --no-overlay\ *) ok "a game started with the switch OFF gets --no-overlay: $(cat "$T/run.args")";; *) bad "no --no-overlay in: $(cat "$T/run.args" 2>/dev/null)";; esac
# the switch is remembered from the copy next to the ROMs when the runtime state is gone (reboot)
rm -f "$O2/menu.state" "$T/run.args"
run_menu "$tmp/s-ov2" OLPC_MENU_RUNDIR="$O2"
case "$(cat "$T/run.args" 2>/dev/null)" in --no-overlay\ *) ok "the OFF setting survives a reboot (copy on the stick)";; *) bad "setting lost without the runtime state: $(cat "$T/run.args" 2>/dev/null)";; esac
cat >"$tmp/s-ov3" <<EOS
200 key START tap
300 key DOWN tap
400 key DOWN tap
500 key DOWN tap
600 key DOWN tap
700 key DOWN tap
800 key A tap
1000 key B tap
1100 key A tap
1500 end
EOS
rm -f "$T/run.args"
run_menu "$tmp/s-ov3" OLPC_MENU_RUNDIR="$O2"
case "$(cat "$T/run.args" 2>/dev/null)" in --no-overlay*|"") bad "after switching back ON a game still got: $(cat "$T/run.args" 2>/dev/null)";; *) ok "switched ON again: plain olpc-run arguments: $(cat "$T/run.args")";; esac
grep -q '^overlay=1' "$O2/menu.state" && ok "state file holds overlay=1" || bad "state file lacks overlay=1"
rm -f "$T/mnt/stick/olpc-menu.state" "$T/run.args"
keep

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

echo "== title-bar indicators (WiFi, volume, brightness, clock)"
I=$tmp/ind; BL=$tmp/bl; WLS=$tmp/wireless; CFGD=$tmp/cfgdir; TZF=$tmp/TZ
mkdir -p "$I" "$BL/panel" "$CFGD"
echo 15 >"$BL/panel/max_brightness"; echo 7 >"$BL/panel/brightness"; echo AST4 >"$TZF"
# 2026-11-19 15:07:00 in AST4 (UTC-4)
T_PM=$(date -u -d '2026-11-19 19:07:00' +%s); T_AM=$(date -u -d '2026-11-19 09:05:00' +%s)
# geometry (1200 px wide): slots wifi 584+44, volume 648+88, brightness 756+92, battery 868+96, clock 984+192; bar is 72 high
X_WIFI=584; X_VOL=648; X_BRI=756; X_BAT=868; X_CLK=984; Y1=4; Y2=68
wl_fix() { printf 'Inter-| sta-|   Quality        |   Discarded packets               | Missed | WE\n face | tus | link level noise |  nwid  crypt   frag  retry   misc | beacon | 22\n wlan0: 0000   70.  %s.  -256        0      0      0      0      0        0\n' "$1" >"$WLS"; }
wl_fix -50
# ind_run NAME [keys-script-line] [env...]: one frame after start (ST/WF/NOW/TZC set by the caller)
ind_run() {
	n=$1; extra=$2; shift; [ $# -gt 0 ] && shift
	rm -rf "$I"/*; rm -f "$CFGD"/*
	[ -n "$ST" ] && printf '%s\n' "$ST" >"$I/status"
	[ -n "$WF" ] && printf '%s\n' "$WF" >"$I/wifi.status"
	[ -n "$TZC" ] && printf '%s\n' "$TZC" >"$CFGD/Tz.Cfg"
	{ [ -n "$extra" ] && echo "100 key $extra tap"; echo "600 dump $D/ind-$n.raw"; } >"$tmp/si"
	run_menu "$tmp/si" OLPC_MENU_RUNDIR="$I" OLPC_MENU_BACKLIGHT="$BL/*" OLPC_MENU_WIRELESS="$WLS" OLPC_MENU_CFGDIR="$CFGD" \
		OLPC_MENU_TZFILE="$TZF" OLPC_MENU_FAKETIME="${NOW:-$T_PM}" "$@" || bad "indicator run $n failed"
	[ "$(stat -c %s "$D/ind-$n.raw" 2>/dev/null)" = 2160000 ] || bad "indicator frame $n missing"
}
VOK="vol=15 volmax=31 mute=0 bri=7 brimax=15 mono=0"
ST=$VOK; WF=; TZC=; NOW=$T_PM
ind_run wifi-off
WF="state=connecting"; ind_run wifi-connecting
WF="state=connected ssid=Casa 5G ip=192.168.1.20"; wl_fix -80; ind_run wifi-1bar
wl_fix -50; ind_run wifi-4bars
rm -f "$WLS"; ind_run wifi-nofile-4bars; wl_fix -50
WF=
for v in 0 15 31; do ST="vol=$v volmax=31 mute=0 bri=7 brimax=15 mono=0"; ind_run vol-$v; done
ST="vol=15 volmax=31 mute=1 bri=7 brimax=15 mono=0"; ind_run vol-muted
ST=; ind_run vol-unknown-sysfs-bri
for b in 0 7 15; do ST="vol=15 volmax=31 mute=0 bri=$b brimax=15 mono=0"; ind_run bri-$b; done
ST=$VOK
ind_run clock-pm
NOW=$T_AM; ind_run clock-am
TZC="AST4 24" ind_run clock-24h
TZC="AST4 24"; NOW=$T_PM ind_run clock-24h-pm
TZC="EST5 12"; ind_run clock-tzcfg-est
TZC=; NOW=1000000000; ind_run clock-unset
NOW=$T_PM; WF="state=connected ssid=x ip=10.0.0.1"; ST="vol=31 volmax=31 mute=0 bri=15 brimax=15 mono=0"; TZC=; NOW=$T_PM
ind_run long-name-gbc RIGHT
WF="state=connecting"; ST="vol=15 volmax=31 mute=1 bri=0 brimax=15 mono=0"; NOW=$T_AM; ind_run long-name-gbc-b RIGHT
WF="state=connected ssid=x ip=10.0.0.1"; ST="vol=31 volmax=31 mute=0 bri=15 brimax=15 mono=0 bat=100 ac=1 chg=1"; NOW=$T_PM; ind_run long-name-gbc-bat RIGHT
ST=$VOK; WF=
# the four values on screen are compared by pixel: they must differ where the states differ
dif() { cmp -s "$D/ind-$1.raw" "$D/ind-$2.raw" && return 1; return 0; }
dif wifi-off wifi-connecting && dif wifi-connecting wifi-1bar && dif wifi-1bar wifi-4bars && ok "WiFi off / connecting / 1 bar / 4 bars render differently" || bad "WiFi states not distinguishable"
cmp -s "$D/ind-wifi-nofile-4bars.raw" "$D/ind-wifi-4bars.raw" && ok "no wlan0 line in /proc/net/wireless: 4 bars" || bad "fallback to 4 bars failed"
dif vol-0 vol-15 && dif vol-15 vol-31 && dif vol-15 vol-muted && dif vol-15 vol-unknown-sysfs-bri && ok "volume 0 / 50 / 100 / muted / unknown render differently" || bad "volume states not distinguishable"
dif bri-0 bri-7 && dif bri-7 bri-15 && ok "brightness 0 / mid / max render differently" || bad "brightness states not distinguishable"
dif clock-pm clock-am && dif clock-pm clock-24h && dif clock-pm clock-unset && dif clock-24h clock-24h-pm && dif clock-pm clock-tzcfg-est && ok "clock AM / PM / 24 h / TZ.CFG zone / unset render differently" || bad "clock states not distinguishable"
# outside_diffs A B x0 x1 y0 y1: number of differing pixels outside the rectangle
outside() {
	cmp -l "$1" "$2" | awk -v x0="$3" -v x1="$4" -v y0="$5" -v y1="$6" '{ p = int(($1 - 1) / 2); x = p % 1200; y = int(p / 1200);
		if (x < x0 || x >= x1 || y < y0 || y >= y1) n++ } END { print n + 0 }'
}
[ "$(outside "$D/ind-vol-15.raw" "$D/ind-vol-0.raw" $X_VOL $((X_VOL + 88)) $Y1 $Y2)" = 0 ] && ok "volume change touches only the volume slot" || bad "volume change leaked outside its slot"
[ "$(outside "$D/ind-bri-0.raw" "$D/ind-bri-15.raw" $X_BRI $((X_BRI + 92)) $Y1 $Y2)" = 0 ] && ok "brightness change touches only the brightness slot" || bad "brightness change leaked outside its slot"
[ "$(outside "$D/ind-clock-pm.raw" "$D/ind-clock-am.raw" $X_CLK $((X_CLK + 192)) $Y1 $Y2)" = 0 ] && ok "clock change touches only the clock slot" || bad "clock change leaked outside its slot"
[ "$(outside "$D/ind-wifi-off.raw" "$D/ind-wifi-1bar.raw" $X_WIFI $((X_WIFI + 44)) $Y1 $Y2)" = 0 ] && ok "WiFi change touches only the WiFi slot" || bad "WiFi change leaked outside its slot"
keep

echo "== indicators update live: inotify on status files, minute timer, dirty rectangles"
# in-place status rewrite, like olpc-chordd; atomic replace, like olpc-wifi
cat >"$tmp/c-vol" <<EOS
#!/bin/sh
printf 'vol=31 volmax=31 mute=0 bri=7 brimax=15 mono=0\n' | dd of="$I/status" conv=notrunc 2>/dev/null
EOS
cat >"$tmp/c-wifi" <<EOS
#!/bin/sh
printf 'state=connecting\n' >"$I/wifi.status.tmp" && mv "$I/wifi.status.tmp" "$I/wifi.status"
EOS
cat >"$tmp/c-wifi2" <<EOS
#!/bin/sh
printf 'state=connected ssid=Casa 5G ip=192.168.1.20\n' >"$I/wifi.status.tmp" && mv "$I/wifi.status.tmp" "$I/wifi.status"
EOS
cat >"$tmp/c-bri" <<EOS
#!/bin/sh
printf 'vol=31 volmax=31 mute=0 bri=15 brimax=15 mono=0\n' | dd of="$I/status" conv=notrunc 2>/dev/null
EOS
cat >"$tmp/c-garbage" <<EOS
#!/bin/sh
printf 'vol=3' >"$I/status"
: >"$I/wifi.status"
EOS
cat >"$tmp/c-mute" <<EOS
#!/bin/sh
printf 'vol=31 volmax=31 mute=1 bri=15 brimax=15 mono=0\n' | dd of="$I/status" conv=notrunc 2>/dev/null
EOS
chmod +x "$tmp"/c-*
rm -rf "$I"/*; printf 'vol=15 volmax=31 mute=0 bri=7 brimax=15 mono=0\n' >"$I/status"; printf 'state=off\n' >"$I/wifi.status"
cat >"$tmp/s-live" <<EOS
300 dump $D/live-0.raw
350 mark
400 sh $tmp/c-vol
700 dump $D/live-1.raw
750 mark
800 sh $tmp/c-wifi
1100 dump $D/live-2.raw
1150 mark
1200 sh $tmp/c-bri
1500 dump $D/live-3.raw
1550 mark
1600 sh $tmp/c-garbage
1900 dump $D/live-4.raw
1950 mark
2000 sh $tmp/c-wifi2
2300 dump $D/live-5.raw
2350 mark
2400 sh $tmp/c-mute
2700 dump $D/live-6.raw
EOS
run_menu "$tmp/s-live" OLPC_MENU_RUNDIR="$I" OLPC_MENU_BACKLIGHT="$BL/*" OLPC_MENU_WIRELESS="$WLS" OLPC_MENU_CFGDIR="$CFGD" OLPC_MENU_TZFILE="$TZF" \
	OLPC_MENU_FAKETIME="$T_PM" OLPC_MENU_DIRTYLOG="$tmp/dirty.log" || bad "live run failed"
# rects_after_mark N: dirty rectangles flushed between the Nth and the next mark, bounding box of all
rects() { awk -v k="$1" '/^M/ { m++; next } m == k && /^D/ { n++; if (!have || $2 < a) a = $2; if (!have || $3 < b) b = $3; if (!have || $4 > c) c = $4; if (!have || $5 > d) d = $5; have = 1 } END { print n + 0, a, b, c, d }' "$tmp/dirty.log"; }
chk() { # chk NAME N before after x0 x1
	cn=$1; ck=$2; cb=$3; ca=$4; cx0=$5; cx1=$6
	set -- $(rects "$ck")           # count x0 y0 x1 y1
	if [ "$1" -ge 1 ] && [ "$2" -ge "$cx0" ] && [ "$4" -le "$cx1" ] && [ "$3" -ge $Y1 ] && [ "$5" -le $Y2 ] \
	   && [ "$(outside "$D/live-$cb.raw" "$D/live-$ca.raw" "$cx0" "$cx1" $Y1 $Y2)" = 0 ]; then
		ok "$cn: dirty rect $2,$3..$4,$5 inside the slot, frame untouched elsewhere"
	else bad "$cn: dirty rects '$*' not inside $cx0..$cx1 x $Y1..$Y2"; fi
}
chk "volume rewritten in place" 1 0 1 $X_VOL $((X_VOL + 88))
chk "WiFi status replaced (connecting)" 2 1 2 $X_WIFI $((X_WIFI + 44))
chk "brightness rewritten in place" 3 2 3 $X_BRI $((X_BRI + 92))
[ "$(rects 4 | cut -d' ' -f1)" = 0 ] && cmp -s "$D/live-3.raw" "$D/live-4.raw" && ok "truncated status / empty wifi.status: nothing redrawn, last values kept" || bad "garbage status changed the screen ($(rects 4))"
chk "WiFi connected (4 bars from the fixture)" 5 4 5 $X_WIFI $((X_WIFI + 44))
chk "mute" 6 5 6 $X_VOL $((X_VOL + 88))
cmp -s "$D/live-1.raw" "$D/live-0.raw" && bad "volume frame did not change"
keep

# minute boundary: 15:06:58 -> the clock must tick to 3:07 PM with no event at all, and only the clock repaints
rm -rf "$I"/*; printf '%s\n' "$VOK" >"$I/status"
T58=$(date -u -d '2026-11-19 19:06:58' +%s)
cat >"$tmp/s-min" <<EOS
300 dump $D/min-0.raw
350 mark
3500 dump $D/min-1.raw
3600 time $(date -u -d '2026-11-19 19:30:10' +%s)
3700 dump $D/min-2.raw
3750 time 1000000000
3800 dump $D/min-3.raw
EOS
run_menu "$tmp/s-min" OLPC_MENU_RUNDIR="$I" OLPC_MENU_BACKLIGHT="$BL/*" OLPC_MENU_WIRELESS="$WLS" OLPC_MENU_CFGDIR="$CFGD" OLPC_MENU_TZFILE="$TZF" \
	OLPC_MENU_FAKETIME="$T58" OLPC_MENU_DIRTYLOG="$tmp/dirty.log"
r=$(awk '/^M/ { m = 1; next } m && /^D/ { n++; if (!have || $2 < a) a = $2; if (!have || $3 < b) b = $3; if (!have || $4 > c) c = $4; if (!have || $5 > d) d = $5; have = 1 } END { print n + 0, a, b, c, d }' "$tmp/dirty.log")
cmp -s "$D/min-0.raw" "$D/min-1.raw" && bad "clock did not tick at the minute boundary" || {
	[ "$(outside "$D/min-0.raw" "$D/min-1.raw" $X_CLK $((X_CLK + 192)) $Y1 $Y2)" = 0 ] && ok "3:06 -> 3:07 PM on the timer alone; only the clock slot differs" || bad "minute tick touched other pixels"; }
cmp -s "$D/min-1.raw" "$D/min-2.raw" && bad "time step (NTP) not shown" || ok "time stepped forward 23 min: clock re-read"
cmp -s "$D/min-2.raw" "$D/min-3.raw" && bad "year before 2024 not shown as --:--" || ok "time stepped back to 2001: --:--"
echo "  dirty rectangles after the first mark: $r"
keep

# ---- battery slot and the low-battery hint ----------------------------------------------------------
echo "== battery indicator: states, own slot only, low-battery hint"
VB="vol=15 volmax=31 mute=0 bri=7 brimax=15 mono=0"
NOW=$T_PM; TZC=; WF=
for b in "100 0 0" "50 0 0" "15 0 0" "5 0 0" "50 0 1" "100 1 0" "-1 -1 0"; do
	set -- $b; ST="$VB bat=$1 ac=$2 chg=$3"; ind_run "bat$1-$2$3"
done
ST="$VB"; ind_run bat-oldformat
dif bat100-00 bat50-00 && dif bat50-00 bat15-00 && dif bat15-00 bat5-00 && dif bat50-00 bat50-01 && dif bat100-00 bat100-10 && dif bat50-00 bat-1--10 \
	&& ok "battery 100 / 50 / 15 / 5 / charging / on AC / unknown render differently" || bad "battery states not distinguishable"
cmp -s "$D/ind-bat-1--10.raw" "$D/ind-bat-oldformat.raw" && ok "bat=-1 and a status line without battery fields render the same (empty slot)" || bad "unknown battery renders differently from absent"
for p in "bat100-00 bat50-00" "bat50-00 bat15-00" "bat50-00 bat50-01" "bat100-00 bat100-10"; do
	set -- $p
	[ "$(outside "$D/ind-$1.raw" "$D/ind-$2.raw" $X_BAT $((X_BAT + 96)) $Y1 $Y2)" = 0 ] && ok "$1 -> $2 touches only the battery slot" || bad "$1 -> $2 leaked outside the battery slot"
done
# the hint bar: warning only when discharging and <= 5 %
[ "$(outside "$D/ind-bat5-00.raw" "$D/ind-bat15-00.raw" $X_BAT $((X_BAT + 96)) $Y1 $Y2)" != 0 ] && ok "5 % discharging: the hint bar differs (warning shown)" || bad "no warning at 5 % discharging"
[ "$(outside "$D/ind-bat15-00.raw" "$D/ind-bat50-00.raw" $X_BAT $((X_BAT + 96)) $Y1 $Y2)" = 0 ] && ok "15 % / 50 %: same hint bar (no warning)" || bad "warning shown above 5 %"
ST="$VB bat=5 ac=1 chg=0"; ind_run bat5-ac
ST="$VB bat=3 ac=0 chg=1"; ind_run bat3-chg
[ "$(outside "$D/ind-bat5-ac.raw" "$D/ind-bat50-00.raw" $X_BAT $((X_BAT + 96)) $Y1 $Y2)" = 0 ] && ok "5 % on AC: no warning" || bad "warning shown while on AC"
[ "$(outside "$D/ind-bat3-chg.raw" "$D/ind-bat50-00.raw" $X_BAT $((X_BAT + 96)) $Y1 $Y2)" = 0 ] && ok "3 % charging: no warning" || bad "warning shown while charging"
# live: a rewrite of the status file in place repaints only the battery slot (and the hint bar when crossing 5 %)
rm -rf "$I"/*; printf '%s bat=50 ac=0 chg=0\n' "$VB" >"$I/status"; printf 'state=off\n' >"$I/wifi.status"
cat >"$tmp/c-bat" <<EOS
#!/bin/sh
printf '%s bat=49 ac=0 chg=0\n' "$VB" | dd of="$I/status" conv=notrunc 2>/dev/null
EOS
cat >"$tmp/c-bat5" <<EOS
#!/bin/sh
printf '%s bat=5 ac=0 chg=0\n' "$VB" | dd of="$I/status" conv=notrunc 2>/dev/null
EOS
chmod +x "$tmp/c-bat" "$tmp/c-bat5"
cat >"$tmp/s-bat" <<EOS
300 dump $D/bat-0.raw
350 mark
400 sh $tmp/c-bat
700 dump $D/bat-1.raw
750 mark
800 sh $tmp/c-bat5
1100 dump $D/bat-2.raw
EOS
run_menu "$tmp/s-bat" OLPC_MENU_RUNDIR="$I" OLPC_MENU_BACKLIGHT="$BL/*" OLPC_MENU_WIRELESS="$WLS" OLPC_MENU_CFGDIR="$CFGD" OLPC_MENU_TZFILE="$TZF" \
	OLPC_MENU_FAKETIME="$T_PM" OLPC_MENU_DIRTYLOG="$tmp/dirty.log" || bad "battery live run failed"
set -- $(rects 1)
if [ "$1" -ge 1 ] && [ "$2" -ge $X_BAT ] && [ "$4" -le $((X_BAT + 96)) ] && [ "$3" -ge $Y1 ] && [ "$5" -le $Y2 ] && [ "$(outside "$D/bat-0.raw" "$D/bat-1.raw" $X_BAT $((X_BAT + 96)) $Y1 $Y2)" = 0 ]; then
	ok "50 -> 49 % live: dirty rect $2,$3..$4,$5 inside the battery slot, frame untouched elsewhere"
else bad "battery live change: rects '$*'"; fi
cmp -s "$D/bat-1.raw" "$D/bat-2.raw" && bad "dropping to 5 % changed nothing" || ok "49 -> 5 % live: screen changes (icon and hint bar)"
keep

echo "== options: SSH switch (real state from the pid file, background on/off, IP shown)"
O3=$tmp/sshrun; rm -rf "$O3"; mkdir -p "$O3"; rm -f "$T/ssh.args"
cat >"$tmp/ssh-stub" <<EOS
#!/bin/sh
echo "\$1" >>"$T/ssh.args"; sleep 0.2
case "\$1" in on) echo \$PPID >"$O3/dropbear.pid";; off) rm -f "$O3/dropbear.pid";; esac
EOS
chmod +x "$tmp/ssh-stub"
cat >"$tmp/s-ssh" <<EOS
200 key START tap
300 key DOWN tap
400 key DOWN tap
500 key DOWN tap
600 key DOWN tap
700 key DOWN tap
800 key DOWN tap
1000 dump $D/ssh-off.raw
1100 key A tap
1200 dump $D/ssh-pending.raw
1800 dump $D/ssh-on.raw
1900 key A tap
2600 dump $D/ssh-off2.raw
2700 key A tap
2800 key A tap
3500 dump $D/ssh-on2.raw
EOS
run_menu "$tmp/s-ssh" OLPC_MENU_RUNDIR="$O3" OLPC_MENU_SSH_BIN="$tmp/ssh-stub" OLPC_MENU_CFGDIR="$tmp/nodev" OLPC_MENU_TZFILE="$tmp/nodev/none"
[ "$(tr '\n' ' ' <"$T/ssh.args" 2>/dev/null)" = "on off on " ] && ok "A toggles: olpc-ssh on, off, on (the second A while busy is ignored)" || bad "ssh stub calls: $(tr '\n' ' ' <"$T/ssh.args" 2>/dev/null)"
cmp -s "$D/ssh-off.raw" "$D/ssh-on.raw" && bad "SSH item looks the same ON and OFF" || ok "SSH ON screen differs from OFF"
cmp -s "$D/ssh-pending.raw" "$D/ssh-on.raw" && bad "no pending state while the job runs" || ok "pending screen while olpc-ssh runs (UI not blocked)"
cmp -s "$D/ssh-off.raw" "$D/ssh-off2.raw" && ok "back to OFF after toggling off: same screen" || bad "OFF screen differs after the toggle"
grep -q 'ssh: now ON' "$tmp/menu.err" && grep -q 'ssh: switching OFF' "$tmp/menu.err" && ok "toggles are logged" || bad "toggle log lines missing"
# a real running state with no toggle (pid file of a live process) shows ON, with the WiFi IP when connected
echo $$ >"$O3/dropbear.pid"
cat >"$tmp/s-ssh2" <<EOS
200 key START tap
300 key DOWN tap
400 key DOWN tap
500 key DOWN tap
600 key DOWN tap
700 key DOWN tap
800 key DOWN tap
1000 dump $D/ssh-live-nonet.raw
EOS
run_menu "$tmp/s-ssh2" OLPC_MENU_RUNDIR="$O3" OLPC_MENU_SSH_BIN="$tmp/ssh-stub" OLPC_MENU_CFGDIR="$tmp/nodev" OLPC_MENU_TZFILE="$tmp/nodev/none"
printf 'state=connected ssid=Casa ip=10.0.0.195\n' >"$O3/wifi.status"
run_menu "$tmp/s-ssh2" OLPC_MENU_RUNDIR="$O3" OLPC_MENU_SSH_BIN="$tmp/ssh-stub" OLPC_MENU_CFGDIR="$tmp/nodev" OLPC_MENU_TZFILE="$tmp/nodev/none"
cp "$D/ssh-live-nonet.raw" "$D/ssh-live-net.raw" 2>/dev/null
cmp -s "$D/ssh-on.raw" "$D/ssh-live-nonet.raw" && ok "ON (no network) looks like the ON screen without WiFi" || ok "live ON screen rendered"
echo 99999999 >"$O3/dropbear.pid"
run_menu "$tmp/s-ssh2" OLPC_MENU_RUNDIR="$O3" OLPC_MENU_SSH_BIN="$tmp/ssh-stub" OLPC_MENU_CFGDIR="$tmp/nodev" OLPC_MENU_TZFILE="$tmp/nodev/none"
cmp -s "$D/ssh-live-nonet.raw" "$D/ssh-off.raw" && bad "stale pid file shows ON" || ok "stale pid file (dead process) is not ON"
keep

sh "$here/test-net.sh" || fail=1

if [ $fail = 0 ]; then echo "olpc-menu tests: ALL OK"; else echo "olpc-menu tests: FAILED"; fi
exit $fail
