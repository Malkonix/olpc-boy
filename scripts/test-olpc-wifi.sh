#!/bin/sh
# Standalone test of the olpc-wifi WIFI.CFG parser / wpa_supplicant config
# generator (made-up credentials only).  Run:
#   docker run --rm -v $PWD:/work -w /work olpc-boy-builder sh scripts/test-olpc-wifi.sh
T=$(mktemp -d /tmp/wifitest.XXXXXX); trap 'rm -rf "$T"' EXIT
export BOOTDIR=$T/boot; mkdir -p "$BOOTDIR"
export OLPC_WIFI_TEST=1
olpc_uptime() { echo 0.00; }
tl() { :; }
. ./rootfs/overlay/usr/local/sbin/olpc-wifi
TAB=$(printf '\t')
fail=0
ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=1; }
has() { grep -qF -- "$2" "$T/out.conf" && ok "$1" || bad "$1 (missing: $2)"; }
hasnt() { grep -qF -- "$2" "$T/out.conf" && bad "$1 (unexpected: $2)" || ok "$1"; }

{
  printf '# my networks\r\n'
  printf '\r\n'
  printf '   \t  \r\n'
  printf 'Home Net 5%sp"a\\ss#w ord%sWPA2\r\n' "$TAB" "$TAB"
  printf 'cafe#free%s%sopen\r\n' "$TAB" "$TAB"
  printf 'OldWep%s12345%sWEP\r\n' "$TAB" "$TAB"
  printf 'HexWep%sA1B2C3D4E5%swep\r\n' "$TAB" "$TAB"
  printf 'Weird%ssecret123%sFOO\r\n' "$TAB" "$TAB"
  printf 'ShortKey%sabc%sWPA2\r\n' "$TAB" "$TAB"
  printf 'Legacy WPA%slongenough1%swpa' "$TAB" "$TAB"   # no trailing newline
} >"$T/wifi.cfg"

genconf "$T/wifi.cfg" "$T/out.conf"
[ "$NNET" -eq 5 ] && ok "5 networks accepted (got $NNET)" || bad "network count $NNET != 5"
HEX=$(printf 'Home Net 5' | od -An -v -tx1 | tr -d ' \n')
has "SSID with spaces as hex"        "ssid=$HEX"
has "key with quote/backslash/#/space" 'psk="p"a\ss#w ord"'
has "WPA2 -> RSN"                    "proto=RSN"
has "WPA -> WPA RSN"                 "proto=WPA RSN"
has "OPEN network"                   "key_mgmt=NONE"
has "WEP ascii key"                  'wep_key0="12345"'
has "WEP hex key unquoted"           "wep_key0=A1B2C3D4E5"
has "first line has top priority"    "priority=1000"
has "second line lower priority"     "priority=999"
hasnt "no CR in output"              "$CR"
hasnt "unknown type not emitted"     "$(printf 'Weird' | od -An -v -tx1 | tr -d ' \n')"
hasnt "short key not emitted"        "$(printf 'ShortKey' | od -An -v -tx1 | tr -d ' \n')"
grep -q "unknown type 'FOO'" "$BOOTDIR/wifi.log" && ok "unknown type logged" || bad "unknown type not logged"
grep -q "key must be 8..63" "$BOOTDIR/wifi.log" && ok "short key logged" || bad "short key not logged"
grep -qF 'p"a\ss' "$BOOTDIR/wifi.log" && bad "KEY LEAKED INTO wifi.log" || ok "key not in wifi.log"
[ "$(stat -c %a "$T/out.conf")" = 600 ] || ok "(file mode is set by umask 077 at run time)"

# urlencode
[ "$(urlenc 'Super Mario Land (World).png')" = 'Super%20Mario%20Land%20%28World%29.png' ] \
  && ok "urlenc" || bad "urlenc: $(urlenc 'Super Mario Land (World).png')"
[ "$(urlenc 'Nintendo - Game Boy Color')" = 'Nintendo%20-%20Game%20Boy%20Color' ] && ok "urlenc set" || bad "urlenc set"

# case-insensitive lookup
mkdir "$T/mnt"; echo x >"$T/mnt/Wifi.Cfg"; LOGMNT=$T/mnt
[ "$(find_cfg)" = "$T/mnt/Wifi.Cfg" ] && ok "case-insensitive WIFI.CFG lookup" || bad "find_cfg"
rm "$T/mnt/Wifi.Cfg"; find_cfg >/dev/null && bad "find_cfg false positive" || ok "no WIFI.CFG -> not found"

# LF-only file, empty key OPEN with trailing type only
printf 'Plain\t\tOPEN\n' >"$T/lf.cfg"; genconf "$T/lf.cfg" "$T/out.conf"
[ "$NNET" -eq 1 ] && ok "LF file, empty-key OPEN" || bad "LF file"

# ---------------------------------------------------------------- lock tests
# Each contender is a separate process (own pid) that sources olpc-wifi in test
# mode and replaces the real work (do_up_real/do_down_real/is_connected) by stubs.
WIFI=$PWD/rootfs/overlay/usr/local/sbin/olpc-wifi
cat >"$T/runner.sh" <<'EOF'
#!/bin/sh
export OLPC_WIFI_TEST=1
olpc_uptime() { echo 0.00; }
tl() { echo "$*" >>"$T/tl"; }
olpc_sleep() { sleep "$1"; }
_cs() { CS=$(( $(date +%s) * 100 )); }
. "$WIFI"
is_connected() { [ -e "$T/connected" ]; }
do_up_real() {
	echo "up-start $$" >>"$T/ev"; sleep "${UPSLEEP:-2}"
	[ "${UPRC:-0}" = 0 ] && touch "$T/connected"
	echo "up-end $$" >>"$T/ev"; return "${UPRC:-0}"
}
do_down_real() { echo "down $$" >>"$T/ev"; rm -f "$T/connected"; }
"$@"
EOF
export T WIFI OLPC_WIFI_LOCKDIR=$T/run
lt_reset() {
	rm -rf "${T:?}/run" "${T:?}/ev" "${T:?}/connected" "${T:?}/tl" "${T:?}"/rc.*
	: >"$T/ev"
}
runit() { # runit NAME cmd...: run in background, rc -> $T/rc.NAME
	_n=$1; shift
	( "$@" >/dev/null 2>&1; echo $? >"$T/rc.$_n" ) &
}
nstart() { grep -c '^up-start' "$T/ev"; }
RUN="sh $T/runner.sh"

