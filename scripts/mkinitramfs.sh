#!/bin/bash
# Assemble out/initramfs/ (the directory tree the kernel embeds through
# CONFIG_INITRAMFS_SOURCE).  Run inside the olpc-boy-builder container:
#   docker run --rm -v $PWD:/work -w /work -e HOST_UID=$(id -u) -e HOST_GID=$(id -g) \
#       olpc-boy-builder bash scripts/mkinitramfs.sh
# No device nodes are created (devtmpfs provides them); all symlinks are relative.
set -eu

cd "$(dirname "$0")/.."
OUT=out/initramfs
BB=/bin/busybox.static

[ -x "$BB" ] || { echo "ERROR: $BB missing (run inside olpc-boy-builder)" >&2; exit 1; }
file "$BB" | grep -q 'Intel 80386' || { echo "ERROR: $BB is not i386" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT"/bin "$OUT"/sbin "$OUT"/usr/bin "$OUT"/usr/sbin \
	"$OUT"/proc "$OUT"/sys "$OUT"/dev "$OUT"/run "$OUT"/tmp "$OUT"/log "$OUT"/newroot "$OUT"/etc

install -m 0755 "$BB" "$OUT/bin/busybox"

# applet symlinks, placed like busybox --install would (bin/sbin/usr/...)
"$BB" --list-full | while read -r path; do
	case "$path" in
	bin/busybox) continue ;;
	esac
	dir=$(dirname "$path")
	mkdir -p "$OUT/$dir"
	depth=$(echo "$dir" | awk -F/ '{print NF}')
	rel=""; i=0
	while [ $i -lt "$depth" ]; do rel="../$rel"; i=$((i+1)); done
	ln -sf "${rel}bin/busybox" "$OUT/$path"
done
# applet that init needs even if --list-full omitted them
for a in sh ash mount umount findfs blkid switch_root getty sync cat; do
	[ -e "$OUT/bin/$a" ] || [ -e "$OUT/sbin/$a" ] || [ -e "$OUT/usr/bin/$a" ] || [ -e "$OUT/usr/sbin/$a" ] \
		|| ln -sf busybox "$OUT/bin/$a"
done

install -m 0755 initramfs/init "$OUT/init"
# /etc/mtab for busybox mount
ln -sf ../proc/mounts "$OUT/etc/mtab"

# all files owned by the build user so the kernel build needs no root
if [ -n "${HOST_UID:-}" ]; then
	chown -R "${HOST_UID}:${HOST_GID:-$HOST_UID}" "$OUT"
fi

echo "initramfs: $(du -sh "$OUT" | cut -f1), $(find "$OUT" -type l | wc -l) symlinks"
file "$OUT/bin/busybox"
