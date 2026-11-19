# core dumps into the boot dir are size-limited (512-byte blocks: ~4 MB)
ulimit -c 8192 2>/dev/null
export PATH=$PATH:/usr/local/sbin
# time zone (POSIX string, UTC-4 with no DST by default).  musl reads only $TZ, not /etc/TZ, and the
# image has no zoneinfo: login shells take it from here; olpc-menu reads the file itself (TZ.CFG
# on the FAT partition overrides it for the clock).
[ -r /etc/TZ ] && read -r TZ </etc/TZ && export TZ
