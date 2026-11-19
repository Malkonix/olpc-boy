#!/bin/sh
# Host tests of the launcher's WiFi network screen (netui.c) and the on-screen keyboard (osk.c): unit checks of
# the keyboard, then the real olpc-menu against a fake framebuffer with scripted key events and a stub olpc-wifi.
# Frames can be kept with FRAMES_DIR=dir (raw RGB565 1200x900).  Made-up networks and keys only.
here=$(cd "$(dirname "$0")" && pwd)
CC=${CC:-gcc}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
bad() { echo "  FAIL: $*"; fail=1; }
ok()  { echo "  ok: $*"; }

HCF="-O2 -Wall -Wextra"
echo "== network screen: build, keyboard unit checks"
$CC $HCF -o "$tmp/netselftest" "$here/netselftest.c" "$here/osk.c" 2>"$tmp/cc1.log" || { cat "$tmp/cc1.log"; exit 1; }
$CC $HCF -o "$tmp/olpc-menu" "$here/olpc-menu.c" "$here/menulib.c" "$here/netui.c" "$here/osk.c" "$here/../common/status.c" \
	"$here/../common/gfx.c" "$here/../olpc-run/input.c" "$here/../olpc-run/diag.c" 2>"$tmp/cc.log" || { cat "$tmp/cc.log"; exit 1; }
grep -E "netui\.c|osk\.c|netselftest\.c" "$tmp/cc.log" "$tmp/cc1.log" | grep -i warning && bad "compiler warnings in netui/osk"
"$tmp/netselftest" || fail=1

# ---- fixtures ---------------------------------------------------------------------------------
TAB=$(printf '\t')
S=$tmp/stub; RUN=$tmp/run; D=$tmp/dump
mkdir -p "$S" "$RUN" "$D" "$tmp/noinput" "$tmp/nodev" "$tmp/fs/mnt/sd/roms/gb"
printf 'x' >"$tmp/fs/mnt/sd/roms/gb/Tetris (World).gb"

cat >"$S/list.txt" <<EOF
Casa 5G${TAB}WPA2
Oficina${TAB}WPA2
Cafe Libre${TAB}OPEN
Vecino${TAB}WPA
EOF
# SSID dBm SECURITY saved connected (as `olpc-wifi scan` prints it)
{
	printf 'Casa 5G\t-48\tWPA2\t1\t1\n'; printf 'Cafe Libre\t-71\tOPEN\t1\t0\n'; printf 'Oficina\t-80\tWPA2\t1\t0\n'
	printf 'TP-Link_8A21\t-58\tWPA2\t0\t0\n'; printf 'Andromeda\t-66\tWPA\t0\t0\n'; printf 'Hotel Guest\t-74\tOPEN\t0\t0\n'
	printf 'FamiliaPerez\t-77\tWEP\t0\t0\n'; printf 'INFINITUM12\t-84\tWPA2\t0\t0\n'
} >"$S/scan.txt"
printf 'state=connected ssid=Casa 5G ip=192.168.1.42\n' >"$RUN/wifi.status"

# stub olpc-wifi: records every call (arguments joined by |), the stdin of `add`; behaviour from files in $STUB_DIR
cat >"$tmp/wifi-stub" <<'EOS'
#!/bin/sh
D=$STUB_DIR
cmd=$1
line=$1; shift
for a in "$@"; do line="$line|$a"; done
echo "$line" >>"$D/calls"
case $cmd in
list) cat "$D/list.txt" ;;
scan)
	[ -f "$D/scan.delay" ] && sleep "$(cat "$D/scan.delay")"
	if [ -f "$D/scan.rc" ]; then echo "RESULT failed WiFi adapter not found" >&2; exit 1; fi
	cp "$D/scan.txt" "$STUB_RUN/wifi.scan.tmp" && mv "$STUB_RUN/wifi.scan.tmp" "$STUB_RUN/wifi.scan"
	cat "$D/scan.txt" ;;
add)
	IFS= read -r k; printf '%s' "$k" >"$D/add.stdin"
	[ -f "$D/add.res" ] && { r=$(cat "$D/add.res"); echo "RESULT $r"; exit 1; }
	echo "RESULT ok saved" ;;
connect|up)
	[ -f "$D/act.delay" ] && sleep "$(cat "$D/act.delay")"
	r=ok; [ -f "$D/act.res" ] && r=$(cat "$D/act.res")
	echo "RESULT $r"; case $r in ok*) exit 0;; *) exit 1;; esac ;;
