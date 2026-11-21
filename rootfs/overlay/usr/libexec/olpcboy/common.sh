# olpc-boy shared shell helpers.  Source with:  . /usr/libexec/olpcboy/common.sh
# POSIX sh / busybox ash.  Never `set -e` in scripts that source this.
#
# Rules for every script that uses this file:
#   * never depend on the wall clock (the RTC may say 2013, or 1970): only
#     /proc/uptime is used for time;
#   * never open /dev/mtd* or /dev/mtdblock* (NAND policy), and never run an
#     un-argumented blkid/findfs: use olpc_blockdevs below;
#   * do not rely on `timeout` / long `sleep` for correctness: the first boot
#     on real hardware showed both returning at once.  Use olpc_sleep and
#     bounded, which measure time against /proc/uptime.

# Boot dir: /var/log/olpc-current is a symlink created by the initramfs
# (-> /mnt/olpclog/logs/boot-NNNN, or a directory on the root fs when the
# log partition is missing).  Fall back to logs/current, then to /var/log.
olpc_bootdir() {
	if [ -d /var/log/olpc-current ]; then
		echo /var/log/olpc-current
	elif [ -r /mnt/olpclog/logs/current ] && \
	     [ -d "/mnt/olpclog/logs/$(cat /mnt/olpclog/logs/current)" ]; then
		echo "/mnt/olpclog/logs/$(cat /mnt/olpclog/logs/current)"
	else
		mkdir -p /var/log/olpc-fallback
		echo /var/log/olpc-fallback
	fi
}

BOOTDIR=$(olpc_bootdir)
export BOOTDIR
# RAM staging area (tmpfs): bulk dumps are written here and copied to the
# stick in one go with olpc_publish.
STAGE=/run/olpcboy/stage
mkdir -p "$STAGE" 2>/dev/null

olpc_uptime() { cut -d' ' -f1 /proc/uptime; }

# _cs: sets CS to the uptime in centiseconds, without forking
_cs() {
	read -r _cs_up _ </proc/uptime
	_cs_s=${_cs_up%.*}
	_cs_f=${_cs_up#*.}
	_cs_f=${_cs_f#0}
	CS=$((_cs_s * 100 + ${_cs_f:-0}))
}

# timeline entry: tl "message"  (tiny append; the 3 s sync loop flushes it)
tl() { echo "$(olpc_uptime) $*" >>"$BOOTDIR/timeline.log"; }

# explicit flush of the log partition: use at stage markers only
olpc_sync() { sync; }

# copy a staged directory to the stick in one go: olpc_publish SRC DST
olpc_publish() {
	mkdir -p "$2" 2>/dev/null
	cp -R "$1"/. "$2"/ 2>/dev/null
}

# Block devices that may hold our partitions.  mtd*/mtdblock*/ram*/loop* are
# deliberately NOT listed: probing them would open the NAND.
olpc_blockdevs() {
	while read -r _bd_ma _bd_mi _bd_bl _bd_nm; do
		case "$_bd_nm" in
		sd[a-z]*|mmcblk[0-9]*|vd[a-z]*|hd[a-z]*|nvme[0-9]*) echo "/dev/$_bd_nm" ;;
		esac
	done </proc/partitions
}

# blkid restricted to olpc_blockdevs (never touches mtd)
olpc_blkid() {
	for _bk in $(olpc_blockdevs); do
		[ -b "$_bk" ] && blkid "$_bk" 2>/dev/null
	done
}

# ---- sleeping without trusting the kernel/libc combination ------------------
_SLEEPMODE=""
[ -r /run/olpcboy/sleepmode ] && read -r _SLEEPMODE </run/olpcboy/sleepmode
# olpc_sleep SECONDS   (integer, or with one decimal: 0.1)
olpc_sleep() {
	if [ -z "$_SLEEPMODE" ]; then
		_cs; _sl_a=$CS
		sleep 0.2 2>/dev/null
		_cs
		if [ $((CS - _sl_a)) -ge 15 ]; then _SLEEPMODE=sleep; else _SLEEPMODE=spin; fi
		echo "$_SLEEPMODE" >/run/olpcboy/sleepmode 2>/dev/null
	fi
	if [ "$_SLEEPMODE" = sleep ]; then sleep "$1" 2>/dev/null; return 0; fi
	# sleep returns at once on this system: wait against /proc/uptime instead
	case "$1" in
	*.*) _sl_i=${1%%.*}; _sl_t=${1#*.}; _sl_t=${_sl_t%${_sl_t#?}} ;;
	*)   _sl_i=$1; _sl_t=0 ;;
	esac
	_cs
	_sl_end=$((CS + ${_sl_i:-0} * 100 + _sl_t * 10))
	while _cs; [ "$CS" -lt "$_sl_end" ]; do :; done
	return 0
}

# bounded SECS cmd [args...]: run cmd (stdout/stderr as the caller set them);
# SIGKILL it after SECS of uptime; status 124 on expiry.  Replacement for
# `timeout`, which fired at once on the first real boot.
bounded() {
	_bd_t=$1; shift
	"$@" &
	_bd_p=$!
	_cs; _bd_end=$((CS + _bd_t * 100))
	_bd_n=0
	while :; do
		[ -r "/proc/$_bd_p/stat" ] || break
		IFS=' ' read -r _bd_a _bd_b _bd_s _ <"/proc/$_bd_p/stat" 2>/dev/null
		[ "$_bd_s" = Z ] && break
		_cs
		if [ "$CS" -ge "$_bd_end" ]; then
			kill -9 "$_bd_p" 2>/dev/null
			wait "$_bd_p" 2>/dev/null
			return 124
		fi
		_bd_n=$((_bd_n + 1))
		if [ "$_bd_n" -le 20 ]; then olpc_sleep 0.1; else olpc_sleep 0.5; fi
	done
	wait "$_bd_p"
}

# does the kernel cmdline contain a word?
cmdline_has() {
	for _w in $(cat "${OLPC_CMDLINE_FILE:-/proc/cmdline}"); do
		[ "$_w" = "$1" ] && return 0
	done
	return 1
}

# read the n-th bit of a sysfs capability bitmap.
#   cap_bit "<contents of capabilities/key>" <bit>
# Words are hex, most significant first, 32 bits each (i386 long).
cap_bit() {
	_idx=$(( $2 / 32 ))
	_bit=$(( $2 % 32 ))
	# shellcheck disable=SC2086
	set -- $1
	[ $# -gt $_idx ] || return 1
	shift $(( $# - 1 - _idx ))
	[ $(( 0x$1 >> _bit & 1 )) -eq 1 ]
}
