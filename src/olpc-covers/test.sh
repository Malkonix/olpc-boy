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

CF=${CF:-"-O2 -march=geode -mtune=geode -Wall -Wextra"}	# host runs: CF="-O2 -Wall -Wextra"
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
# libretro host gone (404 everywhere): only the GitHub fixtures answer
case "$url" in http://fake/api/*|http://fake/raw/*) ;; *) [ -n "$FAKE_NOLR" ] && exit 1 ;; esac
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
	    OLPC_COVERS_THUMBS=http://fake OLPC_COVERS_GH_API=http://fake/api OLPC_COVERS_GH_RAW=http://fake/raw OLPC_COVERS_WGET="$tmp/fake-wget" OLPC_COVERS_WIFI="$tmp/fake-wifi" \
	    OLPC_COVERS_MOUNTPOINTS="$MNT" OLPC_COVERS_ROUTE="${ROUTE:-echo default via 10.0.0.1}" \
	    FAKE_REQ="$REQ" FAKE_FIX="$FIX" FAKE_RUN="$RUN" FAKE_DOWN="$FAKE_DOWN" FAKE_NOLR="$FAKE_NOLR" \
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

# ---- GitHub fallback (libretro host answers nothing)
echo "== olpc-covers (GitHub fallback)"
# tree2html: escapes and filtering
cat >"$tmp/tree.json" <<'EOT'
{
  "sha": "abc",
  "tree": [
    {
      "path": "Quote \" Back \\ Amp \u0026 Caf\u00e9 \ud83c\udfae (World).png",
      "mode": "100644",
      "sha": "1"
    },
    {
      "path": "notes.txt",
      "mode": "100644"
    },
    {
      "path": "A [b] 100% #1?.png",
      "mode": "120000"
    }
  ],
  "truncated": false
}
EOT
"$M" --tree2html <"$tmp/tree.json" >"$tmp/t2h.out"
eq "tree2html: table open/close" "$(sed -n 1p "$tmp/t2h.out") $(tail -n1 "$tmp/t2h.out")" "<table> </table>"
eq "tree2html: escapes decoded and encoded" "$(sed -n 2p "$tmp/t2h.out")" '<a href="Quote%20%22%20Back%20%5C%20Amp%20%26%20Caf%C3%A9%20%F0%9F%8E%AE%20(World).png">x</a>'
eq "tree2html: special chars, symlink kept, .txt dropped" "$(sed -n 3p "$tmp/t2h.out") $(wc -l <"$tmp/t2h.out")" '<a href="A%20%5Bb%5D%20100%25%20%231%3F.png">x</a> 4'
printf '{"tree": []}\n' | "$M" --tree2html >"$tmp/t2h.out"
eq "tree2html: nothing -> empty" "$(wc -c <"$tmp/t2h.out")" 0
eq "script tree2html subcommand = helper" "$(OLPC_COVERS_TEST=1 OLPC_COVERS_BIN="$tmp" sh "$SCRIPT" tree2html <"$tmp/tree.json" | cksum)" "$("$M" --tree2html <"$tmp/tree.json" | cksum)"

# Equivalence with the awk implementation the helper replaced (reference).
cat >"$tmp/ref.awk" <<'EOT'
function init(  i) { for (i = 1; i < 128; i++) ORD[sprintf("%c", i)] = i }
function pct(b) { return sprintf("%%%02X", b) }
function enc1(c) {
	if (index("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._~()!,-", c)) return c
	if (c in ORD) return pct(ORD[c])
	return c
}
function hex4(s,   i, v, d) {
	v = 0
	for (i = 1; i <= 4; i++) {
		d = index("0123456789abcdef", tolower(substr(s, i, 1))) - 1
		if (d < 0) return -1
		v = v * 16 + d
	}
	return v
}
function utf8(cp) {
	if (cp < 128) return enc1(sprintf("%c", cp))
	if (cp < 2048) return pct(192 + int(cp / 64)) pct(128 + cp % 64)
	if (cp < 65536) return pct(224 + int(cp / 4096)) pct(128 + int(cp / 64) % 64) pct(128 + cp % 64)
	return pct(240 + int(cp / 262144)) pct(128 + int(cp / 4096) % 64) pct(128 + int(cp / 64) % 64) pct(128 + cp % 64)
}
function enc(s,   o, i) {
	for (i = 1; i <= length(s); i++) o = o enc1(substr(s, i, 1))
	return o
}
function jenc(s,   o, i, c, n, v, w) {
	i = 1
	while (i <= length(s)) {
		c = substr(s, i, 1)
		if (c != "\\") { o = o enc1(c); i++; continue }
		n = substr(s, i + 1, 1)
		if (n == "u" && (v = hex4(substr(s, i + 2, 4))) >= 0) {
			i += 6
			if (v >= 55296 && v < 56320 && substr(s, i, 2) == "\\u" && (w = hex4(substr(s, i + 2, 4))) >= 56320 && w < 57344) {
				v = 65536 + (v - 55296) * 1024 + (w - 56320); i += 6
			}
			o = o utf8(v); continue
		}
		i += 2
		if (n == "b") o = o pct(8)
		else if (n == "f") o = o pct(12)
		else if (n == "n") o = o pct(10)
		else if (n == "r") o = o pct(13)
		else if (n == "t") o = o pct(9)
		else o = o enc1(n)
	}
	return o
}

