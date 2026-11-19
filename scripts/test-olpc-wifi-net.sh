#!/bin/sh
# Standalone test of the olpc-wifi network manager (scan, list, add/forget/connect, autoconnect,
# autostart, link events) against stub commands; made-up networks and keys only.  Run:
#   docker run --rm -v $PWD:/work -w /work olpc-boy-builder sh scripts/test-olpc-wifi-net.sh
T=$(mktemp -d /tmp/wifinet.XXXXXX); trap 'rm -rf "$T"' EXIT
export BOOTDIR=$T/boot; mkdir -p "$BOOTDIR" "$T/run" "$T/wpa" "$T/wdir" "$T/mnt"
export OLPC_WIFI_TEST=1 OLPC_WIFI_IF=lo OLPC_WIFI_LOCKDIR=$T/run OLPC_WIFI_SCANWAIT=0
olpc_uptime() { echo 0.00; }
tl() { echo "$*" >>"$T/timeline.log"; }
olpc_sleep() { sleep 0.05; }
_cs() { CS=$(( $(date +%s) * 100 )); }
setvars() { WDIR=$T/wdir; CONF=$WDIR/wpa.conf; WPALOG=$WDIR/wpa.log; WPAPID=$WDIR/wpa.pid; DHCPID=$WDIR/dhcp.pid; WCLIPID=$WDIR/wpa_cli.pid; CTRL=$T/ctrl; LOGMNT=$T/mnt; IF=lo; }
. ./rootfs/overlay/usr/local/sbin/olpc-wifi
WDIR=$T/wdir; CONF=$WDIR/wpa.conf; WPALOG=$WDIR/wpa.log; WPAPID=$WDIR/wpa.pid; DHCPID=$WDIR/dhcp.pid
WCLIPID=$WDIR/wpa_cli.pid; CTRL=$T/ctrl; LOGMNT=$T/mnt
TAB=$(printf '\t')
fail=0; npass=0
ok()  { npass=$((npass + 1)); echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=1; }
eq()  { [ "$2" = "$3" ] && ok "$1" || { bad "$1"; echo "   expected: $3"; echo "   got:      $2"; }; }

# ---------------------------------------------------------------- stubs (argv of every external command is recorded)
ARGV=$T/argv.log
bounded() { shift; "$@"; }
wait_selftest() { :; }
rfkill() { echo "rfkill $*" >>"$ARGV"; }
modprobe() { echo "modprobe $*" >>"$ARGV"; }
iw() { echo "iw $*" >>"$ARGV"; [ -f "$T/iw.err" ] && cat "$T/iw.err" >&2; [ -f "$T/iw.out" ] && cat "$T/iw.out"; return "$(cat "$T/iw.rc" 2>/dev/null || echo 0)"; }
ip() {
	echo "ip $*" >>"$ARGV"
	case "$*" in
	"link show"*) [ -e "$T/ifup" ] && echo "3: lo: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500" || echo "3: lo: <BROADCAST,MULTICAST> mtu 1500" ;;
	"link set"*" up") touch "$T/ifup" ;;
	"link set"*" down") rm -f "$T/ifup" ;;
	"-4 addr show"*) [ -f "$T/ip" ] && echo "    inet $(cat "$T/ip")/24 brd 10.0.0.255 scope global lo" ;;
	*"addr flush"*) rm -f "$T/ip" ;;
	esac
	return 0
}
udhcpc() { echo "udhcpc $*" >>"$ARGV"; [ -e "$T/dhcpfail" ] || echo 10.0.0.9 >"$T/ip"; [ -e "$T/dhcpfail" ] && return 1; return 0; }
ntpd() { echo "ntpd $*" >>"$ARGV"; return 1; }
wpa_supplicant() {
	echo "wpa_supplicant $*" >>"$ARGV"
	case "$*" in *"-D nl80211"*) [ -e "$T/wpa/failnl" ] && return 1 ;; esac
	echo DISCONNECTED >"$T/wpa/state"
	sleep 30
}
# wpa_cli stub: state in $T/wpa/{state,ssid,results}; select_network applies $T/wpa/after (state) and appends
# $T/wpa/logadd to the supplicant log
wpa_cli() {
	echo "wpa_cli $*" >>"$ARGV"
	_c=""; for _a in "$@"; do _c=$_a; done
	case "$*" in *"-B"*) echo 4242 >"$WCLIPID"; return 0 ;; esac
	[ -e "$T/wpa/state" ] || return 1
	case "$1$2$3$4" in *) ;; esac
	for _a in "$@"; do case "$_a" in status|scan|scan_results|reconfigure|select_network|enable_network|terminate|signal_poll) _c=$_a ;; esac; done
	case "$_c" in
	status) echo "wpa_state=$(cat "$T/wpa/state")"; [ -f "$T/wpa/ssid" ] && echo "ssid=$(cat "$T/wpa/ssid")" ;;
	scan) echo OK ;;
	scan_results) cat "$T/wpa/results" 2>/dev/null ;;
	reconfigure) echo OK ;;
	select_network) echo OK; [ -f "$T/wpa/after" ] && cat "$T/wpa/after" >"$T/wpa/state"; [ -f "$T/wpa/logadd" ] && cat "$T/wpa/logadd" >>"$WPALOG" ;;
	enable_network) echo OK ;;
	terminate) rm -f "$T/wpa/state" ;;
	esac
}
reset() {
	rm -rf "${T:?}/run" "${T:?}/wpa" "${T:?}/wdir" "$T/ifup" "$T/ip" "$T/iw.out" "$T/iw.err" "$T/iw.rc" "$T/dhcpfail" "$ARGV" "$T/timeline.log" "$BOOTDIR/wifi.log"
	mkdir -p "$T/run" "$T/wpa" "$T/wdir"; : >"$ARGV"; : >"$WPALOG"
	unset OLPC_WIFI_CFG OLPC_WIFI_AUTOCFG
	store_mounted() { return 0; }
}
SECRET='Sup3rS3cretKey!#'

