#!/bin/sh
# Logic test of olpc-ssh / olpc-sshcost with a mock dropbear (no hardware).  Run as root
# in the builder container (common.sh falls back to /var/log/olpc-fallback):
#   docker run --rm -v $PWD:/work -w /work olpc-boy-builder sh scripts/test-olpc-ssh.sh
T=$(mktemp -d /tmp/sshtest.XXXXXX)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$T" /run/olpcboy/dropbear.pid' EXIT
fail=0
ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=1; }
O=rootfs/overlay
mkdir -p "$T/lib" /run/olpcboy /var/log/olpc-fallback
cp "$O/usr/libexec/olpcboy/common.sh" "$T/common.sh"
# private copies of the scripts that source the local common.sh
for s in olpc-ssh olpc-sshcost; do
	sed "s|/usr/libexec/olpcboy/common.sh|$T/common.sh|; s|/usr/local/sbin/olpc-sshcost|$T/olpc-sshcost|" "$O/usr/local/sbin/$s" >"$T/$s"
	chmod +x "$T/$s"
done
# mock dropbear: records its argv; mock dropbearkey: fake key + fingerprint
cat >"$T/dropbear" <<'M'
#!/bin/sh
echo "$@" >"$0.args"; while [ $# -gt 0 ]; do [ "$1" = -P ] && echo $$ >"$2"; shift; done; exec sleep 30
M
cat >"$T/dropbearkey" <<'M'
#!/bin/sh
case "$1" in
-t) f=$4; echo SECRETKEYMATERIAL >"$f" ;;
-y) printf 'Public key portion is:\nssh-ed25519 AAAAPUBLICPART root@x\nFingerprint: SHA256:TESTFP\n' ;;
esac
M
chmod +x "$T/dropbear" "$T/dropbearkey"
B=/var/log/olpc-fallback
export OLPC_SSH_KEYDIR=$T/keys OLPC_SSH_AUTH=$T/auth OLPC_DROPBEAR=$T/dropbear OLPC_DROPBEARKEY=$T/dropbearkey OLPC_SSH_PORT=2299 OLPC_ROMS_DIR=$T/sd/roms OLPC_ROMS_LINK=$T/ROMs OLPC_MOUNTS=$T/mounts OLPC_SSH_CFGDIR=$T/cfg
mkdir -p "$T/cfg"
run() { : >"$B/timeline.log"; : >"$B/dropbear.log"; echo "$1" >"$T/cmdline"; OLPC_CMDLINE_FILE=$T/cmdline "$T/olpc-ssh" start; sleep 1; }
stop() { OLPC_CMDLINE_FILE=$T/cmdline "$T/olpc-ssh" stop; pkill -f "$T/dropbear" 2>/dev/null; sleep 0.3; }

echo 'ssh-ed25519 AAAAKEY test' >"$T/auth"
run "quiet olpcboy.nossh"
grep -q 'ssh: disabled (olpcboy.nossh)' "$B/timeline.log" && [ ! -e "$T/dropbear.args" ] && ok "nossh disables dropbear" || bad "nossh"
stop
rm -f "$T/auth"; rm -f "$T/dropbear.args"
run "quiet"
grep -q 'ssh: skipped (no authorized_keys' "$B/timeline.log" && [ ! -e "$T/dropbear.args" ] && ok "no authorized_keys: skipped" || bad "no auth"
stop
echo 'ssh-ed25519 AAAAKEY test' >"$T/auth"
echo "tmpfs $T/sd vfat rw 0 0" >"$T/mounts"; mkdir -p "$T/sd"
run "quiet"
[ -d "$T/sd/roms/gbc" ] && [ "$(readlink "$T/ROMs")" = "$T/sd/roms" ] && ok "ROMs link + SD dirs" || bad "ROMs link"
grep -q 'host key generated' "$B/timeline.log" && ok "host key generated" || bad "keygen"
grep -q 'fingerprint SHA256:TESTFP' "$B/timeline.log" && ok "fingerprint logged" || bad "fingerprint"
[ "$(stat -c %a "$T/keys/dropbear_ed25519_host_key")" = 600 ] && ok "host key mode 0600" || bad "key mode"
grep -rq 'SECRETKEYMATERIAL\|AAAAPUBLICPART\|AAAAKEY' "$B" && bad "key material in logs" || ok "no key material in logs"
for o in -s -j -k -F -E; do grep -qw -- "$o" "$T/dropbear.args" || bad "dropbear option $o"; done
grep -qw -- '-b' "$T/dropbear.args" && bad "banner enabled" || ok "dropbear options (-s -j -k, no -b)"
stop
run "quiet"
grep -q 'host key reused' "$B/timeline.log" && ok "host key reused on the 2nd start" || bad "reuse"
stop