BEGIN { init() }
/^[ ]*"path": "/ {
	s = $0; sub(/^[ ]*"path": "/, "", s); sub(/",?[ \r]*$/, "", s)
	h = jenc(s)
	if (h ~ /\.png$/) { if (!n++) print "<table>"; print "<a href=\"" h "\">x</a>" }
}
END { if (n) print "</table>" }
EOT
AWK=awk; command -v busybox >/dev/null 2>&1 && AWK="busybox awk"	# the XO's awk
same() { # name file: helper and reference must agree byte for byte
	$AWK -f "$tmp/ref.awk" <"$2" >"$tmp/ref.out"; "$M" --tree2html <"$2" >"$tmp/c.out"
	cmp -s "$tmp/ref.out" "$tmp/c.out" && ok || bad "tree2html differs from awk reference: $1"
}
{ # escapes, surrogates, CRLF, trailing commas/blanks, odd lines
printf '%s\r\n' '{' '      "path": "Q \" B \\ S \/ \b\f\n\r\t é € 🎮.png",' \
 '  "path": "lone \ud83c x.png"' ' "path": "bad \u12g4 \u12" ' '"path": "no indent.png"' \
 '   "path": "trail\\.png",   ' '   "path": "nonpng.PNG",' '   "path": "unk \q \x.png"' \
 '   "path": "A.PNG.png"' '   "path": "A~ÿ.png"' '   "path": "back at end\\",' \
 '   "path": "x.png\"",' '   "path": "x.png" junk",' '   "path": "x.png"  ' '   "path": "\ud83cA.png"' \
 '   "path": "\udc00\ud83c.png"' '   "path": "\u0000x.png"' '   "path": "\ud83c\udc' '   "path": "🀀' \
 '   "path": "short\u12'
printf '\t"path": "tab indent.png"\n'
# raw UTF-8 only: a unicode-aware busybox awk (Alpine) drops lines with invalid
# UTF-8 (\377), which JSON from GitHub never contains; the helper passes bytes through
printf '   "path": "raw \303\251 \001 q.png"\n   "path": "no newline at end.png"'
} >"$tmp/edge.json"
same "edge cases" "$tmp/edge.json"
printf '   "path": "a.txt"\n' >"$tmp/e2.json"; same "no png" "$tmp/e2.json"
: >"$tmp/e3.json"; same "empty input" "$tmp/e3.json"
$AWK 'BEGIN { srand(7); n = split("a Z 0 _ \\\\ \\\" \\u \\ud83c \\udfae \\u00e9 \\n \" , .png \\/ \\uD83D \\uDE00 \\u12 \\q %", A, " ")
	for (i = 0; i < 4000; i++) { v = ""; k = int(rand() * 9)
		for (j = 0; j < k; j++) v = v A[1 + int(rand() * n)]
		if (rand() < .5) v = v ".png"
		printf "%s\"path\": \"%s%s\n", (rand() < .3 ? "      " : "  "), v, (rand() < .5 ? "\"," : "\"") } }' >"$tmp/fuzz.json"