# ---------------------------------------------------------------- fixtures
tabs() { tr '|' '\t'; }
cat >"$T/wpa.fix" <<'EOF'
bssid / frequency / signal level / flags / ssid
aa:00:00:00:00:01|2412|-45|[WPA2-PSK-CCMP][ESS]|HomeNet
aa:00:00:00:00:02|2437|-70|[WPA2-PSK-CCMP][ESS]|HomeNet
aa:00:00:00:00:03|2462|-60|[WPA-PSK-TKIP][ESS]|OldWPA
aa:00:00:00:00:04|2412|-80|[WEP][ESS]|WepCafe
aa:00:00:00:00:05|2417|-55|[ESS]|FreeWifi
aa:00:00:00:00:06|2422|-50|[WPA-PSK-TKIP][WPA2-PSK-CCMP][ESS]|Mixed Mode
aa:00:00:00:00:07|2427|-52|[WPA2-EAP-CCMP][ESS]|CorpEAP
aa:00:00:00:00:08|2432|-53|[WPA2-SAE-CCMP][ESS]|SaeOnly
aa:00:00:00:00:09|2437|-54|[WPA2-PSK+SAE-CCMP][ESS]|Transition
aa:00:00:00:00:0a|2442|-48|[WPA2-PSK-CCMP][ESS]|
aa:00:00:00:00:0b|2447|-49|[WPA2-PSK-CCMP][ESS]|\x00\x00\x00
aa:00:00:00:00:0c|2452|-61|[ESS]|Caf\xc3\xa9 \xe2\x82\xac
aa:00:00:00:00:0d|2457|-62|[ESS]|bad\xff\xfebytes
aa:00:00:00:00:0e|2462|-63|[ESS]|back\\slash
aa:00:00:00:00:0f|2467|-64|[ESS]|ctl\x01x
aa:00:00:00:00:10|2472|-65|[IBSS]|AdHoc
aa:00:00:00:00:11|2412|70|[WPA2-PSK-CCMP][ESS]|QualityNet
aa:00:00:00:00:12|2412|-90|[ESS]|say \"hi\"
EOF
tabs <"$T/wpa.fix" >"$T/wpa.results"
cat >"$T/expected.scan" <<'EOF'
HomeNet|-45|WPA2|1|1
Mixed Mode|-50|WPA2|0|0
Transition|-54|WPA2|0|0
FreeWifi|-55|OPEN|0|0
OldWPA|-60|WPA|0|0
Café €|-61|OPEN|0|0
bad\xff\xfebytes|-62|OPEN|0|0
back\x5cslash|-63|OPEN|0|0
ctl\x01x|-64|OPEN|0|0
QualityNet|-65|WPA2|0|0
WepCafe|-80|WEP|1|0
say "hi"|-90|OPEN|0|0
EOF
tabs <"$T/expected.scan" >"$T/expected.tsv"

RSN_PSK='\tRSN:\t * Version: 1\n\t\t * Group cipher: CCMP\n\t\t * Pairwise ciphers: CCMP\n\t\t * Authentication suites: PSK\n\t\t * Capabilities: 16-PTKSA-RC (0x000c)\n'
WPA_PSK='\tWPA:\t * Version: 1\n\t\t * Group cipher: TKIP\n\t\t * Pairwise ciphers: TKIP\n\t\t * Authentication suites: PSK\n'
RSN_EAP='\tRSN:\t * Version: 1\n\t\t * Authentication suites: IEEE 802.1X\n'
RSN_SAE='\tRSN:\t * Version: 1\n\t\t * Authentication suites: SAE\n'
IWFIX=$T/iw.fix
{
	printf 'BSS aa:00:00:00:00:01(on wlan0) -- associated\n'; printf '\tcapability: ESS Privacy ShortSlotTime (0x0431)\n\tsignal: -45.00 dBm\n\tSSID: HomeNet\n'; printf "$RSN_PSK"
	printf 'BSS aa:00:00:00:00:02(on wlan0)\n\tcapability: ESS Privacy (0x0411)\n\tsignal: -70.00 dBm\n\tSSID: HomeNet\n'; printf "$RSN_PSK"
	printf 'BSS aa:00:00:00:00:03(on wlan0)\n\tcapability: ESS Privacy (0x0411)\n\tsignal: -60.00 dBm\n\tSSID: OldWPA\n'; printf "$WPA_PSK"
	printf 'BSS aa:00:00:00:00:04(on wlan0)\n\tcapability: ESS Privacy (0x0411)\n\tsignal: -80.00 dBm\n\tSSID: WepCafe\n'
	printf 'BSS aa:00:00:00:00:05(on wlan0)\n\tcapability: ESS ShortSlotTime (0x0401)\n\tsignal: -55.00 dBm\n\tSSID: FreeWifi\n'
	printf 'BSS aa:00:00:00:00:06(on wlan0)\n\tcapability: ESS Privacy (0x0411)\n\tsignal: -50.00 dBm\n\tSSID: Mixed Mode\n'; printf "$WPA_PSK"; printf "$RSN_PSK"
	printf 'BSS aa:00:00:00:00:07(on wlan0)\n\tcapability: ESS Privacy (0x0411)\n\tsignal: -52.00 dBm\n\tSSID: CorpEAP\n'; printf "$RSN_EAP"
	printf 'BSS aa:00:00:00:00:08(on wlan0)\n\tcapability: ESS Privacy (0x0411)\n\tsignal: -53.00 dBm\n\tSSID: SaeOnly\n'; printf "$RSN_SAE"
	printf 'BSS aa:00:00:00:00:0a(on wlan0)\n\tcapability: ESS Privacy (0x0411)\n\tsignal: -48.00 dBm\n\tSSID: \n'; printf "$RSN_PSK"
	printf 'BSS aa:00:00:00:00:0b(on wlan0)\n\tcapability: ESS Privacy (0x0411)\n\tsignal: -49.00 dBm\n\tSSID: \\x00\\x00\\x00\n'; printf "$RSN_PSK"
	printf 'BSS aa:00:00:00:00:0c(on wlan0)\n\tcapability: ESS (0x0401)\n\tsignal: -61.00 dBm\n\tSSID: Caf\\xc3\\xa9 \\xe2\\x82\\xac\n'
	printf 'BSS aa:00:00:00:00:0d(on wlan0)\n\tcapability: ESS (0x0401)\n\tsignal: -62.00 dBm\n\tSSID: bad\\xff\\xfebytes\n'
	printf 'BSS aa:00:00:00:00:0e(on wlan0)\n\tcapability: ESS (0x0401)\n\tsignal: -63.00 dBm\n\tSSID: back\\x5cslash\n'
	printf 'BSS aa:00:00:00:00:0f(on wlan0)\n\tcapability: ESS (0x0401)\n\tsignal: -64.00 dBm\n\tSSID: ctl\\x01x\n'
	printf 'BSS aa:00:00:00:00:10(on wlan0)\n\tcapability: IBSS (0x0002)\n\tsignal: -65.00 dBm\n\tSSID: AdHoc\n'
	printf 'BSS aa:00:00:00:00:11(on wlan0)\n\tcapability: ESS Privacy (0x0411)\n\tsignal: 35/100\n\tSSID: QualityNet\n'; printf "$RSN_PSK"
	printf 'BSS aa:00:00:00:00:12(on wlan0)\n\tcapability: ESS (0x0401)\n\tsignal: -90.00 dBm\n\tSSID: say "hi"\n'
} >"$IWFIX"
# iw shows the 35/100 network at 35/2-100 = -83; the wpa_cli one reports "70" (quality) = -65
cat >"$T/exp.iw.src" <<'EXPEOF'
HomeNet|-45|WPA2|1|0
Mixed Mode|-50|WPA2|0|0
FreeWifi|-55|OPEN|0|0
OldWPA|-60|WPA|0|0
Café €|-61|OPEN|0|0
bad\xff\xfebytes|-62|OPEN|0|0
back\x5cslash|-63|OPEN|0|0
ctl\x01x|-64|OPEN|0|0
WepCafe|-80|WEP|1|0
QualityNet|-83|WPA2|0|0
say "hi"|-90|OPEN|0|0
EXPEOF
tabs <"$T/exp.iw.src" >"$T/exp.iw2"

