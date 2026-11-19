#!/bin/sh
# Tests for png2obc, olpc-covers-match and the olpc-covers script.
# Runs in the builder container (or anywhere with gcc and a POSIX sh); no
# network, no hardware.  Optional: OLPC_COVERS_REAL_INDEX=<dir with gb.html,
# gbc.html, nes.html> and a ROMs dir as $2 print the matches of real names.
#   sh src/olpc-covers/test.sh
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
CC=${CC:-gcc}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0; n=0
bad() { echo "  FAIL: $*"; fail=1; }
ok() { n=$((n + 1)); }
eq() { ok; [ "$2" = "$3" ] || bad "$1: got '$2', want '$3'"; }

CF="-O2 -march=geode -mtune=geode -Wall -Wextra"
$CC $CF -static -o "$tmp/png2obc" "$here/png2obc.c" || exit 1
$CC $CF -static -o "$tmp/olpc-covers-match" "$here/olpc-covers-match.c" || exit 1
$CC -O1 -o "$tmp/t_gen" "$here/t_gen.c" || exit 1
P2O=$tmp/png2obc; M=$tmp/olpc-covers-match; G=$tmp/t_gen

# ------------------------------------------------------------ png2obc
echo "== png2obc"
conv() { # type w h [maxw maxh] -> $tmp/o.obc
	t=$1; w=$2; h=$3; shift 3
	"$G" png "$t" "$w" "$h" "$tmp/i.png" || return 99
	rm -f "$tmp/o.obc"
	"$P2O" "$tmp/i.png" "$tmp/o.obc" "$@" 2>"$tmp/err"
}
px() { "$G" obc "$tmp/o.obc" "$1" "$2" | tr '\n' ' '; }

conv rgba 8 8; eq "rgba rc" $? 0
eq "rgba size (never upscaled)" "$("$G" obc "$tmp/o.obc" | tr '\n' ' ')" "8 8 "
eq "rgba opaque red" "$(px 0 0)" "8 8 PIXEL f800 "
eq "rgba transparent -> dark bg" "$(px 7 3)" "8 8 PIXEL 18c3 "
conv pal 4 4; eq "palette rc" $? 0
eq "palette red" "$(px 0 0)" "4 4 PIXEL f800 "
eq "palette green" "$(px 1 0)" "4 4 PIXEL 07e0 "
eq "palette blue" "$(px 2 0)" "4 4 PIXEL 001f "
eq "palette white" "$(px 3 0)" "4 4 PIXEL ffff "
eq "palette row 1" "$(px 3 1)" "4 4 PIXEL f800 "
conv grey 5 3; eq "grey rc" $? 0
eq "grey black" "$(px 0 2)" "5 3 PIXEL 0000 "
eq "grey 240" "$(px 4 0)" "5 3 PIXEL ef7d "
conv blue 1000 500; eq "scale rc" $? 0
eq "1000x500 -> 480x240" "$(px 100 100)" "480 240 PIXEL 001f "
conv checker 960 960; eq "checker rc" $? 0
eq "box filter 1px checker at 2:1 = mid grey" "$(px 7 9)" "480 480 PIXEL 8410 "
conv blue 481 333; eq "odd size rc" $? 0
eq "481x333 -> 480x332" "$("$G" obc "$tmp/o.obc" | tr '\n' ' ')" "480 332 "
eq "last pixel" "$(px 479 331)" "480 332 PIXEL 001f "
conv blue 200 100 100 100; eq "maxw/maxh rc" $? 0
eq "200x100 in 100x100" "$("$G" obc "$tmp/o.obc" | tr '\n' ' ')" "100 50 "
conv blue 300 300 480 480; eq "300x300 stays" "$("$G" obc "$tmp/o.obc" | tr '\n' ' ')" "300 300 "
eq "no temp file left" "$(ls -A "$tmp" | grep -c '\.tmp\.')" 0