same "random escapes (4000 lines)" "$tmp/fuzz.json"
[ -n "$OLPC_COVERS_REAL_TREE" ] && [ -r "$OLPC_COVERS_REAL_TREE" ] && same "real tree" "$OLPC_COVERS_REAL_TREE"
cat >"$tmp/root.json" <<'EOT'
{
  "tree": [
    {
      "path": "Named_Snaps",
      "sha": "1111"
    },
    {
      "path": "Named_Boxarts",
      "mode": "040000",
      "sha": "abc123def",
      "url": "x"
    }
  ]
}
EOT
eq "tree-sha" "$("$M" --tree-sha Named_Boxarts <"$tmp/root.json")" abc123def
eq "tree-sha: absent" "$("$M" --tree-sha Nope <"$tmp/root.json" | wc -c)" 0

GR=Nintendo_-_Game_Boy
mkdir -p "$FIX/api/$GR/git/trees" "$FIX/raw/$GR/master/Named_Boxarts" "$tmp/mnt2/ROMs/GB"
RAWD=$FIX/raw/$GR/master/Named_Boxarts
cat >"$FIX/api/$GR/git/trees/master" <<EOT
{
  "sha": "r00t",
  "tree": [
    {
      "path": "Named_Boards",
      "mode": "040000",
      "type": "tree",
      "sha": "0000000000000000000000000000000000000000"
    },
    {
      "path": "Named_Boxarts",
      "mode": "040000",
      "type": "tree",
      "sha": "1111111111111111111111111111111111111111"
    }
  ]
}
EOT
cat >"$FIX/api/$GR/git/trees/1111111111111111111111111111111111111111" <<'EOT'
{
  "tree": [
    {
      "path": "Super Mario Land (World).png",
      "mode": "100644"
    },
    {
      "path": "Tetris (World).png",
      "mode": "120000"
    },
    {
      "path": "Tetris (World) (Rev 1).png",
      "mode": "120000"
    },
    {
      "path": "Tetris (World) (Rev 2).png",
      "mode": "100644"
    },
    {
      "path": "Loop (World).png",
      "mode": "120000"
    },
    {
      "path": "Hack \u0026 Slash (World).png",
      "mode": "100644"
    }
  ],
  "truncated": false
}
EOT
"$G" png rgba 300 200 "$RAWD/Super%20Mario%20Land%20(World).png"
"$G" png blue 64 64 "$RAWD/Tetris%20(World)%20(Rev%202).png"
"$G" png pal 16 16 "$RAWD/Hack%20%26%20Slash%20(World).png"
printf 'Tetris (World) (Rev 1).png' >"$RAWD/Tetris%20(World).png"          # symlink, 2 hops
printf 'Tetris (World) (Rev 2).png' >"$RAWD/Tetris%20(World)%20(Rev%201).png"
printf 'Loop (World).png' >"$RAWD/Loop%20(World).png"                        # self-link: must give up
: >"$tmp/mnt2/ROMs/GB/Super Mario Land (World).zip"
: >"$tmp/mnt2/ROMs/GB/Tetris (World).gb"
: >"$tmp/mnt2/ROMs/GB/Hack & Slash (World).zip"
: >"$tmp/mnt2/ROMs/GB/Loop (World).gb"
MNT_SAVE=$MNT; MNT=$tmp/mnt2; COV=$MNT/covers
FAKE_NOLR=1 run fetch --system gb; eq "github: rc" $RC 0
eq "github: status (loop is a failure)" "$(st)" "done 3 ok 0 missing 1 failed"
eq "github: index source remembered" "$(cat "$COV/.index/gb.html.src")" github
eq "github: index converted" "$(grep -c '^<a href=' "$COV/.index/gb.html")" 6
eq "github: covers made" "$(ls "$COV/gb" | grep '\.obc$' | sort | tr '\n' '|')" "Hack & Slash (World).obc|Super Mario Land (World).obc|Tetris (World).obc|"
eq "github: symlink resolved to the real image" "$("$G" obc "$COV/gb/Tetris (World).obc" | tr '\n' ' ')" "64 64 "
eq "github: exactly 2 API calls" "$(nreq fake/api)" 2
eq "github: libretro tried for the index only" "$(grep -c 'fake/Nintendo' "$REQ")" 2
eq "github: no libretro image requests" "$(grep 'fake/Nintendo' "$REQ" | grep -c '\.png')" 0
eq "github: symlink hops (1 + 2 raw gets)" "$(nreq 'Tetris')" 3
eq "github: self-link gives up (3 gets, then fail)" "$(nreq 'Loop')" 3
# cached index keeps its source: no API calls, images from raw even with libretro back up
rm -f "$COV/gb/"*.obc
run fetch --system gb
eq "cached github index: no api, no libretro index" "$(nreq fake/api) $(grep -c 'Named_Boxarts/$' "$REQ")" "0 0"
eq "cached github index: images from raw" "$(nreq fake/raw)" "$(nreq .png)"
eq "cached github index: still ok" "$(st)" "done 3 ok 0 missing 1 failed"
# index refresh with libretro healthy again prefers libretro
rm -rf "$tmp/mnt3"; mkdir -p "$tmp/mnt3/ROMs/GB"; : >"$tmp/mnt3/ROMs/GB/Tetris (World).gb"
MNT=$tmp/mnt3; COV=$MNT/covers
run fetch --system gb
eq "libretro preferred when it answers" "$(cat "$COV/.index/gb.html.src") $(nreq fake/api)" "libretro 0"
# both sources dead -> failed, no index
rm -rf "$tmp/mnt3/covers"
FAKE_DOWN=1 run fetch --system gb
eq "all sources down: failed" "$(st) $([ -e "$COV/.index/gb.html" ] && echo idx || echo noidx)" "done 0 ok 0 missing 1 failed noidx"