# the saved list (and the CRLF/comment conventions) used by most tests
mkcfg() {
	{
		printf '# my networks\r\n\r\n'
		printf 'HomeNet%s%s%sWPA2\r\n' "$TAB" "$SECRET" "$TAB"
		printf 'WepCafe%s12345%sWEP\r\n' "$TAB" "$TAB"
		printf 'Gone Away%sgoneaway1%swpa\r\n' "$TAB" "$TAB"
	} >"$1"
}
nokey() { # nokey NAME: the secret must not be in any file of the run tree except the cfg files and wpa.conf
	_leak=$(grep -rlF -- "$SECRET" "$T" 2>/dev/null | grep -v -i -e '\.cfg' -e '\.orig' -e 'WIFI' -e 'wpa.conf' -e 'cfg.cur' | head -n3)
	[ -z "$_leak" ] && ok "$1: key in no log/status/scan/argv file" || bad "$1: key leaked into: $_leak"
}

# ================================================================ scan: wpa_supplicant running
echo "== scan (wpa_supplicant running)"
reset; mkcfg "$T/wifi.cfg"; export OLPC_WIFI_CFG=$T/wifi.cfg
echo COMPLETED >"$T/wpa/state"; echo HomeNet >"$T/wpa/ssid"; cp "$T/wpa.results" "$T/wpa/results"
do_scan >"$T/scan.out" 2>"$T/scan.err"; rc=$?
eq "scan rc 0" "$rc" 0
eq "scan output: merged, sorted, escaped, flags" "$(cat "$T/scan.out")" "$(cat "$T/expected.tsv")"
eq "wifi.scan equals stdout" "$(cat "$T/run/wifi.scan")" "$(cat "$T/scan.out")"
[ ! -e "$T/run/wifi.scan.tmp" ] && ok "no temp file left" || bad "wifi.scan.tmp left"
grep -q 'wpa_cli.* scan$' "$ARGV" && ok "asked the running supplicant to scan" || bad "no wpa_cli scan"
grep -q '^iw ' "$ARGV" && bad "iw used while wpa_supplicant runs" || ok "iw not used while running"
[ ! -d "$T/run/wifi.lock" ] && ok "lock released" || bad "lock left"
nokey scan

echo "== scan: empty result keeps an empty wifi.scan; not connected -> flag 0"
echo DISCONNECTED >"$T/wpa/state"; rm -f "$T/wpa/ssid"
printf 'bssid / frequency / signal level / flags / ssid\n' >"$T/wpa/results"
echo stale >"$T/run/wifi.scan"
do_scan >"$T/scan.out" 2>/dev/null; rc=$?
[ "$rc" = 0 ] && [ ! -s "$T/run/wifi.scan" ] && [ ! -s "$T/scan.out" ] && ok "empty scan: rc 0, empty file" || bad "empty scan rc=$rc"
cp "$T/wpa.results" "$T/wpa/results"
do_scan 2>/dev/null | grep -q "^HomeNet${TAB}-45${TAB}WPA2${TAB}1${TAB}0$" && ok "HomeNet saved, not connected" || bad "connected flag while disconnected"

echo "== scan: failure leaves wifi.scan untouched"
echo previous >"$T/run/wifi.scan"; echo garbage >"$T/wpa/results"
do_scan >"$T/scan.out" 2>"$T/scan.err"; rc=$?
[ "$rc" != 0 ] && grep -q '^RESULT failed ' "$T/scan.err" && [ "$(cat "$T/run/wifi.scan")" = previous ] && ok "failed scan: rc!=0, RESULT on stderr, file untouched" || bad "failed scan handling rc=$rc: $(cat "$T/scan.err")"