"$G" png huge 20000 20000 "$tmp/huge.png"
rm -f "$tmp/o.obc"; "$P2O" "$tmp/huge.png" "$tmp/o.obc" 2>"$tmp/err"; eq "huge rejected (exit 3)" $? 3
[ -e "$tmp/o.obc" ] && bad "huge left an output"
"$G" png noise 200 200 "$tmp/n.png"
size=$(wc -c <"$tmp/n.png"); head -c $((size / 2)) "$tmp/n.png" >"$tmp/trunc.png"
echo old >"$tmp/o.obc"; "$P2O" "$tmp/trunc.png" "$tmp/o.obc" 2>"$tmp/err"; rc=$?
[ $rc -ne 0 ] && [ $rc -lt 5 ] && ok || bad "truncated PNG rc=$rc"
eq "truncated: message on stderr" "$(grep -c png2obc "$tmp/err")" 1
eq "truncated: old output untouched" "$(cat "$tmp/o.obc")" old
echo "not an image" >"$tmp/txt.png"; "$P2O" "$tmp/txt.png" "$tmp/o2.obc" 2>/dev/null; eq "garbage rc" $? 2
"$P2O" "$tmp/nonexistent.png" "$tmp/o2.obc" 2>/dev/null; eq "missing file rc" $? 2
"$P2O" 2>/dev/null; eq "usage rc" $? 1
"$P2O" "$tmp/n.png" "$tmp/nodir/o.obc" 2>/dev/null; eq "unwritable output rc" $? 4

# ------------------------------------------------------------ matcher
echo "== olpc-covers-match (synthetic index)"
{
	echo '<html><body><table>'
	echo '<tr><td><a href="/Nintendo%20-%20Game%20Boy/">Parent Directory</a></td></tr>'
	echo '<tr><td><a href="?C=N;O=D">Name</a></td></tr>'
	for e in "Super Mario Land (World)" "Super Mario Land (World) (Rev 1)" "Super Mario Land (World) (Beta)" \
		 "Super Mario Land 2 - 6 Golden Coins (USA, Europe)" "Tetris (Japan)" "Tetris (World)" \
		 "Legend of Zelda, The - Link's Awakening (USA, Europe)" "Pokemon - Gold Version (USA, Europe) (SGB Enhanced)" \
		 "Mario _ Luigi (USA)" "Only Beta (World) (Beta)" "Dr. Mario (World)" "Kirby (Japan)" "Kirby (USA)" \
		 "Hack &amp; Slash (World)"; do
		h=$(echo "$e" | sed 's/ /%20/g')
		echo "<tr><td><a href=\"$h.png\">$e.png</a></td><td>2025-04-09</td></tr>"
	done
	echo '</table></body></html>'
} >"$tmp/idx.html"
m() { "$M" "$tmp/idx.html" "$1"; }
eq "exact" "$(m 'Super Mario Land (World)')" "Super Mario Land (World)"
eq "Rev A -> plain" "$(m 'Super Mario Land (World) (Rev A)')" "Super Mario Land (World)"
eq "tag-free ROM, only tagged entries" "$(m 'Only Beta (World)')" "Only Beta (World) (Beta)"
eq "same base, other region -> World, not Beta" "$(m 'Super Mario Land (Europe)')" "Super Mario Land (World)"
eq "region World preferred over Japan" "$(m 'Tetris (USA)')" "Tetris (World)"
eq "same region preferred" "$(m 'Kirby (USA)')" "Kirby (USA)"
eq "same region preferred (JP)" "$(m 'Kirby (Japan) (Rev 2)')" "Kirby (Japan)"
eq "shared region token" "$(m 'Legend of Zelda, The - Link'"'"'s Awakening (USA)')" "Legend of Zelda, The - Link's Awakening (USA, Europe)"
eq "article moved" "$(m 'The Legend of Zelda - Link'"'"'s Awakening (Europe)')" "Legend of Zelda, The - Link's Awakening (USA, Europe)"
eq "punctuation/case normalised" "$(m 'dr mario (USA)')" "Dr. Mario (World)"
eq "char substitution" "$(m 'Mario & Luigi (USA)')" "Mario _ Luigi (USA)"
eq "zip extension stripped" "$(m 'Tetris (Japan).zip')" "Tetris (Japan)"
eq "no fuzzy: sequel number" "$(m 'Super Mario Land 3 (World)')" ""
eq "no fuzzy: prefix" "$(m 'Super Mario (World)')" ""
eq "no fuzzy: typo" "$(m 'Super Mario Lnad (World)')" ""
eq "no fuzzy: other game" "$(m 'Metroid (USA)')" ""
m 'Metroid (USA)' >/dev/null; eq "miss exit status 1" $? 1
m 'Tetris (Japan)' >/dev/null; eq "hit exit status 0" $? 0
printf '%s\n' 'Tetris (USA)' 'Metroid (USA)' 'Hack & Slash (World)' | "$M" -b "$tmp/idx.html" >"$tmp/b.out"
eq "batch: hit" "$(sed -n 1p "$tmp/b.out")" "$(printf 'Tetris (USA)\tTetris (World)\tTetris%%20(World).png')"
eq "batch: miss" "$(sed -n 2p "$tmp/b.out")" "$(printf 'Metroid (USA)\t\t')"
eq "batch: href entity decoded" "$(sed -n 3p "$tmp/b.out" | cut -f3)" "Hack%20&%20Slash%20(World).png"
"$M" "$tmp/nonexistent.html" x 2>/dev/null; eq "missing index exit 2" $? 2

