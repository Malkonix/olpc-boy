#!/bin/sh
# mkinstall.sh OUT_DIR -- build the small NAND JFFS2 image used by step 5 of the write test.
# Output: OUT_DIR/install.jffs2 (written with nandwrite -p after flash_erase -j) and
#         OUT_DIR/install.sha256 (sha256sum -c list relative to the mount point).
# Same geometry as the XO-1 NAND: 128 KiB erase blocks, 2048-byte pages, little endian; no
# cleanmarkers in the image (-n): on NAND they live in the OOB and flash_erase -j writes them;
# a summary (sumtool) is added so the mount is quick.  Env: NBTOOL MKFS_JFFS2 SUMTOOL
set -eu
O=${1:?usage: mkinstall.sh OUT_DIR}
NBTOOL=${NBTOOL:-nbtool}; MKFS=${MKFS_JFFS2:-mkfs.jffs2}; SUMTOOL=${SUMTOOL:-sumtool}
ERASE=${MI_ERASE:-131072} PAGE=${MI_PAGE:-2048}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$O" "$T/tree/docs" "$T/tree/data"
echo "olpc-boy NAND install test image" >"$T/tree/README.txt"
i=0; while [ "$i" -lt 40 ]; do echo "install test file $i: $(( i * 7919 % 10007 ))" >"$T/tree/docs/f$i.txt"; i=$(( i + 1 )); done
"$NBTOOL" gen 424242 1048576 >"$T/tree/data/random.bin"
"$NBTOOL" fill 00 131072 >"$T/tree/data/zeros.bin"
i=0; while [ "$i" -lt 6 ]; do "$NBTOOL" gen "$(( 100 + i ))" "$(( 5000 + i * 4099 ))" >"$T/tree/data/r$i.bin"; i=$(( i + 1 )); done
( cd "$T/tree" && find . -type f | sort | xargs sha256sum ) >"$O/install.sha256"
"$MKFS" -r "$T/tree" -o "$T/raw.jffs2" -e "$ERASE" -s "$PAGE" -l -n -p
"$SUMTOOL" -i "$T/raw.jffs2" -o "$O/install.jffs2" -e "$ERASE" -l -n -p
[ -s "$O/install.jffs2" ] && [ $(( $(wc -c <"$O/install.jffs2") % ERASE )) = 0 ] || { echo "mkinstall: bad image size" >&2; exit 1; }
echo "mkinstall: install.jffs2 $(wc -c <"$O/install.jffs2") bytes, $(wc -l <"$O/install.sha256") files"