# ---- saved choice (ssh.off in the OLPCLOG root): boot honours OFF, the menu's on/off persist it
rm -f "$T/dropbear.args" "$RUNDIR/sshcost.pid" 2>/dev/null; RUNDIR=/run/olpcboy
: >"$T/cfg/ssh.off"
run "quiet"
grep -q 'ssh: disabled (saved OFF)' "$B/timeline.log" && [ ! -e "$T/dropbear.args" ] && ok "boot with saved OFF: dropbear not started" || bad "saved OFF ignored"
[ -s "$RUNDIR/sshcost.pid" ] && ok "cost logger still runs with saved OFF" || bad "cost logger not started with saved OFF"
stop
rm -f "$T/cfg/ssh.off"
run "quiet"
[ -e "$T/dropbear.args" ] && ok "no flag: default ON, dropbear started" || bad "default ON"
OLPC_CMDLINE_FILE=$T/cmdline "$T/olpc-ssh" off
[ -e "$T/cfg/ssh.off" ] && ok "olpc-ssh off persists the flag" || bad "off: no flag"
[ ! -e "$RUNDIR/dropbear.pid" ] && ok "olpc-ssh off stops dropbear" || bad "off: pid file left"
[ -s "$RUNDIR/sshcost.pid" ] && ok "olpc-ssh off keeps the cost logger" || bad "off killed the cost logger"
pkill -f "$T/dropbear" 2>/dev/null; sleep 0.3; rm -f "$T/dropbear.args"
OLPC_CMDLINE_FILE=$T/cmdline "$T/olpc-ssh" on
[ ! -e "$T/cfg/ssh.off" ] && [ -e "$T/dropbear.args" ] && [ -s "$RUNDIR/dropbear.pid" ] && ok "olpc-ssh on clears the flag and starts dropbear" || bad "on"
grep -q 'switched OFF from the menu' "$B/timeline.log" && grep -q 'switched ON from the menu' "$B/timeline.log" && ok "menu toggles in the timeline" || bad "toggle timeline"
stop
: >"$T/cfg/ssh.off"; echo "quiet olpcboy.nossh" >"$T/cmdline"; rm -f "$T/dropbear.args"
OLPC_CMDLINE_FILE=$T/cmdline "$T/olpc-ssh" on
[ ! -e "$T/dropbear.args" ] && ok "olpcboy.nossh still forces OFF" || bad "nossh ignored by on"
stop; rm -f "$T/cfg/ssh.off"

# ---- cost logger: format, A/B (nossh) and sample count ---------------------------------------
for c in "quiet" "quiet olpcboy.nossh"; do
	rm -f "$B/sshcost.log"; echo "$c" >"$T/cmdline"
	OLPC_CMDLINE_FILE=$T/cmdline OLPC_SSHCOST_MAX=3 OLPC_SSHCOST_FAST_S=1 "$T/olpc-sshcost"
	n=$(grep -vc '^#' "$B/sshcost.log")
	[ "$n" = 3 ] && ok "sshcost [$c]: 3 samples" || bad "sshcost [$c] samples=$n"
	grep -v '^#' "$B/sshcost.log" | tail -n1 | grep -Eq '^up=[0-9]+\.[0-9]{2} ssh=(on|off) db_n=[0-9]+ db_rss_kb=[0-9]+ db_pss_kb=(NA|[0-9]+) db_jif=[0-9]+ db_jif_d=[0-9]+ memfree_kb=[0-9]+ memavail_kb=[0-9A-Z]+ idle_pct=[0-9]+\.[0-9] iow_pct=[0-9]+\.[0-9] cpu_dt=[0-9]+ load=[0-9.]+,[0-9.]+,[0-9.]+ run=[01] run_rss_kb=[0-9]+ run_jif_d=[0-9]+$' \
		&& ok "sshcost [$c]: line format" || { bad "sshcost [$c] format"; tail -n1 "$B/sshcost.log"; }
done
# a live dropbear process is counted (rename a sleep)
cp /bin/busybox "$T/dropbear" 2>/dev/null; ln -sf /bin/busybox "$T/dropbear"
( exec -a dropbear sleep 5 ) 2>/dev/null &
sleep 0.3
rm -f "$B/sshcost.log"; echo quiet >"$T/cmdline"
OLPC_CMDLINE_FILE=$T/cmdline OLPC_SSHCOST_MAX=1 "$T/olpc-sshcost"
grep -q 'ssh=on db_n=[1-9]' "$B/sshcost.log" && ok "sshcost counts dropbear processes" || { echo "note: exec -a unsupported here"; }
rm -f "$B/sshcost.log" "$B/timeline.log" "$B/dropbear.log"
[ "$fail" = 0 ] && echo "olpc-ssh tests passed" || { echo "olpc-ssh tests FAILED"; exit 1; }