# streaming/large index: 200k entries, memory stays small
awk 'BEGIN { print "<table>"; for (i = 0; i < 100000; i++) printf "<tr><td><a href=\"Game%%20%d%%20(USA).png\">Game %d (USA).png</a></td></tr>\n", i, i; print "</table>" }' >"$tmp/big.html"
eq "big index (100k entries) finds one" "$("$M" "$tmp/big.html" 'Game 99999 (Europe)')" "Game 99999 (USA)"

if [ -n "$OLPC_COVERS_REAL_INDEX" ] && [ -d "$2" ]; then
	echo "== real indexes ($OLPC_COVERS_REAL_INDEX) against ROM names in $2"
	for d in "$2"/*/; do
		s=$(basename "$d" | tr 'A-Z' 'a-z'); [ -f "$OLPC_COVERS_REAL_INDEX/$s.html" ] || continue
		for f in "$d"*; do
			b=$(basename "$f"); b=${b%.*}
			printf '  [%s] %s -> %s\n' "$s" "$b" "$("$M" "$OLPC_COVERS_REAL_INDEX/$s.html" "$b" || echo '(no cover)')"
		done
	done
fi

# ------------------------------------------------------------ olpc-covers script
echo "== olpc-covers (fake server, no network)"
SCRIPT=$root/rootfs/overlay/usr/local/sbin/olpc-covers
FIX=$tmp/fix; MNT=$tmp/mnt; RUN=$tmp/run; LOGF=$tmp/covers.log; REQ=$tmp/requests
GBD="Nintendo%20-%20Game%20Boy/Named_Boxarts"
GBCD="Nintendo%20-%20Game%20Boy%20Color/Named_Boxarts"
mkdir -p "$FIX/$GBD" "$FIX/$GBCD" "$MNT/ROMs/GB" "$MNT/Roms/gbc" "$MNT/ROMs/GBA" "$RUN"
mkindex() { # file entries...
	f=$1; shift
	{ echo '<table>'; for e in "$@"; do echo "<a href=\"$(echo "$e" | sed 's/ /%20/g').png\">$e.png</a>"; done; echo '</table>'; } >"$f"
}
mkindex "$FIX/$GBD/index.html" "Super Mario Land (World)" "Tetris (World)"
mkindex "$FIX/$GBCD/index.html" "Hamtaro - Ham-Hams Unite! (Europe) (En,Fr,De,Es,It)"
"$G" png rgba 300 200 "$FIX/$GBD/Super%20Mario%20Land%20(World).png"
"$G" png blue 64 64 "$FIX/$GBD/Tetris%20(World).png"
"$G" png pal 16 16 "$FIX/$GBCD/Hamtaro%20-%20Ham-Hams%20Unite!%20(Europe)%20(En,Fr,De,Es,It).png"
: >"$MNT/ROMs/GB/Super Mario Land (World) (Rev A).zip"
: >"$MNT/ROMs/GB/Tetris (World).gb"
: >"$MNT/ROMs/GB/Unknown Game (USA).zip"
: >"$MNT/ROMs/GB/.hidden.zip"
: >"$MNT/ROMs/GB/readme.txt"
: >"$MNT/ROMs/GBA/Pokemon (Spain).zip"
: >"$MNT/Roms/gbc/Hamtaro - Ham-Hams Unite! (Europe) (En,Fr,De,Es,It).zip"

# fake wget: busybox-compatible -q -T n -O dst url; serves $FIX, logs requests
cat >"$tmp/fake-wget" <<'FW'
#!/bin/sh
while [ $# -gt 0 ]; do
	case "$1" in -O) dst=$2; shift ;; -T) shift ;; -q) ;; *) url=$1 ;; esac
	shift
done
echo "$url" >>"$FAKE_REQ"
[ -n "$FAKE_DOWN" ] && exit 1
p=${url#http://fake/}
case "$p" in */) p=${p}index.html ;; esac
[ -f "$FAKE_FIX/$p" ] || exit 1
case "$p" in *.png)
	c=$(grep -c '\.png$' "$FAKE_REQ")
	if [ -n "$FAKE_GAME_AFTER" ] && [ "$c" -ge "$FAKE_GAME_AFTER" ]; then echo "$FAKE_GAME_PID" >"$FAKE_RUN/running"; fi ;;