autoconnect)
	case $1 in status) cat "$D/auto" 2>/dev/null || echo on ;; on|off) echo "$1" >"$D/auto"; echo "RESULT ok autoconnect $1" ;; esac ;;
forget) echo "RESULT ok forgotten" ;;
down) echo "RESULT ok off" ;;
autostart) : ;;
esac
exit 0
EOS
chmod +x "$tmp/wifi-stub"

reset_stub() { rm -f "$S/calls" "$S/add.stdin" "$S/scan.delay" "$S/scan.rc" "$S/act.delay" "$S/act.res" "$S/add.res" "$S/auto"; cp "$S/scan.txt.orig" "$S/scan.txt" 2>/dev/null; cp "$S/list.txt.orig" "$S/list.txt" 2>/dev/null; rm -f "$RUN/wifi.scan" "$RUN/net-job.out"; printf 'state=connected ssid=Casa 5G ip=192.168.1.42\n' >"$RUN/wifi.status"; }
cp "$S/scan.txt" "$S/scan.txt.orig"; cp "$S/list.txt" "$S/list.txt.orig"

run_menu() {
	sc=$1; shift
	env STUB_DIR="$S" STUB_RUN="$RUN" OLPC_MENU_FAKEFB="$tmp/fb.raw" OLPC_MENU_SCAN="$tmp/fs/mnt" OLPC_MENU_RUNDIR="$RUN" OLPC_MENU_INPUTDIR="$tmp/noinput" \
		OLPC_MENU_DEVDIR="$tmp/nodev" OLPC_MENU_RUN="$tmp/none" OLPC_MENU_COVERS_BIN="$tmp/none" OLPC_MENU_WIFI_BIN="$tmp/wifi-stub" \
		OLPC_MENU_CFGDIR="$tmp/nodev" OLPC_MENU_TZFILE="$tmp/nodev/none" OLPC_MENU_LOGDIR="$tmp" "$@" \
		"$tmp/olpc-menu" --script "$sc" 2>"$tmp/menu.err"
}
keep() { [ -n "$FRAMES_DIR" ] && { mkdir -p "$FRAMES_DIR"; cp "$D"/*.raw "$FRAMES_DIR"/ 2>/dev/null; }; }
# open Options (START), go to "WiFi networks..." (third entry) and enter it; the screen is open after ~900 ms
open_net() { printf '300 key START tap\n500 key DOWN tap\n650 key DOWN tap\n800 key A tap\n'; }
frame_ok() { [ "$(stat -c %s "$D/$1.raw" 2>/dev/null)" = 2160000 ] || bad "frame $1 missing"; }
calls() { cat "$S/calls" 2>/dev/null; }
nexttime() { sed -n 's/^# next free time //p' "$1"; }
KEY='Tr0ub4dor&3-xkcd_936 correct horse@2026!'

# ---------------------------------------------------------------------------------------------------
echo "== list screen: saved / visible / connected / out of range, scanning state, autostart"
reset_stub; echo 1.2 >"$S/scan.delay"
{ open_net; cat <<EOS
1250 dump $D/net-01-scanning.raw
3000 dump $D/net-02-list.raw
3100 key DOWN tap
3300 key DOWN tap
3500 key DOWN tap
3700 key DOWN tap
3900 dump $D/net-03-list-moved.raw
4000 key B tap
4300 dump $D/net-04-back-options.raw
4400 end
EOS
} >"$tmp/s1"
run_menu "$tmp/s1" || bad "run 1 failed"
for f in net-01-scanning net-02-list net-03-list-moved net-04-back-options; do frame_ok $f; done
cmp -s "$D/net-01-scanning.raw" "$D/net-02-list.raw" && bad "scanning frame equals the final list"
cmp -s "$D/net-02-list.raw" "$D/net-03-list-moved.raw" && bad "moving the selection changed nothing"
calls | grep -qx 'autostart' && ok "the launcher started olpc-wifi autostart once after it was up" || bad "no autostart call: $(calls | tr '\n' ' ')"
[ "$(calls | grep -c '^autostart$')" = 1 ] || bad "autostart called more than once"
[ "$(calls | sed -n 2,4p | sort | tr '\n' ' ')" = "autoconnect|status list scan " ] && ok "opening the screen runs list, autoconnect status and scan" || bad "open calls: $(calls | tr '\n' ' ')"
[ -s "$tmp/fb.raw" ] && ok "frames dumped"
keep

# ---------------------------------------------------------------------------------------------------
echo "== connect to a saved network: progress, failure reason, success"
reset_stub; echo 1.5 >"$S/act.delay"; echo "wrongkey Wrong password (the key was rejected)" >"$S/act.res"
{ open_net; cat <<EOS
1800 key DOWN tap
1950 key A tap
2300 dump $D/net-10-connecting.raw
4000 dump $D/net-11-failed.raw
4100 key A tap
4400 dump $D/net-12-after-failure.raw
4500 end
EOS
} >"$tmp/s2"
run_menu "$tmp/s2" || bad "run 2 failed"
calls | grep -qx 'connect|Oficina' && ok "A on a saved network runs: olpc-wifi connect Oficina" || bad "connect not called: $(calls | tr '\n' ' ')"
calls | grep -q '^add|' && bad "a saved network must not be added again"
cmp -s "$D/net-10-connecting.raw" "$D/net-11-failed.raw" && bad "connecting and failed frames are equal"
cmp -s "$D/net-11-failed.raw" "$D/net-12-after-failure.raw" && bad "the failure box was not dismissed by A"
keep

echo "== forget needs two presses; another key cancels the confirmation"
reset_stub
{ open_net; cat <<EOS
1800 key DOWN tap
1950 key SELECT tap
2300 dump $D/net-20-forget-confirm.raw
2400 key DOWN tap
2550 key SELECT tap
2900 dump $D/net-21-forget-other.raw
3000 end
EOS
} >"$tmp/s3"
run_menu "$tmp/s3" || bad "run 3 failed"
calls | grep -q '^forget' && bad "forget ran without the second press / after a move: $(calls | tr '\n' ' ')" || ok "one press, or a press after another key, forgets nothing"
reset_stub
{ open_net; cat <<EOS
1800 key DOWN tap
1950 key SELECT tap
2200 key SELECT tap
2900 dump $D/net-22-forgotten.raw
3000 end
EOS
} >"$tmp/s3b"
run_menu "$tmp/s3b" || bad "run 3b failed"
calls | grep -qx 'forget|Oficina' && ok "second SELECT on the same row runs: olpc-wifi forget Oficina" || bad "forget not run: $(calls | tr '\n' ' ')"
[ "$(calls | grep -c '^list$')" = 2 ] && ok "the saved list is re-read after forgetting" || bad "list calls: $(calls | grep -c '^list$')"
reset_stub
{ open_net; cat <<EOS
1800 key UP tap
1950 key SELECT tap
2300 dump $D/net-23-select-on-action.raw
2400 end
EOS
} >"$tmp/s3c"
run_menu "$tmp/s3c"; calls | grep -q '^forget' && bad "SELECT on an action row ran forget"
keep

echo "== empty scan, failed scan"
reset_stub; : >"$S/scan.txt"
{ open_net; printf '2000 dump %s/net-30-empty.raw\n2100 end\n' "$D"; } >"$tmp/s4"
run_menu "$tmp/s4" || bad "run 4 failed"
reset_stub; : >"$S/scan.rc"
{ open_net; printf '2000 dump %s/net-31-scan-failed.raw\n2100 end\n' "$D"; } >"$tmp/s4b"
run_menu "$tmp/s4b" || bad "run 4b failed"
cmp -s "$D/net-30-empty.raw" "$D/net-31-scan-failed.raw" && bad "empty scan and failed scan look the same"
keep

echo "== long list scrolls and wraps"
reset_stub
{ i=0; while [ $i -lt 30 ]; do printf 'Network %02d\t-%d\tWPA2\t0\t0\n' "$i" $((50 + i)); i=$((i + 1)); done; } >"$S/scan.txt"
{ open_net; cat <<EOS
1800 key UP tap
2000 dump $D/net-40-wrapped-to-end.raw
2100 key DOWN tap
2300 dump $D/net-41-wrapped-to-start.raw
2400 end
EOS
} >"$tmp/s5"
run_menu "$tmp/s5" || bad "run 5 failed"
cmp -s "$D/net-40-wrapped-to-end.raw" "$D/net-41-wrapped-to-start.raw" && bad "UP from the first row did not wrap"
keep

# ---------------------------------------------------------------------------------------------------
echo "== secured unsaved network: key typed on the keyboard, passed on stdin"
reset_stub
t0=$(nexttime /dev/null)
{ open_net
  printf '1800 key DOWN tap\n1950 key DOWN tap\n2100 key DOWN tap\n2250 key DOWN tap\n2400 key A tap\n2700 dump %s/key-00-start.raw\n' "$D"
  "$tmp/netselftest" --plan 3000 80 "text:$KEY" "dump:$D/key-01-typed.raw" sel "dump:$D/key-02-layer-next.raw" sel \
	"dump:$D/key-03-layer-next2.raw" act:hide "dump:$D/key-04-hidden.raw" act:hide act:ok >"$tmp/plan1"
  grep -v '^#' "$tmp/plan1"
  t=$(nexttime "$tmp/plan1")
  printf '%s dump %s/key-05-after-ok.raw\n%s end\n' $((t + 600)) "$D" $((t + 700))
} >"$tmp/s6"
run_menu "$tmp/s6" || bad "run 6 failed"
printf '%s\n' "$KEY" | tr -d '\n' >"$tmp/key.expect"
[ "$(cat "$S/add.stdin" 2>/dev/null)" = "$KEY" ] && ok "the stub received the exact key on stdin (${#KEY} characters, spaces and symbols included)" || bad "stdin of add: '$(cat "$S/add.stdin" 2>/dev/null)'"
calls | grep -qx 'add|TP-Link_8A21|WPA2' && ok "add got the exact SSID and type as arguments" || bad "add call: $(calls | tr '\n' ' ')"
calls | grep -qxF 'connect|TP-Link_8A21' && ok "connect followed add" || bad "connect missing"
grep -qF -- "$KEY" "$S/calls" && bad "THE KEY APPEARS IN THE ARGUMENTS OF A CALL" || ok "the key is in no command-line argument"
grep -qF -- "$KEY" "$RUN/net-job.out" 2>/dev/null && bad "the key reached the job output file"
[ "$(calls | grep -n '^add|' | cut -d: -f1)" -lt "$(calls | grep -n '^connect|' | cut -d: -f1)" ] && ok "order: add, then connect"
for f in key-00-start key-01-typed key-02-layer-next key-03-layer-next2 key-04-hidden key-05-after-ok; do frame_ok $f; done
cmp -s "$D/key-01-typed.raw" "$D/key-02-layer-next.raw" && bad "SELECT did not change the layer"
cmp -s "$D/key-02-layer-next.raw" "$D/key-03-layer-next2.raw" && bad "second SELECT did not change the layer"
cmp -s "$D/key-03-layer-next2.raw" "$D/key-04-hidden.raw" && bad "Hide did not change the field"
keep

echo "== key too short: refused with a message, nothing runs"
reset_stub
{ open_net
  printf '1800 key DOWN tap\n1950 key DOWN tap\n2100 key DOWN tap\n2250 key DOWN tap\n2400 key A tap\n'
  "$tmp/netselftest" --plan 2700 80 "text:abc" act:ok >"$tmp/plan2"
  grep -v '^#' "$tmp/plan2"
  t=$(nexttime "$tmp/plan2")
  printf '%s dump %s/key-10-too-short.raw\n%s end\n' $((t + 300)) "$D" $((t + 400))
} >"$tmp/s7"
run_menu "$tmp/s7" || bad "run 7 failed"
calls | grep -q '^add|' && bad "a 3-character WPA key was submitted" || ok "OK with an invalid length submits nothing"
frame_ok key-10-too-short
keep

echo "== cancel leaves nothing behind (B on an empty field, and the Cancel key)"
reset_stub
{ open_net
  printf '1800 key DOWN tap\n1950 key DOWN tap\n2100 key DOWN tap\n2250 key DOWN tap\n2400 key A tap\n2700 dump %s/key-20-open.raw\n' "$D"
  "$tmp/netselftest" --plan 3000 80 "text:x" >"$tmp/plan3"
  grep -v '^#' "$tmp/plan3"
  t=$(nexttime "$tmp/plan3")
  printf '%s key B tap\n%s dump %s/key-21-deleted.raw\n%s key B tap\n%s dump %s/key-22-cancelled.raw\n' $((t + 100)) $((t + 400)) "$D" $((t + 500)) $((t + 800)) "$D"
  printf '%s key A tap\n' $((t + 900))
  "$tmp/netselftest" --plan $((t + 1200)) 80 "text:ab" act:cancel >"$tmp/plan3b"
  grep -v '^#' "$tmp/plan3b"
  t=$(nexttime "$tmp/plan3b")
  printf '%s dump %s/key-23-cancelled-by-key.raw\n%s end\n' $((t + 400)) "$D" $((t + 500))
} >"$tmp/s8"
run_menu "$tmp/s8" || bad "run 8 failed"
calls | grep -q '^add|\|^connect|' && bad "cancelling started add/connect: $(calls | tr '\n' ' ')" || ok "no add/connect after cancelling twice"
cmp -s "$D/key-20-open.raw" "$D/key-22-cancelled.raw" && bad "the keyboard is still on screen after B on an empty field"
cmp -s "$D/key-22-cancelled.raw" "$D/key-23-cancelled-by-key.raw" && ok "Cancel key returns to the same list as B on an empty field" || bad "list differs after the Cancel key"
[ ! -e "$S/add.stdin" ] && ok "no key was ever sent" || bad "add.stdin exists"
keep

echo "== Other network...: SSID, security picker, key"
reset_stub
{ open_net
  printf '1800 key UP tap\n1950 key UP tap\n2100 key UP tap\n2250 key A tap\n2600 dump %s/other-00-ssid.raw\n' "$D"
  "$tmp/netselftest" --plan 3000 80 "text:Mi Red Oculta" >"$tmp/plan4"
  grep -v '^#' "$tmp/plan4"
  t=$(nexttime "$tmp/plan4")
  printf '%s dump %s/other-01-ssid-typed.raw\n%s key START tap\n%s dump %s/other-02-security.raw\n' $((t + 300)) "$D" $((t + 400)) $((t + 800)) "$D"
  printf '%s key DOWN tap\n%s key A tap\n%s dump %s/other-03-key.raw\n' $((t + 900)) $((t + 1050)) $((t + 1400)) "$D"
  "$tmp/netselftest" --plan $((t + 1500)) 80 "text:clave12345" >"$tmp/plan4b"
  grep -v '^#' "$tmp/plan4b"
  t=$(nexttime "$tmp/plan4b")
  printf '%s key START tap\n%s dump %s/other-04-connecting.raw\n%s end\n' $((t + 100)) $((t + 500)) "$D" $((t + 600))
} >"$tmp/s9"
echo 1 >"$S/act.delay"
run_menu "$tmp/s9" || bad "run 9 failed"
calls | grep -qx 'add|Mi Red Oculta|WPA' && ok "manual network: add with the typed SSID and the chosen type" || bad "add call: $(calls | tr '\n' ' ')"
[ "$(cat "$S/add.stdin" 2>/dev/null)" = "clave12345" ] && ok "manual network: key on stdin" || bad "stdin: $(cat "$S/add.stdin" 2>/dev/null)"
calls | grep -qx 'connect|Mi Red Oculta' && ok "manual network: connect follows" || bad "no connect"
keep

echo "== open unsaved network: added as OPEN without a key"
reset_stub
{ open_net; cat <<EOS
1800 key DOWN tap
1950 key DOWN tap
2100 key DOWN tap
2250 key DOWN tap
2400 key DOWN tap
2550 key DOWN tap
2700 key A tap
3500 dump $D/net-50-open-added.raw
3600 end
EOS
} >"$tmp/s10"
run_menu "$tmp/s10" || bad "run 10 failed"
calls | grep -qx 'add|Hotel Guest|OPEN' && ok "open network: add Hotel Guest OPEN" || bad "calls: $(calls | tr '\n' ' ')"
calls | grep -qx 'connect|Hotel Guest' && ok "open network: connect" || bad "no connect"
[ "$(cat "$S/add.stdin" 2>/dev/null)" = "" ] && ok "open network: empty stdin" || bad "open network sent a key"
keep

echo "== action rows: Connect at start, WiFi on/off"
reset_stub
{ open_net; cat <<EOS
1800 key UP tap
1950 key A tap
2700 dump $D/net-60-auto-off.raw
2800 key UP tap
2950 key A tap
3500 end
EOS
} >"$tmp/s11"
run_menu "$tmp/s11" || bad "run 11 failed"
calls | grep -qx 'autoconnect|off' && ok "Connect at start: A runs autoconnect off" || bad "calls: $(calls | tr '\n' ' ')"
[ "$(calls | grep -c '^autoconnect|status$')" = 2 ] && ok "the flag is re-read afterwards" || bad "status calls: $(calls | grep -c 'status')"
calls | grep -qx 'down' && ok "WiFi row while connected runs: down" || bad "no down call"
reset_stub; printf 'state=off\n' >"$RUN/wifi.status"
{ open_net; cat <<EOS
1800 key UP tap
1950 key UP tap
2100 key A tap
2800 dump $D/net-61-power-on.raw
2900 end
EOS
} >"$tmp/s11b"
run_menu "$tmp/s11b" || bad "run 11b failed"
calls | grep -qx 'up' && ok "WiFi row while off runs: up" || bad "no up call: $(calls | tr '\n' ' ')"
keep

echo "== chord flag blocks the D-pad on the network screen"
reset_stub
{ open_net; cat <<EOS
1800 dump $D/net-70-before.raw
1900 chord 1
2000 key DOWN tap
2150 key DOWN tap
2300 key UP tap
2600 dump $D/net-71-chord.raw
2700 chord 0
2800 key DOWN tap
3100 dump $D/net-72-after.raw
3200 end
EOS
} >"$tmp/s12"
run_menu "$tmp/s12" || bad "run 12 failed"
cmp -s "$D/net-70-before.raw" "$D/net-71-chord.raw" && ok "D-pad ignored while the chord flag is 1" || bad "the selection moved during a chord"
cmp -s "$D/net-71-chord.raw" "$D/net-72-after.raw" && bad "D-pad dead after the chord ended"
echo "-- chord blocks A too (no connect)"
reset_stub
{ open_net; cat <<EOS
1800 chord 1
1900 key A tap
2500 end
EOS
} >"$tmp/s12b"
run_menu "$tmp/s12b"; calls | grep -q '^connect' && bad "A acted under the chord"
reset_stub; : >"$S/act.res"; rm -f "$S/act.res"
# the keyboard under the chord: moving must be blocked too
{ open_net
  printf '1800 key DOWN tap\n1950 key DOWN tap\n2100 key DOWN tap\n2250 key DOWN tap\n2400 key A tap\n2700 dump %s/key-30-before.raw\n2800 chord 1\n2900 key RIGHT tap\n3050 key DOWN tap\n3400 dump %s/key-31-chord.raw\n3500 end\n' "$D" "$D"
} >"$tmp/s12c"
run_menu "$tmp/s12c"
cmp -s "$D/key-30-before.raw" "$D/key-31-chord.raw" && ok "keyboard cursor does not move under the chord" || bad "the keyboard moved under the chord"
keep

echo "== wifi.status changes are shown live"
reset_stub
cat >"$tmp/c-off" <<EOS
#!/bin/sh
printf 'state=off\n' >"$RUN/wifi.status.tmp"; mv "$RUN/wifi.status.tmp" "$RUN/wifi.status"
EOS
cat >"$tmp/c-conn" <<EOS
#!/bin/sh
printf 'state=connecting\n' >"$RUN/wifi.status.tmp"; mv "$RUN/wifi.status.tmp" "$RUN/wifi.status"
EOS
chmod +x "$tmp/c-off" "$tmp/c-conn"
{ open_net; cat <<EOS
2500 dump $D/net-80-connected.raw
2600 sh $tmp/c-off
3000 dump $D/net-81-off.raw
3100 sh $tmp/c-conn
3500 dump $D/net-82-connecting.raw
3600 end
EOS
} >"$tmp/s13"
run_menu "$tmp/s13" || bad "run 13 failed"
cmp -s "$D/net-80-connected.raw" "$D/net-81-off.raw" && bad "wifi.status off not reflected"
cmp -s "$D/net-81-off.raw" "$D/net-82-connecting.raw" && bad "wifi.status connecting not reflected"
keep

echo "== idle: no CPU on the network screen (no spinner running)"
reset_stub
{ open_net; echo '9000 end'; } >"$tmp/s14"
env STUB_DIR="$S" STUB_RUN="$RUN" OLPC_MENU_FAKEFB="$tmp/fb.raw" OLPC_MENU_SCAN="$tmp/fs/mnt" OLPC_MENU_RUNDIR="$RUN" OLPC_MENU_INPUTDIR="$tmp/noinput" \
	OLPC_MENU_DEVDIR="$tmp/nodev" OLPC_MENU_WIFI_BIN="$tmp/wifi-stub" OLPC_MENU_CFGDIR="$tmp/nodev" OLPC_MENU_TZFILE="$tmp/nodev/none" \
	"$tmp/olpc-menu" --script "$tmp/s14" 2>/dev/null &
pid=$!
sleep 4
t1=$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null); sleep 0.7; t2=$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null)
wait $pid
echo "  cpu ticks at 4 s: ${t1:-?}, 0.7 s later: ${t2:-?}"
[ -n "$t1" ] && [ "$t2" = "$t1" ] && ok "idle network screen used no CPU in 0.7 s" || bad "idle CPU not zero on the network screen"
echo "== idle: no CPU on the keyboard"
{ open_net; printf '1800 key DOWN tap\n1950 key DOWN tap\n2100 key DOWN tap\n2250 key DOWN tap\n2400 key A tap\n9000 end\n'; } >"$tmp/s15"
env STUB_DIR="$S" STUB_RUN="$RUN" OLPC_MENU_FAKEFB="$tmp/fb.raw" OLPC_MENU_SCAN="$tmp/fs/mnt" OLPC_MENU_RUNDIR="$RUN" OLPC_MENU_INPUTDIR="$tmp/noinput" \
	OLPC_MENU_DEVDIR="$tmp/nodev" OLPC_MENU_WIFI_BIN="$tmp/wifi-stub" OLPC_MENU_CFGDIR="$tmp/nodev" OLPC_MENU_TZFILE="$tmp/nodev/none" \
	"$tmp/olpc-menu" --script "$tmp/s15" 2>/dev/null &
pid=$!
sleep 4
t1=$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null); sleep 0.7; t2=$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null)
wait $pid
[ -n "$t1" ] && [ "$t2" = "$t1" ] && ok "idle keyboard used no CPU in 0.7 s" || bad "idle CPU not zero on the keyboard ($t1 -> $t2)"

echo "== the key is wiped: not in the process memory after use (best effort: the typed text is gone from the heap)"
reset_stub
{ open_net
  printf '1800 key DOWN tap\n1950 key DOWN tap\n2100 key DOWN tap\n2250 key DOWN tap\n2400 key A tap\n'
  "$tmp/netselftest" --plan 2700 80 "text:zq9Wm4Xk7pL2" act:ok >"$tmp/plan5"
  grep -v '^#' "$tmp/plan5"
  t=$(nexttime "$tmp/plan5")
  printf '%s sh %s/memscan\n%s end\n' $((t + 800)) "$tmp" $((t + 1000))
} >"$tmp/s16"
cat >"$tmp/memscan" <<'EOS'
#!/bin/sh
# runs inside the menu's process group: look for the key in the memory of the menu (parent of this shell's parent)
pid=$PPID
EOS
printf 'exec python3 -I %s/memscan.py "$PPID"\n' "$tmp" >>"$tmp/memscan"
chmod +x "$tmp/memscan"
cat >"$tmp/memscan.py" <<'EOS'
import re, sys
pid = sys.argv[1]
needle = b"zq9Wm4Xk7pL2"
found = 0
try:
    maps = open("/proc/%s/maps" % pid).read().splitlines()
    mem = open("/proc/%s/mem" % pid, "rb", 0)
    for l in maps:
        m = re.match(r"([0-9a-f]+)-([0-9a-f]+) (r)(.)", l)
        if not m or "[vvar" in l or "[vsyscall" in l: continue
        a, b = int(m.group(1), 16), int(m.group(2), 16)
        if b - a > 64 << 20: continue
        try:
            mem.seek(a); data = mem.read(b - a)
        except Exception:
            continue
        found += data.count(needle)
except Exception as e:
    print("memscan-error", e)
    sys.exit(0)
open(sys.argv[0] + ".out", "w").write(str(found))
EOS
if command -v python3 >/dev/null 2>&1; then
	rm -f "$tmp/memscan.py.out"
	run_menu "$tmp/s16" || bad "run 16 failed"
	if [ -f "$tmp/memscan.py.out" ]; then
		n=$(cat "$tmp/memscan.py.out")
		# the fake framebuffer is a mapped file holding pixels only; the planner's script text is not in memory as a key
		[ "$n" -le 1 ] && ok "memory scan after submit: key found $n time(s) (the script text of the test itself is the only allowed copy)" || bad "the typed key is still in memory $n times"
	else
		echo "  (memory scan not possible here, skipped)"
	fi
else
	echo "  (no python3: memory scan skipped)"
fi
keep

if [ $fail = 0 ]; then echo "olpc-menu network tests: ALL OK"; else echo "olpc-menu network tests: FAILED"; fi
exit $fail