# ================================================================ scan: WiFi down
echo "== scan (WiFi down): iw"
reset; mkcfg "$T/wifi.cfg"; export OLPC_WIFI_CFG=$T/wifi.cfg
cp "$IWFIX" "$T/iw.out"
do_scan >"$T/scan.out" 2>"$T/scan.err"; rc=$?
eq "iw fixture parsed (35/100 -> -83 dBm)" "$(cat "$T/scan.out")" "$(cat "$T/exp.iw2")"
grep -q '^iw dev lo scan' "$ARGV" && ok "iw dev IF scan used" || bad "iw not called"
grep -q 'wpa_supplicant' "$ARGV" && bad "supplicant started although iw worked" || ok "no supplicant needed"
[ ! -e "$T/ifup" ] && ok "interface restored to down" || bad "interface left up"
grep -q 'ip link set lo up' "$ARGV" && ok "interface was brought up for the scan" || bad "no ip link up"
grep -q 'rfkill unblock wifi' "$ARGV" && ok "rfkill unblocked" || bad "no rfkill unblock"

echo "== scan (WiFi down): interface already up stays up; busy retried"
reset; mkcfg "$T/wifi.cfg"; export OLPC_WIFI_CFG=$T/wifi.cfg
touch "$T/ifup"; cp "$IWFIX" "$T/iw.out"
do_scan >/dev/null 2>&1
[ -e "$T/ifup" ] && ok "interface that was up is left up" || bad "interface taken down"
reset; mkcfg "$T/wifi.cfg"; export OLPC_WIFI_CFG=$T/wifi.cfg
echo 'command failed: Device or resource busy (-16)' >"$T/iw.err"; echo 240 >"$T/iw.rc"
do_scan >/dev/null 2>"$T/scan.err"
[ "$(grep -c '^iw dev' "$ARGV")" = 3 ] && ok "busy: iw tried 3 times" || bad "busy: $(grep -c '^iw dev' "$ARGV") iw calls"
grep -q 'wpa_supplicant' "$ARGV" && ok "then fell back to a temporary supplicant" || bad "no fallback after busy"

echo "== scan (WiFi down): iw unusable -> temporary supplicant (nl80211, then wext)"
reset; mkcfg "$T/wifi.cfg"; export OLPC_WIFI_CFG=$T/wifi.cfg
echo 'nl80211 not found' >"$T/iw.err"; echo 1 >"$T/iw.rc"
touch "$T/wpa/failnl"; cp "$T/wpa.results" "$T/wpa.results.keep"
# the stub supplicant sets the state itself; scan_results come from $T/wpa/results
cp "$T/wpa.results" "$T/wpa/results"; rm -f "$T/wpa/state"
do_scan >"$T/scan.out" 2>"$T/scan.err"; rc=$?
eq "fallback scan rc" "$rc" 0
grep -q 'wpa_supplicant .*-D nl80211' "$ARGV" && grep -q 'wpa_supplicant .*-D wext' "$ARGV" && ok "tried nl80211 then wext" || bad "driver order: $(grep wpa_supplicant "$ARGV")"
grep -q "^HomeNet${TAB}-45" "$T/scan.out" && ok "results taken from the temporary supplicant" || bad "no results from the fallback"
[ ! -e "$T/wpa/state" ] && ok "temporary supplicant terminated" || bad "supplicant left running"
[ ! -e "$T/ifup" ] && ok "interface down again" || bad "interface left up"
! grep -q 'network' "$ARGV" && ok "no network configured in the temporary supplicant" || bad "network added"

echo "== scan: no adapter -> RESULT nodev"
reset; export OLPC_WIFI_IF=nonexistent0; IF=nonexistent0
do_scan >/dev/null 2>"$T/scan.err"; rc=$?
[ "$rc" != 0 ] && grep -q '^RESULT nodev ' "$T/scan.err" && ok "nodev reported" || bad "nodev: rc=$rc $(cat "$T/scan.err")"
export OLPC_WIFI_IF=lo; IF=lo

echo "== scan: lock honoured"
reset; mkcfg "$T/wifi.cfg"; export OLPC_WIFI_CFG=$T/wifi.cfg
echo COMPLETED >"$T/wpa/state"; cp "$T/wpa.results" "$T/wpa/results"
sleep 30 & holder=$!
mkdir -p "$T/run/wifi.lock"; echo $holder >"$T/run/wifi.lock/pid"
( do_scan >"$T/scan.out" 2>"$T/scan.err"; echo $? >"$T/scan.rc" ) &
sleep 2
[ ! -s "$T/scan.out" ] && [ ! -e "$T/scan.rc" ] && ok "scan waits while up/down/connect holds the lock" || bad "scan ran under a held lock"
wait $! 2>/dev/null; while [ ! -e "$T/scan.rc" ]; do sleep 1; done
[ "$(cat "$T/scan.rc")" = 3 ] && grep -q '^RESULT busy ' "$T/scan.err" && ok "gave up after the wait with RESULT busy" || bad "busy: rc=$(cat "$T/scan.rc") $(cat "$T/scan.err")"
kill $holder 2>/dev/null; wait $holder 2>/dev/null; rm -rf "$T/run/wifi.lock"

# ================================================================ list
echo "== list"
reset; mkcfg "$T/wifi.cfg"; export OLPC_WIFI_CFG=$T/wifi.cfg
eq "saved networks in priority order, types normalised, no keys" "$(do_list)" "$(printf 'HomeNet\tWPA2\nWepCafe\tWEP\nGone Away\tWPA\n')"
do_list | grep -qF "$SECRET" && bad "list shows the key" || ok "list never shows the key"
printf 'Caf\303\251 \377x\tpassword1\tWPA2\nnotype\tkey12345\nopenone\t\t\nbad\tx\tFOO\n' >"$T/odd.cfg"
eq "list: UTF-8 raw, invalid byte escaped, type inferred, unknown type dropped" "$(do_list "$T/odd.cfg")" "$(printf 'Café \\xffx\tWPA2\nnotype\tWPA2\nopenone\tOPEN\n')"

