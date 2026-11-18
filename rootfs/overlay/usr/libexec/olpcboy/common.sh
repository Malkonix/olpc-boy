# olpc-boy shared shell helpers.  Source with:  . /usr/libexec/olpcboy/common.sh
# POSIX sh / busybox ash.  Never `set -e` in scripts that source this.

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

olpc_uptime() { cut -d' ' -f1 /proc/uptime; }

# timeline entry: tl "message"
tl() { echo "$(olpc_uptime) $*" >>"$BOOTDIR/timeline.log"; }

# cheap flush of the log partition (only the log fs, if the tool exists)
olpc_sync() { sync; }

# does the kernel cmdline contain a word?
cmdline_has() {
	for _w in $(cat /proc/cmdline); do
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