esac
cp "$FAKE_FIX/$p" "$dst"
FW
printf '#!/bin/sh\necho wifi-up >>"$FAKE_REQ"\nexit 1\n' >"$tmp/fake-wifi"
chmod +x "$tmp/fake-wget" "$tmp/fake-wifi"
sleep 300 & GAMEPID=$!
trap 'kill $GAMEPID 2>/dev/null; rm -rf "$tmp"' EXIT

run() { # args...; sets RC
	: >"$REQ"
	env OLPC_COVERS_TEST=1 OLPC_COVERS_RUN="$RUN" OLPC_COVERS_LOG="$LOGF" OLPC_COVERS_BIN="$tmp" \
	    OLPC_COVERS_THUMBS=http://fake OLPC_COVERS_WGET="$tmp/fake-wget" OLPC_COVERS_WIFI="$tmp/fake-wifi" \
	    OLPC_COVERS_MOUNTPOINTS="$MNT" OLPC_COVERS_ROUTE="${ROUTE:-echo default via 10.0.0.1}" \
	    FAKE_REQ="$REQ" FAKE_FIX="$FIX" FAKE_RUN="$RUN" FAKE_DOWN="$FAKE_DOWN" \
	    FAKE_GAME_AFTER="$FAKE_GAME_AFTER" FAKE_GAME_PID="$GAMEPID" \
	    sh "$SCRIPT" "$@"
	RC=$?
}
st() { cat "$RUN/covers.status"; }
nreq() { grep -c "${1:-.}" "$REQ"; }
COV=$MNT/covers

# fresh fetch
run fetch; eq "fresh: rc" $RC 0
eq "fresh: status" "$(st)" "done 3 ok 1 missing 0 failed"
for f in "gb/Super Mario Land (World) (Rev A)" "gb/Tetris (World)" "gbc/Hamtaro - Ham-Hams Unite! (Europe) (En,Fr,De,Es,It)"; do
	[ -s "$COV/$f.obc" ] && ok || bad "missing $f.obc"
done
eq "fresh: cover header" "$("$G" obc "$COV/gb/Super Mario Land (World) (Rev A).obc" | tr '\n' ' ')" "300 200 "
eq "fresh: miss recorded" "$(cut -f3 "$COV/gb/.missing")" "Unknown Game (USA)"
eq "fresh: miss has epoch and date" "$(cut -f1,2 "$COV/gb/.missing" | grep -c '^[0-9]*	[0-9-]*$')" 1
[ -s "$COV/.index/gb.html" ] && [ -s "$COV/.index/gbc.html" ] && ok || bad "index not cached"
eq "fresh: requests (2 indexes + 3 covers)" "$(nreq)" 5
eq "fresh: no hidden/unsupported files covered" "$(ls -A "$COV/gb" | sort | tr '\n' '|')" ".missing|Super Mario Land (World) (Rev A).obc|Tetris (World).obc|"
eq "fresh: no PNG left in work dir" "$(ls "$RUN" | grep -c png)" 0
eq "fresh: lock released" "$([ -d "$RUN/covers.lock" ] && echo held || echo free)" free
eq "fresh: log has names" "$(grep -c 'Tetris (World): ok' "$LOGF")" 1
eq "status subcommand" "$(OLPC_COVERS_RUN="$RUN" sh "$SCRIPT" status)" "done 3 ok 1 missing 0 failed"

# second run does nothing
run fetch; eq "again: rc" $RC 0
eq "again: status" "$(st)" "done 0 ok 0 missing 0 failed"
eq "again: no requests" "$(nreq)" 0

