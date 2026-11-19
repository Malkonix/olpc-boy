#!/bin/sh
# Logic test of olpc-usbdiag against a fake sysfs/debugfs/kmsg tree (no hardware):
# kmsg event parsing, stuck/appear/disconnect/replug snapshots, snapshot cap,
# usbmon ring rotation + content, OHCI register decoding, game back-off,
# recovery gating.  Time is faked (FAKE_CS), nothing really sleeps.  Run:
#   docker run --rm -v $PWD:/work -w /work olpc-boy-builder sh scripts/test-olpc-usbdiag.sh
T=$(mktemp -d /tmp/usbdiagtest.XXXXXX)
cleanup() { [ -r "$T/mon/pid" ] && kill "$(cat "$T/mon/pid")" 2>/dev/null; exec 9>&- 2>/dev/null; kill $(jobs -p) 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
fail=0
ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=1; }

# ---- fake machine -------------------------------------------------------------
S=$T/sys; D=$T/dbg
HUB=$S/devices/pci0000:00/0000:00:0f.4/usb2
mkdir -p "$HUB/power" "$HUB/usb2-port4" "$S/bus/usb/devices" "$S/bus/pci/drivers/ohci-pci" \
	"$D/usb/usbmon" "$D/usb/ohci/0000:00:0f.4" "$T/input" "$T/psu/BAT" "$T/drv" "$T/boot"
ln -s ../../bus/pci/drivers/ohci-pci "$S/devices/pci0000:00/0000:00:0f.4/driver"
ln -s ../../../devices/pci0000:00/0000:00:0f.4/usb2 "$S/bus/usb/devices/usb2"
echo 2 >"$HUB/busnum"; echo 12 >"$HUB/speed"; echo auto >"$HUB/power/control"
echo 0 >"$HUB/usb2-port4/disable"; echo hotplug >"$HUB/usb2-port4/connect_type"
mkfifo "$D/usb/usbmon/2u"
echo "usb devices" >"$D/usb/devices"
echo "async list" >"$D/usb/ohci/0000:00:0f.4/async"; echo "periodic list" >"$D/usb/ohci/0000:00:0f.4/periodic"
echo Discharging >"$T/psu/BAT/status"; echo 4100000 >"$T/psu/BAT/voltage_now"
regs() { # regs PORT0_HEX
	cat >"$D/usb/ohci/0000:00:0f.4/registers" <<EOF
roothub.a 0a000903 POTPGT=10 OCPM NDP=3(3)
roothub.b 00000000 PPCM=0000 DR=0000
roothub.status 00008000 DRWE
roothub.portstatus [0] 0x00000000
roothub.portstatus [1] 0x00000000
roothub.portstatus [2] $1
EOF
}
regs 0x00000103