# ================================================================ add
echo "== add"
reset; export OLPC_WIFI_CFG=$T/wifi.cfg; mkcfg "$T/wifi.cfg"
cp "$T/wifi.cfg" "$T/wifi.orig"
out=$(printf '%s\n' "$SECRET" | do_add "New Net" wpa2); rc=$?
eq "add rc 0" "$rc" 0
eq "add RESULT line" "$out" "RESULT ok Saved"
{ printf '# my networks\r\n\r\n'; printf 'New Net\t%s\tWPA2\r\n' "$SECRET"; printf 'HomeNet\t%s\tWPA2\r\n' "$SECRET"; printf 'WepCafe\t12345\tWEP\r\nGone Away\tgoneaway1\twpa\r\n'; } >"$T/exp.cfg"
cmp -s "$T/wifi.cfg" "$T/exp.cfg" && ok "new entry above the first network, header comments on top, CRLF kept" || { bad "cfg after add"; od -c "$T/wifi.cfg" | head -20; }
[ ! -e "$T/wifi.cfg.tmp" ] && ok "no temp file left" || bad "temp left"
printf 'otherkey99\n' | do_add "HomeNet" WPA >/dev/null
eq "replace: HomeNet moved to the top with new key and type, one entry" "$(do_list)" "$(printf 'HomeNet\tWPA\nNew Net\tWPA2\nWepCafe\tWEP\nGone Away\tWPA\n')"
grep -c "^HomeNet" "$T/wifi.cfg" | grep -q '^1$' && ok "no duplicate line" || bad "duplicate HomeNet"
do_add "Cafe\\x20Open" open </dev/null >/dev/null
eq "OPEN: empty key field, SSID escapes decoded" "$(grep 'Cafe Open' "$T/wifi.cfg" | tr -d '\r' | od -An -c | tr -s ' ' | tr -d '\n')" " C a f e O p e n \\t \\t O P E N \\n"
# LF-only file keeps LF; missing file is created (mounted store)
reset; export OLPC_WIFI_CFG=$T/lf.cfg; printf 'one\tkey12345\tWPA2\n' >"$T/lf.cfg"
printf 'abcdefgh\n' | do_add two WPA2 >/dev/null
grep -q "$CR" "$T/lf.cfg" && bad "CR introduced into an LF file" || ok "LF convention kept"
rm -f "$T/new.cfg"; export OLPC_WIFI_CFG=$T/new.cfg
printf 'abcdefgh\n' | do_add fresh WPA2 >/dev/null; rc=$?
[ "$rc" = 0 ] && [ "$(do_list)" = "$(printf 'fresh\tWPA2')" ] && ok "WIFI.CFG created when absent" || bad "create: rc=$rc"
# comments only, no network line: new entry appended after them
printf '# only a comment\n' >"$T/c.cfg"; export OLPC_WIFI_CFG=$T/c.cfg
printf 'abcdefgh\n' | do_add x WPA2 >/dev/null
eq "comment-only file: comment first, entry after" "$(head -n1 "$T/c.cfg")|$(sed -n 2p "$T/c.cfg" | cut -f1)" "# only a comment|x"

echo "== add: validation"
export OLPC_WIFI_CFG=$T/v.cfg; : >"$T/v.cfg"
chk() { # chk NAME EXPECTCODE SSID TYPE KEY
	_o=$(printf '%s\n' "$5" | do_add "$3" "$4" 2>&1); _rc=$?
	case "$_o" in "RESULT $2 "*) [ "$2" = ok ] && [ $_rc = 0 ] && ok "$1" || { [ "$2" != ok ] && [ $_rc != 0 ] && ok "$1" || bad "$1 (rc=$_rc)"; } ;; *) bad "$1: $_o" ;; esac
}
chk "WPA2 7 chars rejected" badkey s WPA2 1234567
chk "WPA2 8 chars ok" ok s WPA2 12345678
chk "WPA2 63 chars ok" ok s63 WPA2 "$(printf 'a%.0s' $(seq 63))"
chk "WPA2 64 chars non-hex rejected" badkey s64 WPA2 "$(printf 'z%.0s' $(seq 64))"
chk "WPA2 64 hex ok" ok s64 WPA2 "$(printf 'A1%.0s' $(seq 32))"
chk "WPA2 65 chars rejected" badkey s65 WPA2 "$(printf 'a%.0s' $(seq 65))"
chk "WPA key with spaces, quotes, backslash, # ok" ok sp WPA 'a "b" \c #d'
chk "WEP 5 ok" ok w5 WEP abcde
chk "WEP 13 ok" ok w13 WEP abcdefghijklm
chk "WEP 10 hex ok" ok w10 WEP 0123456789
chk "WEP 26 hex ok" ok w26 WEP 0123456789abcdef0123456789
chk "WEP 6 chars rejected" badkey w6 WEP abcdef
chk "WEP 10 non-hex rejected" badkey w10x WEP ghijklmnop
chk "unknown type" badtype s FOO key12345
chk "empty SSID" badssid "" WPA2 12345678
chk "SSID 33 bytes" badssid "$(printf 's%.0s' $(seq 33))" WPA2 12345678
chk "SSID 32 bytes ok" ok "$(printf 's%.0s' $(seq 32))" WPA2 12345678
chk "SSID with control byte" badssid 'a\x09b' WPA2 12345678
chk "SSID with newline escape" badssid 'a\x0ab' WPA2 12345678
chk "key with control char" badkey s WPA2 "$(printf 'abcdefg\001h')"
printf '' | do_add s WPA2 >/dev/null 2>&1 && bad "empty stdin accepted" || ok "empty key (no stdin) rejected"
out=$(printf 'whatever\n' | do_add open1 OPEN); eq "OPEN ignores stdin" "$out" "RESULT ok Saved"
grep -q "^open1${TAB}${TAB}OPEN" "$T/v.cfg" && ok "OPEN stored with empty key" || bad "OPEN line"
grep -qF 'whatever' "$T/v.cfg" && bad "OPEN stored the stdin text" || ok "OPEN did not read stdin"