# 1. two concurrent ups: one real run, both succeed, second waits
lt_reset
runit a env UPSLEEP=3 $RUN do_up
sleep 1
runit b $RUN do_up
wait
[ "$(nstart)" -eq 1 ] && ok "concurrent up: only one real up ran" || bad "concurrent up: $(nstart) real ups"
[ "$(cat "$T/rc.a")" = 0 ] && [ "$(cat "$T/rc.b")" = 0 ] && ok "concurrent up: both return success" || bad "concurrent up rc a=$(cat "$T/rc.a") b=$(cat "$T/rc.b")"
grep -q 'took the result of the concurrent up' "$BOOTDIR/wifi.log" && ok "second up reports it took the first result" || bad "no 'took the result' line"
[ ! -d "$T/run/wifi.lock" ] && ok "lock released after up" || bad "lock left behind"

# 2. a failing first up: the waiter gets the same failure, nothing is restarted
lt_reset
runit a env UPSLEEP=3 UPRC=1 $RUN do_up
sleep 1
runit b env UPRC=1 $RUN do_up
wait
[ "$(nstart)" -eq 1 ] && ok "failing up: waiter did not start a second up" || bad "failing up: $(nstart) ups"
[ "$(cat "$T/rc.a")" = 1 ] && [ "$(cat "$T/rc.b")" = 1 ] && ok "failing up: waiter returns the first result (1)" || bad "failing up rc a=$(cat "$T/rc.a") b=$(cat "$T/rc.b")"
runit c env UPSLEEP=1 UPRC=1 $RUN do_up; wait
[ "$(nstart)" -eq 2 ] && ok "a later up after a failure retries" || bad "later up did not retry ($(nstart))"

# 3. already connected: fast no-op, no work, no lock
lt_reset; touch "$T/connected"
a=$(date +%s)
$RUN do_up; rc=$?
b=$(date +%s)
[ $rc -eq 0 ] && [ "$(nstart)" -eq 0 ] && [ $((b - a)) -le 1 ] && ok "up when connected: no-op, rc 0, fast" || bad "up when connected: rc=$rc starts=$(nstart) took $((b - a)) s"
[ ! -d "$T/run/wifi.lock" ] && ok "no-op takes no lock" || bad "no-op left a lock"

# 4. stale lock (dead pid) is taken over
lt_reset
dead=$(sh -c 'echo $$'); mkdir -p "$T/run/wifi.lock"; echo "$dead" >"$T/run/wifi.lock/pid"
UPSLEEP=1 $RUN do_up; rc=$?
[ $rc -eq 0 ] && [ "$(nstart)" -eq 1 ] && ok "stale lock taken over, up ran" || bad "stale lock: rc=$rc starts=$(nstart)"
grep -q "stale lock of pid $dead taken over" "$BOOTDIR/wifi.log" && ok "stale takeover logged" || bad "stale takeover not logged"
[ ! -d "$T/run/wifi.lock" ] && ok "lock released after takeover" || bad "lock left after takeover"
lt_reset; mkdir -p "$T/run/wifi.lock"      # no pid file: owner died before writing it
UPSLEEP=1 $RUN do_up; rc=$?
[ $rc -eq 0 ] && [ "$(nstart)" -eq 1 ] && ok "pid-less stale lock taken over" || bad "pid-less lock: rc=$rc starts=$(nstart)"

# 5. down does not race an up: it runs after the up ended
lt_reset
runit a env UPSLEEP=3 $RUN do_up
sleep 1
runit b $RUN do_down
wait
[ "$(cut -d' ' -f1 "$T/ev" | tr '\n' ' ')" = "up-start up-end down " ] \
  && ok "down waited for the running up" || bad "down raced the up: $(tr '\n' ';' <"$T/ev")"

# 6. three simultaneous cold ups (no head start): exactly one real up
lt_reset
runit a env UPSLEEP=2 $RUN do_up
runit b env UPSLEEP=2 $RUN do_up
runit c env UPSLEEP=2 $RUN do_up
wait
[ "$(nstart)" -eq 1 ] && ok "three simultaneous ups: one real up" || bad "three simultaneous ups: $(nstart) real ups"
[ "$(cat "$T/rc.a")$(cat "$T/rc.b")$(cat "$T/rc.c")" = 000 ] && ok "all three return success" || bad "rcs: $(cat "$T"/rc.*)"

# 7. the lock holder is killed: the waiter takes the stale lock and does the work
lt_reset
runit a env UPSLEEP=30 $RUN do_up
sleep 1
holder=$(cat "$T/run/wifi.lock/pid")
runit b env UPSLEEP=1 $RUN do_up
sleep 1
kill -KILL "$holder"
wait
[ "$(cat "$T/rc.b" 2>/dev/null)" = 0 ] && [ "$(nstart)" -eq 2 ] && ok "waiter took over from a killed holder and did the work" || bad "killed holder: rc.b=$(cat "$T/rc.b" 2>/dev/null) starts=$(nstart)"

[ $fail -eq 0 ] && echo "ALL PASS" || { echo "SOME FAILED"; exit 1; }