# shared index: a fresh copy in any mount's covers dir serves all mounts
mkdir -p "$tmp/mnt4/ROMs/GB"; : >"$tmp/mnt4/ROMs/GB/Tetris (World).gb"
MNT="$tmp/mnt2 $tmp/mnt4"; COV=$tmp/mnt4/covers
run fetch --system gb
eq "shared index: reused from the other mount, no request for it" "$(nreq 'Named_Boxarts/$') $(nreq fake/api)" "0 0"
eq "shared index: not copied into the second mount" "$([ -e "$tmp/mnt4/covers/.index" ] && echo copy || echo none)" none
eq "shared index: cover stored next to its ROMs" "$([ -s "$tmp/mnt4/covers/gb/Tetris (World).obc" ] && echo y)" y
# empty first mount (nothing cached): the index is built in the first writable covers dir
rm -rf "$tmp/mnt2/covers" "$tmp/mnt4/covers"; mkdir -p "$tmp/mnt5/ROMs/GB"; : >"$tmp/mnt5/ROMs/GB/Tetris (World).gb"
MNT="$tmp/mnt4 $tmp/mnt5"
run fetch --system gb
eq "shared index: built once in the first mount" "$([ -s "$tmp/mnt4/covers/.index/gb.html" ] && echo y) $([ -e "$tmp/mnt5/covers/.index" ] && echo dup)" "y "
eq "shared index: second mount's cover made" "$([ -s "$tmp/mnt5/covers/gb/Tetris (World).obc" ] && echo y)" y
run fetch --system gb
eq "shared index: later runs need no download" "$(nreq 'Named_Boxarts/$') $(nreq fake/api)" "0 0"

# timing lines and resumable tree download
: >"$LOGF"
MNT=$tmp/mnt6; COV=$MNT/covers; mkdir -p "$MNT/ROMs/GB"; : >"$MNT/ROMs/GB/Tetris (World).gb"
FAKE_NOLR=1 run fetch --system gb
eq "log: tree download and conversion timed" "$(grep -c 'index gb: tree downloaded [0-9]* bytes in [0-9]* s' "$LOGF") $(grep -c 'index gb: converted in [0-9]* s' "$LOGF")" "1 1"
eq "log: tree json removed once converted" "$(ls -A "$COV/.index" | tr '\n' ' ')" "gb.html gb.html.src "
# a finished tree left behind by an aborted run is converted without downloading
rm -f "$COV/.index/gb.html" "$COV/.index/gb.html.src" "$COV/gb/"*.obc
cp "$FIX/api/$GR/git/trees/1111111111111111111111111111111111111111" "$COV/.index/gb.tree.json"
FAKE_NOLR=1 run fetch --system gb --force
eq "resume: no tree download" "$(nreq 'fake/api')" 0
eq "resume: index built from the kept tree" "$(grep -c '^<a href=' "$COV/.index/gb.html") $([ -e "$COV/.index/gb.tree.json" ] && echo left)" "6 "
MNT=$MNT_SAVE; COV=$MNT/covers

run bogus 2>/dev/null; eq "usage rc" $RC 2

echo "$n checks"
if [ $fail = 0 ]; then echo "olpc-covers tests: OK"; else echo "olpc-covers tests: FAILED"; exit 1; fi