echo "== add: storage problems"
reset; export OLPC_WIFI_CFG=$T/ro.cfg; mkcfg "$T/ro.cfg"; cp "$T/ro.cfg" "$T/ro.orig"
mv_real=$(command -v mv); mv() { return 1; }          # rename fails (read-only / full)
out=$(printf '%s\n' "$SECRET" | do_add "Late" WPA2 2>&1); rc=$?
unset -f mv
[ "$rc" != 0 ] && eq "failed rename: RESULT nostore" "$out" "RESULT nostore $(reason_text nostore)" || bad "failed rename not reported"
cmp -s "$T/ro.cfg" "$T/ro.orig" && ok "old file intact after a failed write" || bad "old file damaged"
[ ! -e "$T/ro.cfg.tmp" ] && ok "temp file removed after a failed write" || bad "temp file left"
export OLPC_WIFI_CFG=$T/nodir/x.cfg           # parent directory missing: cannot even create the temp file
out=$(printf '%s\n' "$SECRET" | do_add "Late" WPA2 2>&1); rc=$?
eq "missing directory: nostore" "$out" "RESULT nostore $(reason_text nostore)"
unset OLPC_WIFI_CFG; LOGMNT=$T/notmounted; mkdir -p "$LOGMNT"; store_mounted() { return 1; }
out=$(printf '%s\n' "$SECRET" | do_add "Late" WPA2 2>&1); rc=$?
eq "no WIFI.CFG and the partition is not mounted: nostore" "$out" "RESULT nostore $(reason_text nostore)"
[ ! -e "$LOGMNT/WIFI.CFG" ] && ok "nothing written into the unmounted directory" || bad "wrote into the unmounted mount point"
out=$(do_forget x 2>&1); eq "forget with no storage: nostore" "$out" "RESULT nostore $(reason_text nostore)"
out=$(do_autoconnect off 2>&1); eq "autoconnect with no storage: nostore" "$out" "RESULT nostore $(reason_text nostore)"
LOGMNT=$T/mnt; store_mounted() { return 0; }
nokey add

# ================================================================ forget
echo "== forget"
reset; export OLPC_WIFI_CFG=$T/wifi.cfg; mkcfg "$T/wifi.cfg"
out=$(do_forget "WepCafe"); rc=$?
eq "forget RESULT" "$out" "RESULT ok Forgotten"
eq "remaining entries keep their order" "$(do_list)" "$(printf 'HomeNet\tWPA2\nGone Away\tWPA\n')"
grep -q "$CR" "$T/wifi.cfg" && ok "CRLF kept" || bad "CRLF lost"
head -n1 "$T/wifi.cfg" | grep -q '^# my networks' && ok "comment kept" || bad "comment lost"
out=$(do_forget "Nope" 2>&1); rc=$?
[ "$rc" != 0 ] && eq "forget unknown: notsaved" "$out" "RESULT notsaved $(reason_text notsaved)" || bad "forget unknown rc"
do_forget 'Gone\x20Away' >/dev/null; eq "escaped SSID argument matches" "$(do_list)" "$(printf 'HomeNet\tWPA2')"

# ================================================================ autoconnect
echo "== autoconnect"
reset; export OLPC_WIFI_CFG=$T/ac/WIFI.CFG; mkdir -p "$T/ac"; mkcfg "$T/ac/WIFI.CFG"; export OLPC_WIFI_AUTOCFG=$T/ac/WIFIAUTO.CFG
eq "default is on when the file is absent" "$(do_autoconnect status)" on
out=$(do_autoconnect off); eq "off RESULT" "$out" "RESULT ok Connect at start: off"
eq "status off" "$(do_autoconnect status)" off
eq "flag file is one line" "$(cat "$T/ac/WIFIAUTO.CFG")" off
[ ! -e "$T/ac/WIFIAUTO.CFG.tmp" ] && ok "no temp left" || bad "tmp left"
do_autoconnect on >/dev/null; eq "on again" "$(do_autoconnect status)" on
printf 'OFF\r\n' >"$T/ac/WIFIAUTO.CFG"; eq "hand-edited 'OFF' with CRLF understood" "$(do_autoconnect status)" off
printf 'garbage\n' >"$T/ac/WIFIAUTO.CFG"; eq "garbage means on" "$(do_autoconnect status)" on
unset OLPC_WIFI_AUTOCFG
mkdir -p "$T/ac2"; mkcfg "$T/ac2/Wifi.Cfg"; export OLPC_WIFI_CFG=; unset OLPC_WIFI_CFG; LOGMNT=$T/ac2
do_autoconnect off >/dev/null
[ -f "$T/ac2/WIFIAUTO.CFG" ] && ok "flag file created next to WIFI.CFG (found case-insensitively)" || bad "flag file location: $(ls "$T/ac2")"
LOGMNT=$T/mnt