# fake time and system helpers (the real ones come from common.sh)
FAKE_CS=500
olpc_uptime() { echo "$((FAKE_CS / 100)).$(printf %02d $((FAKE_CS % 100)))"; }
_cs() { CS=$FAKE_CS; }
tl() { echo "$(olpc_uptime) $*" >>"$T/boot/timeline.log"; }
olpc_sync() { :; }
olpc_sleep() { case "$1" in *.*) FAKE_CS=$((FAKE_CS + ${1%%.*} * 100 + ${1#*.} * 10)) ;; *) FAKE_CS=$((FAKE_CS + $1 * 100)) ;; esac; }
bounded() { shift; "$@"; }

export BOOTDIR=$T/boot STAGE=$T/stage
mkdir -p "$STAGE"
export OLPC_USBDIAG_TEST=1 OLPC_USBDIAG_SYSFS=$S/bus/usb/devices OLPC_USBDIAG_DEBUGFS=$D \
	OLPC_USBDIAG_INPUT=$T/input OLPC_USBDIAG_PSU=$T/psu OLPC_USBDIAG_SYSRQ=$T/sysrq \
	OLPC_USBDIAG_RUNNING=$T/running OLPC_USBDIAG_KMSG=$T/kmsg.log OLPC_USBDIAG_DRV=$T/drv \
	OLPC_USBDIAG_CHUNK=1k OLPC_USBDIAG_KEEP=3 OLPC_USBDIAG_SNAPMAX=6
: >"$T/kmsg.log"
. ./rootfs/overlay/usr/local/sbin/olpc-usbdiag
MON=$STAGE/usbmon       # the script derives it from STAGE at source time
OUT=$BOOTDIR/usbdiag

SEQ=0
km() { SEQ=$((SEQ + 1)); printf '6,%d,%s,-;%s\n SUBSYSTEM=usb\n DEVICE=+usb:2-4\n' "$SEQ" "$1" "$2" >>"$T/kmsg.log"; }
nsnap() { n=0; for e in "$OUT"/snap-*; do [ -d "$e" ] && n=$((n + 1)); done; echo $n; }
snapdir() { _w=$(echo "$1" | tr -c "A-Za-z0-9\n" _); for e in "$OUT"/snap-*"$_w"*; do [ -d "$e" ] && { echo "$e"; return; }; done; }

# ---- 1. quiet when nothing happens -----------------------------------------------
mon_find_bus && [ "$MONBUS" = 2 ] && ok "OHCI bus found from sysfs (bus $MONBUS)" || bad "bus detection: '$MONBUS'"
km 737329 "usb 1-1: new high-speed USB device number 2 using ehci-pci"
km 941651 "usb 1-1: New USB device found, idVendor=1286, idProduct=2001, bcdDevice= 0.01"
km 1087752 "usb 1-2: new high-speed USB device number 3 using ehci-pci"
km 1448565 "usb 1-2: New USB device found, idVendor=0781, idProduct=5567, bcdDevice= 1.00"
poll_once; poll_once
[ ! -e "$OUT" ] && [ "$(nsnap)" = 0 ] && ok "no Sony device, no stuck port: nothing written, no snapshot" || bad "not silent: $(ls "$OUT")"
[ -z "$PEND" ] && ok "resolved enumerations leave no pending port" || bad "PEND='$PEND'"
mon_alive && ok "usbmon capture started at service start" || bad "capture not running"

# ---- 2. never-enumerates (as in the 115836 boot): stuck after 10 s ------------------
km 1937762 "usb 2-4: new full-speed USB device number 2 using ohci-pci"
FAKE_CS=$((193 + 5)); poll_once          # 1.98 s of fake uptime: well under 10 s
[ "$(nsnap)" = 0 ] && ok "pending < 10 s: no snapshot" || bad "early snapshot"
FAKE_CS=1400; poll_once
sd=$(snapdir stuck-2-4)
[ -n "$sd" ] && ok "stuck port -> snapshot ($(basename "$sd"))" || bad "no stuck snapshot"
[ "$(cat "$T/sysrq" 2>/dev/null)" = w ] && ok "sysrq-w requested for the stuck case" || bad "no sysrq-w"
grep -qF "roothub.port[2] 0x00000103: CONNECTED ENABLED POWERED" "$sd/state.txt" && ok "port status decoded (connect/enable/power)" || bad "port decode missing"
grep -q "roothub.a  0x0a000903: NDP=3 power-good-delay=20ms per-port-power-switching per-port-over-current-reporting" "$sd/state.txt" && ok "roothub.a decoded (over-current reporting mode)" || bad "roothub.a decode: $(grep 'roothub.a ' "$sd/state.txt")"
grep -q "usb2-port4" "$sd/state.txt" && grep -q "power/control=auto\|control=auto" "$sd/state.txt" && ok "root hub power attrs + port dirs in state" || bad "sysfs power attrs missing"
grep -q "voltage_now=4100000" "$sd/state.txt" && ok "power supplies in state" || bad "psu missing"
grep -q "async list" "$sd/state.txt" && ok "OHCI async schedule in state" || bad "async missing"
FAKE_CS=1410; poll_once
[ "$(nsnap)" = 1 ] && ok "stuck snapshot taken only once per attempt" || bad "stuck repeated: $(nsnap)"

# ---- 3. usbmon ring: bounded, newest data kept, snapshot has the last lines ------------
exec 9>"$D/usb/usbmon/2u"
i=0; while [ $i -lt 120 ]; do echo "ffff8800 $i S Ci:2:003:0 s 80 06 0100 0000 0040 64 <" >&9; i=$((i + 1)); done
echo "LASTLINE-BEFORE-DISCONNECT" >&9
sleep 1
poll_once        # mon_prune
nch=0; for c in "$MON"/c*.*; do [ -e "$c" ] && nch=$((nch + 1)); done
[ "$nch" -le 3 ] && [ "$nch" -ge 1 ] && ok "ring bounded: $nch chunks (keep 3, 1k each)" || bad "ring chunks=$nch"
cat "$MON"/c*.* | grep -q LASTLINE-BEFORE-DISCONNECT && ok "newest capture data kept" || bad "newest data lost"
# The capture rolls chunks by restarting cat on a live stream; the fixture is a
# finite file read in one go, so rotation of old data cannot be shown here.

# ---- 4. replug of a pending port, Sony appear, Sony disconnect -------------------------
km 90687553 "usb 2-4: new full-speed USB device number 3 using ohci-pci"
FAKE_CS=9070; poll_once
[ -n "$(snapdir replug-pending-2-4)" ] && ok "new device on a pending port -> replug-pending snapshot" || bad "no replug snapshot"
km 90958654 "usb 2-4: New USB device found, idVendor=054c, idProduct=05c4, bcdDevice= 2.21"
FAKE_CS=9100; poll_once
[ -n "$(snapdir appear-2-4)" ] && ok "Sony appear -> snapshot" || bad "no appear snapshot"
port_has 2-4 && ok "port 2-4 marked as Sony" || bad "port not marked"
km 91384747 "sony 0003:054C:05C4.0001: feature report 0x81 (MAC) failed: -62, continuing without MAC, calibration and version info"
echo "LAST-OUT-REPORT" >&9; sleep 1
km 91458930 "usb 2-4: USB disconnect, device number 3"
FAKE_CS=9150; poll_once
sd=$(snapdir disconnect-2-4)
[ -n "$sd" ] && ok "Sony disconnect -> snapshot" || bad "no disconnect snapshot"
zcat "$sd"/usbmon-bus2.txt.gz 2>/dev/null | grep -q LAST-OUT-REPORT && ok "disconnect snapshot contains the capture up to the disconnect" || bad "usbmon lines missing in snapshot"
grep -q "feature report 0x81" "$OUT/usbdiag.log" && ok "sony kernel error line logged" || bad "sony line not logged"
port_has 2-4 && bad "port still marked after disconnect" || ok "port cleared after disconnect"

# ---- 5. cap ------------------------------------------------------------------------------
c=0
while [ $c -lt 5 ]; do
	km $((92000000 + c)) "usb 2-4: new full-speed USB device number 5 using ohci-pci"
	km $((92100000 + c)) "usb 2-4: New USB device found, idVendor=054c, idProduct=05c4, bcdDevice= 2.21"
	km $((92200000 + c)) "usb 2-4: USB disconnect, device number 5"
	FAKE_CS=$((9300 + c * 10)); poll_once
	c=$((c + 1))
done
[ "$(nsnap)" = 6 ] && ok "snapshots capped at 6 (got $(nsnap))" || bad "cap: $(nsnap) snapshots"
[ "$(grep -c "snapshot cap" "$OUT/usbdiag.log")" = 1 ] && ok "cap logged once" || bad "cap log count $(grep -c 'snapshot cap' "$OUT/usbdiag.log")"
grep -c "Sony device on 2-4" "$OUT/usbdiag.log" | grep -qv '^0$' && ok "events still logged after the cap" || bad "no events after cap"

# ---- 6. game running: no capture, no snapshots, events still logged ------------------------
sleep 600 & GP=$!
echo $GP >"$T/running"
SNAPMAX_SAVE=$SNAPMAX; SNAPMAX=99; before=$(nsnap)
km 95000000 "usb 2-4: new full-speed USB device number 6 using ohci-pci"
km 95100000 "usb 2-4: New USB device found, idVendor=054c, idProduct=05c4, bcdDevice= 2.21"
FAKE_CS=9500; poll_once
[ "$GAME" = 1 ] && [ "$(nsnap)" = "$before" ] && ok "game running: event logged, no snapshot" || bad "snapshot while playing"
sleep 1; mon_alive && bad "capture still running during a game" || ok "capture paused during a game"
grep -q "skipped, game running" "$OUT/usbdiag.log" && ok "deferred snapshot noted in log" || bad "no skip note"
kill $GP 2>/dev/null; rm -f "$T/running"; SNAPMAX=$SNAPMAX_SAVE

# ---- 7. recovery gating -----------------------------------------------------------------------
DEVD=$S/devices/pci0000:00/0000:00:0f.4/usb2/2-4
mkdir -p "$DEVD/power" "$DEVD/2-4:1.0"
echo 054c >"$DEVD/idVendor"; echo 05c4 >"$DEVD/idProduct"; echo 12 >"$DEVD/speed"; echo 500mA >"$DEVD/bMaxPower"
echo 2 >"$DEVD/busnum"; echo 6 >"$DEVD/devnum"; echo 1 >"$DEVD/bConfigurationValue"; echo 1 >"$DEVD/authorized"
echo on >"$DEVD/power/control"; echo 03 >"$DEVD/2-4:1.0/bInterfaceClass"
ln -s ../../../devices/pci0000:00/0000:00:0f.4/usb2/2-4 "$S/bus/usb/devices/2-4"
SONYNOW=""; sysfs_sony
[ "$SONYNOW" = " 2-4" ] && ok "sysfs scan finds the Sony device" || bad "sysfs scan: '$SONYNOW'"
mkdir -p "$T/input/input7"; echo "Sony Computer Entertainment Wireless Controller" >"$T/input/input7/name"
FAKE_CS=$((FAKE_CS + 5000)); recovery_check
grep -q "attempt" "$OUT/usbdiag.log" && bad "recovery ran although the pad works" || ok "no recovery while the pad has an input device"
rm -rf "$T/input/input7"
FAKE_CS=$((FAKE_CS + 10)); recovery_check
grep -q "attempt 1 (rebind)" "$OUT/usbdiag.log" && ok "recovery starts for a present pad without input" || bad "no recovery"
grep -q "attempt 5 (port-cycle)" "$OUT/usbdiag.log" && ok "all five attempts tried" || bad "attempts incomplete"
n1=$(grep -c "attempt 1 (rebind): start" "$OUT/usbdiag.log")
FAKE_CS=$((FAKE_CS + 5000)); recovery_check
[ "$(grep -c "attempt 1 (rebind): start" "$OUT/usbdiag.log")" = "$n1" ] && ok "recovery runs once per plug-in" || bad "recovery repeated"

[ $fail -eq 0 ] && echo "ALL PASS" || { echo "SOME FAILED"; exit 1; }
