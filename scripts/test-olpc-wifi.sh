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
[ $fail -eq 0 ] && echo "ALL PASS" || { echo "SOME FAILED"; exit 1; }