# ================================================================ connect
echo "== connect (supplicant running): success"
reset; export OLPC_WIFI_CFG=$T/wifi.cfg; mkcfg "$T/wifi.cfg"
echo COMPLETED >"$T/wpa/state"; echo WepCafe >"$T/wpa/ssid"; echo COMPLETED >"$T/wpa/after"
echo 12345 >/dev/null
out=$(do_connect "Gone Away"); rc=$?
eq "connect RESULT ok" "$out" "RESULT ok Connected"
eq "chosen network is now first in WIFI.CFG" "$(do_list | head -n1)" "$(printf 'Gone Away\tWPA')"
head -n1 "$T/wifi.cfg" | grep -q '^# my networks' && ok "comments still on top" || bad "comment moved"
FIRST=$(sed -n 's/^[ \t]*ssid=//p' "$CONF" | head -n1)
eq "wpa.conf lists it first (network id 0)" "$FIRST" "$(printf 'Gone Away' | od -An -v -tx1 | tr -d ' \n')"
for w in reconfigure select_network enable_network; do grep -q "wpa_cli .*$w" "$ARGV" && ok "wpa_cli $w called" || bad "wpa_cli $w missing"; done
grep -q 'select_network 0' "$ARGV" && ok "select_network 0" || bad "select_network 0"
grep -q 'wpa_supplicant' "$ARGV" && bad "supplicant restarted" || ok "no restart: reconfigure/select only"
grep -q '^udhcpc' "$ARGV" && ok "DHCP re-run" || bad "no DHCP"
case "$(cat "$T/run/wifi.status")" in "state=connected ssid=WepCafe ip=10.0.0.9") ok "status published: $(cat "$T/run/wifi.status")" ;; *) bad "status: $(cat "$T/run/wifi.status")" ;; esac
grep -q 'wpa_cli.*-B.*-a' "$ARGV" && ok "link watcher started (wpa_cli -B -a)" || bad "watcher not started"
[ ! -d "$T/run/wifi.lock" ] && ok "lock released" || bad "lock left"
[ ! -e "$WDIR/cfg.cur" ] && ok "temporary ordered config removed" || bad "cfg.cur left"
nokey connect

echo "== connect: reason codes"
fc() { # fc NAME CODE : state after select / log lines / dhcp prepared by the caller
	out=$(do_connect "HomeNet" 2>&1); rc=$?
	[ "$rc" != 0 ] && [ "$out" = "RESULT $2 $(reason_text "$2")" ] && ok "$1: RESULT $2" || bad "$1: rc=$rc out=$out"
	[ "$2" = badkey ] && return
	[ "$rc" != 0 ] && [ "$(cat "$T/run/wifi.status")" = "state=off" ] && ok "$1: WiFi ends off" || bad "$1: status $(cat "$T/run/wifi.status")"
}
setup_live() { reset; export OLPC_WIFI_CFG=$T/wifi.cfg; mkcfg "$T/wifi.cfg"; echo COMPLETED >"$T/wpa/state"; echo Other >"$T/wpa/ssid"; : >"$WPALOG"; }
setup_live; echo DISCONNECTED >"$T/wpa/after"; echo "WPA: 4-Way Handshake failed - pre-shared key may be incorrect" >"$T/wpa/logadd"
fc "wrong key (early detection)" wrongkey
setup_live; echo DISCONNECTED >"$T/wpa/after"; touch "$T/wpa/logadd"; : >"$T/wpa/logadd"
FAKE=0; _cs() { FAKE=$((FAKE + 1500)); CS=$FAKE; }     # every call advances 15 s: the 30 s window ends quickly
fc "nothing happens: timeout" notfound
setup_live; echo DISCONNECTED >"$T/wpa/after"; echo "wlan0: SME: Trying to authenticate with aa:bb:cc:00:00:01
wlan0: Trying to associate with aa:bb:cc:00:00:01
wlan0: CTRL-EVENT-ASSOC-REJECT bssid=aa:bb:cc:00:00:01 status_code=17" >"$T/wpa/logadd"
fc "AP refuses" rejected
setup_live; echo DISCONNECTED >"$T/wpa/after"; printf 'wlan0: SME: Trying to authenticate with x\nwlan0: Trying to associate with x\n' >"$T/wpa/logadd"
fc "tried but never completed" timeout
_cs() { CS=$(( $(date +%s) * 100 )); }
setup_live; echo COMPLETED >"$T/wpa/after"; touch "$T/dhcpfail"
fc "associated but no DHCP" dhcp
setup_live; sed -i 's/^HomeNet.*/HomeNet\tshort\tWPA2\r/' "$T/wifi.cfg"
fc "saved entry with an invalid key" badkey
setup_live; out=$(do_connect "Unknown" 2>&1); eq "unsaved network: notsaved" "$out" "RESULT notsaved $(reason_text notsaved)"
nokey connect-failures

echo "== connect (supplicant not running): full up with the reordered config"
reset; export OLPC_WIFI_CFG=$T/wifi.cfg; mkcfg "$T/wifi.cfg"
# stubs for do_up_real: start_wpa / wait_assoc
start_wpa() { echo $$ >"$WPAPID"; echo "start_wpa $1" >>"$ARGV"; echo ASSOC >"$T/wpa/state"; return 0; }
wait_assoc() { return 0; }
stop_procs() { :; }
out=$(do_connect "WepCafe"); rc=$?
# the connect ran in a subshell: do_up_real needs wstat to report the SSID
eq "RESULT" "$(echo "$out" | tail -n1)" "RESULT ok Connected"
eq "WepCafe first in WIFI.CFG" "$(do_list | head -n1)" "$(printf 'WepCafe\tWEP')"
FIRST=$(sed -n 's/^[ \t]*ssid=//p' "$CONF" | head -n1)
eq "supplicant configured with it first" "$FIRST" "$(printf 'WepCafe' | od -An -v -tx1 | tr -d ' \n')"
unset -f start_wpa wait_assoc stop_procs
. ./rootfs/overlay/usr/local/sbin/olpc-wifi; setvars

echo "== connect with a read-only WIFI.CFG still connects with the right order"
reset; export OLPC_WIFI_CFG=$T/wifi.cfg; mkcfg "$T/wifi.cfg"; cp "$T/wifi.cfg" "$T/wifi.orig"
echo COMPLETED >"$T/wpa/state"; echo COMPLETED >"$T/wpa/after"; echo x >"$T/wpa/ssid"
mv() { case "$2" in *.cfg) return 1 ;; esac; command mv "$@"; }      # the FAT rename fails, tmpfs works
out=$(do_connect "WepCafe"); rc=$?; unset -f mv
eq "connect ok although the order could not be saved" "$out" "RESULT ok Connected"
cmp -s "$T/wifi.cfg" "$T/wifi.orig" && ok "WIFI.CFG untouched" || bad "WIFI.CFG changed"
eq "wpa.conf has the chosen one first" "$(sed -n 's/^[ \t]*ssid=//p' "$CONF" | head -n1)" "$(printf 'WepCafe' | od -An -v -tx1 | tr -d ' \n')"