# --force: retries the miss and refreshes the index, covers untouched
run fetch --force; eq "force: rc" $RC 0
eq "force: status" "$(st)" "done 0 ok 1 missing 0 failed"
eq "force: index refetched, miss retried (no PNG)" "$(nreq 'Named_Boxarts/$') $(nreq '\.png$')" "1 0"
eq "force: miss not duplicated" "$(grep -c . "$COV/gb/.missing")" 1
# a miss that appears on the server later is fetched and forgotten
mkindex "$FIX/$GBD/index.html" "Super Mario Land (World)" "Tetris (World)" "Unknown Game (USA)"
"$G" png grey 40 40 "$FIX/$GBD/Unknown%20Game%20(USA).png"
run fetch --force; eq "force/new: status" "$(st)" "done 1 ok 0 missing 0 failed"
eq "force/new: cover" "$([ -s "$COV/gb/Unknown Game (USA).obc" ] && echo y)" y
eq "force/new: .missing cleared" "$(grep -c . "$COV/gb/.missing")" 0
# --system limits the work
rm -f "$COV/gb/Tetris (World).obc" "$COV/gbc/"*.obc
run fetch --system gbc; eq "system: status" "$(st)" "done 1 ok 0 missing 0 failed"
eq "system: gb untouched" "$([ -e "$COV/gb/Tetris (World).obc" ] && echo y || echo n)" n
eq "system: no new index (cached)" "$(nreq 'Named_Boxarts/$')" 0

# expired index (older than 30 days) is refreshed
touch -d '2026-01-01' "$COV/.index/gb.html" 2>/dev/null || touch -t 202601010000 "$COV/.index/gb.html"
run fetch; eq "stale index: status" "$(st)" "done 1 ok 0 missing 0 failed"
eq "stale index: refetched" "$(nreq 'Game%20Boy/Named_Boxarts/$')" 1
# expired miss is retried without --force
printf '1\t1970-01-01\tNever Existed (USA)\n' >>"$COV/gb/.missing"
: >"$MNT/ROMs/GB/Never Existed (USA).zip"
run fetch; eq "expired miss retried: status" "$(st)" "done 0 ok 1 missing 0 failed"
run fetch; eq "fresh miss not retried" "$(st)" "done 0 ok 0 missing 0 failed"

# download failure: counted as failed, not recorded as a miss
rm -f "$COV/gb/Tetris (World).obc"
FAKE_DOWN=1 run fetch; eq "down: status" "$(st)" "done 0 ok 0 missing 1 failed"
eq "down: no miss recorded for Tetris" "$(grep -c Tetris "$COV/gb/.missing")" 0
run fetch; eq "recovers" "$(st)" "done 1 ok 0 missing 0 failed"

# lock held by a live process
mkdir "$RUN/covers.lock"; echo $GAMEPID >"$RUN/covers.lock/pid"
rm -f "$COV/gb/Tetris (World).obc"
run fetch; eq "lock: rc" $RC 3
eq "lock: nothing requested" "$(nreq)" 0
# stale lock (dead pid) is taken over
echo 999999 >"$RUN/covers.lock/pid"
run fetch; eq "stale lock: rc" $RC 0
eq "stale lock: fetched" "$(st)" "done 1 ok 0 missing 0 failed"

# game running: refuse
rm -f "$COV/gb/Tetris (World).obc"
echo $GAMEPID >"$RUN/running"
run fetch; eq "game: rc" $RC 4
eq "game: status" "$(st)" "game running"
eq "game: nothing requested" "$(nreq)" 0
echo 999999 >"$RUN/running"             # stale marker: no game
run fetch; eq "stale running marker ignored" "$(st)" "done 1 ok 0 missing 0 failed"
# a game that starts mid-run stops it between files
rm -f "$COV/gb/"*.obc "$COV/gbc/"*.obc "$RUN/running"
FAKE_GAME_AFTER=1 run fetch --system gb; eq "game starts: rc" $RC 4
eq "game starts: status" "$(st)" "game running"
eq "game starts: only one cover made" "$(ls "$COV/gb" | grep -c '\.obc$')" 1
eq "game starts: lock released" "$([ -d "$RUN/covers.lock" ] && echo held || echo free)" free
rm -f "$RUN/running"

# no network
rm -f "$COV/gb/"*.obc
ROUTE="echo" run fetch; eq "nonet: rc" $RC 5
eq "nonet: status" "$(st)" "no network"
eq "nonet: wifi tried, nothing downloaded" "$(nreq wifi-up) $(nreq http)" "1 0"
# nothing to do needs no network
run fetch; run fetch   # fill everything
ROUTE="echo" run fetch; eq "nonet but nothing to do: rc" $RC 0
eq "nonet but nothing to do: status" "$(st)" "done 0 ok 0 missing 0 failed"
eq "nonet but nothing to do: wifi not touched" "$(nreq wifi-up)" 0

run bogus 2>/dev/null; eq "usage rc" $RC 2

echo "$n checks"
if [ $fail = 0 ]; then echo "olpc-covers tests: OK"; else echo "olpc-covers tests: FAILED"; exit 1; fi