# ================================================================ up/down RESULT lines
echo "== up / down RESULT"
reset
do_up() { UPREASON=nodev; return 1; }
out=$(cmd_up 2>&1); rc=$?; eq "up failure reports the reason" "$out" "RESULT nodev $(reason_text nodev)"
do_up() { return 0; }; out=$(cmd_up); eq "up success" "$out" "RESULT ok Connected"
do_down() { return 0; }; out=$(cmd_down); eq "down" "$out" "RESULT ok WiFi off"
. ./rootfs/overlay/usr/local/sbin/olpc-wifi; setvars

# ================================================================ autostart
echo "== autostart"
cmdline_has() { [ -n "$NOWIFI" ] && [ "$1" = olpcboy.nowifi ]; }
ups=0; do_up() { ups=$((ups + 1)); echo up >>"$T/ups"; return 0; }
as() { rm -f "$T/ups"; (do_autostart); [ -f "$T/ups" ] && echo ran || echo skipped; }
reset; export OLPC_WIFI_CFG=$T/wifi.cfg; mkcfg "$T/wifi.cfg"; export OLPC_WIFI_AUTOCFG=$T/auto.cfg; rm -f "$T/auto.cfg"
eq "autoconnect on + saved network: runs up" "$(as)" ran
[ -e "$T/run/wifi.autostarted" ] && ok "marker created" || bad "no marker"
eq "second call in the same boot: skipped" "$(as)" skipped
rm -f "$T/run/wifi.autostarted"; echo off >"$T/auto.cfg"
eq "autoconnect off: skipped" "$(as)" skipped
grep -q 'autostart skipped (connect at start is off)' "$T/timeline.log" && ok "one timeline line says why" || bad "no timeline line"
echo on >"$T/auto.cfg"; NOWIFI=1
eq "olpcboy.nowifi: skipped" "$(as)" skipped; NOWIFI=
export OLPC_WIFI_CFG=$T/missing.cfg; unset OLPC_WIFI_CFG; LOGMNT=$T/empty; mkdir -p "$LOGMNT"
eq "no WIFI.CFG: skipped quietly" "$(as)" skipped
grep -q 'autostart skipped (no WIFI.CFG)' "$T/timeline.log" && ok "timeline: no WIFI.CFG" || bad "timeline line missing"
LOGMNT=$T/mnt; export OLPC_WIFI_CFG=$T/none.cfg; printf '# nothing\n' >"$T/none.cfg"
eq "WIFI.CFG without a network: skipped" "$(as)" skipped
[ ! -e "$T/run/wifi.autostarted" ] && ok "no marker when skipped" || bad "marker on a skip"
. ./rootfs/overlay/usr/local/sbin/olpc-wifi; setvars

# ================================================================ link events
echo "== link events"
reset; echo COMPLETED >"$T/wpa/state"; echo HomeNet >"$T/wpa/ssid"
wstatus connected HomeNet 10.0.0.9; echo 10.0.0.9 >"$T/ip"
do_event lo DISCONNECTED
eq "DISCONNECTED -> connecting" "$(cat "$T/run/wifi.status")" "state=connecting"
rm -f "$T/ip"; : >"$ARGV"
(do_event lo CONNECTED)
eq "CONNECTED -> DHCP re-run, connected again" "$(cat "$T/run/wifi.status")" "state=connected ssid=HomeNet ip=10.0.0.9"
grep -q '^udhcpc' "$ARGV" && ok "udhcpc re-run" || bad "no DHCP on reconnect"
[ ! -d "$T/run/wifi.lock" ] && ok "lock released" || bad "lock left"
touch "$T/dhcpfail"; rm -f "$T/ip"; (do_event lo CONNECTED)
eq "CONNECTED without a lease stays connecting" "$(cat "$T/run/wifi.status")" "state=connecting"
rm -f "$T/dhcpfail"
# events while another operation holds the lock are ignored
wstatus connected HomeNet 10.0.0.9
sleep 30 & holder=$!; mkdir -p "$T/run/wifi.lock"; echo $holder >"$T/run/wifi.lock/pid"; : >"$ARGV"
do_event lo DISCONNECTED; do_event lo CONNECTED
eq "events ignored under the lock" "$(cat "$T/run/wifi.status")" "state=connected ssid=HomeNet ip=10.0.0.9"
grep -q '^udhcpc' "$ARGV" && bad "DHCP ran under the lock" || ok "no DHCP under the lock"
kill $holder; wait $holder 2>/dev/null; rm -rf "$T/run/wifi.lock"
rm -f "$T/wpa/state"; do_event lo DISCONNECTED
eq "DISCONNECTED without a supplicant leaves the status alone" "$(cat "$T/run/wifi.status")" "state=connected ssid=HomeNet ip=10.0.0.9"
do_event lo TERMINATING; do_event lo SCAN-RESULTS; ok "other events ignored"
# dispatch through the real command line: interface name as first argument
reset; echo COMPLETED >"$T/wpa/state"; wstatus connected HomeNet 10.0.0.9
grep -q '^"\$IF")      do_event "\$1" "\$2" ;;' ./rootfs/overlay/usr/local/sbin/olpc-wifi && ok "main dispatches '<ifname> <EVENT>' (wpa_cli -a calling convention)" || bad "no <if> <event> dispatch"
grep -q 'start_watch$' ./rootfs/overlay/usr/local/sbin/olpc-wifi && ok "watcher started after association" || bad "start_watch not called in do_up_real"
grep -q 'for _pf in "\$WCLIPID"' ./rootfs/overlay/usr/local/sbin/olpc-wifi && ok "watcher killed in stop_procs" || bad "watcher not stopped"

echo
echo "$npass checks passed"
[ $fail -eq 0 ] && echo "ALL PASS" || { echo "SOME FAILED"; exit 1; }
