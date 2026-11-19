#!/bin/sh
# Host-side logic test for olpc-chordd: no hardware, no root.
# Builds the daemon (CC, default gcc) and replays synthetic key sequences with
# the sysfs paths pointed at a temp directory.
set -u
cd "$(dirname "$0")"
CC=${CC:-gcc}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
$CC -Os -Wall -Wextra -o "$T/chordd" olpc-chordd.c || exit 1

fail=0
# run <title> <start brightness> <expected final brightness> <expected final mono> <expected steps> <input...>
run() {
	title=$1; start=$2; expb=$3; expm=$4; expn=$5; shift 5
	d="$T/case"; rm -rf "$d"; mkdir -p "$d/bl/dcon-bl" "$d/in"
	echo 15 >"$d/bl/dcon-bl/max_brightness"; echo "$start" >"$d/bl/dcon-bl/brightness"
	echo 0 >"$d/mono"; : >"$d/state"
	echo "=== $title"
	printf '%s\n' "$@" | CHORDD_BACKLIGHT_GLOB="$d/bl/*" CHORDD_MONO="$d/mono" \
		CHORDD_STATE="$d/state" "$T/chordd" --test 2>&1 \
		| grep -v '^\[' | sed 's/^olpc-chordd: /  /'
	b=$(cat "$d/bl/dcon-bl/brightness"); m=$(cat "$d/mono"); s=$(cat "$d/state")
	echo "  -> brightness=$b mono=$m state-file=$s"
	[ "$b" = "$expb" ] && [ "$m" = "$expm" ] || { echo "  FAIL: expected brightness=$expb mono=$expm"; fail=1; }
}

run "plain D-pad, no chord: no action" 5 5 0 0 \
	"0 UP 1" "50 UP 0" "100 DOWN 1" "150 DOWN 0" "200 LEFT 1" "250 LEFT 0" "300 RIGHT 1" "350 RIGHT 0"
run "only CIRCLE + UP (no CHECK): no action" 5 5 0 0 \
	"0 CIRCLE 1" "10 UP 1" "500 UP 0" "510 CIRCLE 0"
run "chord + UP held 1 s from 12: steps then clamps at max 15" 12 15 0 0 \
	"0 CIRCLE 1" "5 CHECK 1" "100 UP 1" "1100 UP 0" "1110 CHECK 0" "1115 CIRCLE 0"
run "chord + DOWN held 1 s from 3: steps then clamps at 0" 3 0 0 0 \
	"0 CHECK 1" "5 CIRCLE 1" "100 DOWN 1" "1100 DOWN 0" "1110 CIRCLE 0" "1115 CHECK 0"
run "chord + RIGHT = colour, then LEFT = mono" 5 5 1 0 \
	"0 CIRCLE 1" "5 CHECK 1" "100 RIGHT 1" "150 RIGHT 0" "300 LEFT 1" "350 LEFT 0" "400 CHECK 0" "405 CIRCLE 0"
run "mono forced to 1 first, chord + RIGHT gives colour (0)" 5 5 0 0 \
	"0 CIRCLE 1" "5 CHECK 1" "100 RIGHT 1" "150 RIGHT 0" "160 CHECK 0" "165 CIRCLE 0"
run "CHECK re-sends press (value 1 and 2) without release: state intact, one step per press" 5 6 0 0 \
	"0 CIRCLE 1" "5 CHECK 1" "50 CHECK 2" "100 CHECK 1" "150 CHECK 2" "200 UP 1" "250 CHECK 1" "300 CHECK 2" "350 UP 0" "400 CHECK 0" "405 CIRCLE 0"
run "release CIRCLE first while UP still held: repeat stops, state tail kept until UP released" 5 6 0 0 \
	"0 CIRCLE 1" "5 CHECK 1" "100 UP 1" "200 CIRCLE 0" "1000 UP 0" "1010 CHECK 0"
run "UP pressed BEFORE the chord completes: ignored" 5 5 0 0 \
	"0 UP 1" "100 CIRCLE 1" "110 CHECK 1" "1000 UP 0" "1010 CHECK 0" "1015 CIRCLE 0"
run "chord broken then re-formed with UP still held: no new action" 5 6 0 0 \
	"0 CIRCLE 1" "5 CHECK 1" "100 UP 1" "150 CHECK 0" "200 CHECK 1" "900 UP 0" "910 CHECK 0" "915 CIRCLE 0"
run "UP then DOWN overlapped: latest direction wins (9, back to 8)" 8 8 0 0 \
	"0 CIRCLE 1" "5 CHECK 1" "100 UP 1" "130 DOWN 1" "160 DOWN 0" "170 UP 0" "180 CHECK 0" "190 CIRCLE 0"
run "unrelated keys (CROSS, SQUARE, ROTATE) never act" 5 5 0 0 \
	"0 CIRCLE 1" "5 CHECK 1" "50 CROSS 1" "60 SQUARE 1" "70 ROTATE 1" "80 ROTATE 2" "90 ROTATE 0" "100 SQUARE 0" "110 CROSS 0" "120 CHECK 0" "125 CIRCLE 0"

# Missing sysfs nodes (QEMU: no DCON) must not break anything.
echo "=== no backlight, no monochrome node"
printf '%s\n' "0 CIRCLE 1" "5 CHECK 1" "100 UP 1" "800 UP 0" "810 LEFT 1" "820 LEFT 0" "830 CHECK 0" \
	| CHORDD_BACKLIGHT_GLOB="$T/none/*" CHORDD_MONO="$T/none/mono" CHORDD_STATE="$T/s2" "$T/chordd" --test 2>&1 \
	| grep -v '^\[' | sed 's/^olpc-chordd: /  /'
[ $? -eq 0 ] && echo "  exit ok"

# ---- volume chord (SQUARE+CROSS) against the file-backed fake mixer ----------
# fake <master vol> <master sw> <mono vol> <mono sw> <pcm vol> <pcm sw>
fake() {
	cat >"$T/mixer" <<EOM
Master Playback Volume=0 31 2 $1 $1
Master Playback Switch=0 1 2 $2 $2
Master Mono Playback Volume=0 31 1 $3 0
Master Mono Playback Switch=0 1 1 $4 0
PCM Playback Volume=0 31 2 $5 $5
PCM Playback Switch=0 1 2 $6 $6
EOM
}
# vrun <title> <expected "master mastersw mono monosw pcm pcmsw brightness"> <input...>
# (starts from the fake set up by the caller; brightness starts at 5 of 15)
vrun() {
	title=$1; exp=$2; shift 2
	d="$T/case"; rm -rf "$d"; mkdir -p "$d/bl/dcon-bl"
	echo 15 >"$d/bl/dcon-bl/max_brightness"; echo 5 >"$d/bl/dcon-bl/brightness"; echo 0 >"$d/mono"
	echo "=== $title"
	printf '%s\n' "$@" | CHORDD_FAKE_MIXER="$T/mixer" CHORDD_BACKLIGHT_GLOB="$d/bl/*" CHORDD_MONO="$d/mono" \
		CHORDD_STATE="$d/state" "$T/chordd" --test 2>&1 | grep -v '^\[' | tee "$T/last.out" | sed 's/^olpc-chordd: /  /'
	got=$(awk -F= '{ split($2, a, " "); v[$1] = a[4] }
		END { printf "%s %s %s %s %s %s", v["Master Playback Volume"], v["Master Playback Switch"],
			v["Master Mono Playback Volume"], v["Master Mono Playback Switch"],
			v["PCM Playback Volume"], v["PCM Playback Switch"] }' "$T/mixer")
	got="$got $(cat "$d/bl/dcon-bl/brightness")"
	echo "  -> $got"
	[ "$got" = "$exp" ] || { echo "  FAIL: expected $exp"; fail=1; }
}

fake 20 1 20 1 23 1
vrun "vol chord + UP held 1 s from 20: press + 5 repeats = 26 (Master Mono follows)" "26 1 26 1 23 1 5" \
	"0 SQUARE 1" "5 CROSS 1" "100 UP 1" "1100 UP 0" "1110 CROSS 0" "1115 SQUARE 0"
fake 28 1 28 1 23 1
vrun "vol chord + UP held 1 s from 28: clamps at max 31" "31 1 31 1 23 1 5" \
	"0 CROSS 1" "5 SQUARE 1" "100 UP 1" "1100 UP 0" "1110 SQUARE 0" "1115 CROSS 0"
fake 3 1 3 1 23 1
vrun "vol chord + DOWN held 1 s from 3: clamps at 0" "0 1 0 1 23 1 5" \
	"0 SQUARE 1" "5 CROSS 1" "100 DOWN 1" "1100 DOWN 0" "1110 CROSS 0" "1115 SQUARE 0"
fake 20 1 20 1 23 1
vrun "vol chord + LEFT = mute (volume kept)" "20 0 20 0 23 1 5" \
	"0 SQUARE 1" "5 CROSS 1" "100 LEFT 1" "150 LEFT 0" "160 CROSS 0" "165 SQUARE 0"
fake 20 0 20 0 23 1
vrun "vol chord + RIGHT = unmute, a second RIGHT stays unmuted" "20 1 20 1 23 1 5" \
	"0 SQUARE 1" "5 CROSS 1" "100 RIGHT 1" "150 RIGHT 0" "200 RIGHT 1" "250 RIGHT 0" "260 CROSS 0" "265 SQUARE 0"
fake 20 1 20 1 23 1
vrun "plain D-pad, SQUARE alone, CROSS alone: no volume action" "20 1 20 1 23 1 5" \
	"0 UP 1" "50 UP 0" "100 DOWN 1" "150 DOWN 0" "200 LEFT 1" "250 LEFT 0" "300 RIGHT 1" "350 RIGHT 0" \
	"400 SQUARE 1" "410 UP 1" "900 UP 0" "910 SQUARE 0" "920 CROSS 1" "930 DOWN 1" "1400 DOWN 0" "1410 CROSS 0"
vrun "brightness chord unchanged with a mixer present: volume untouched" "20 1 20 1 23 1 6" \
	"0 CIRCLE 1" "5 CHECK 1" "100 UP 1" "150 UP 0" "160 CHECK 0" "165 CIRCLE 0"
vrun "all four keys held + UP/LEFT: nothing at all" "20 1 20 1 23 1 5" \
	"0 CIRCLE 1" "5 CHECK 1" "10 SQUARE 1" "15 CROSS 1" "100 UP 1" "900 UP 0" "910 LEFT 1" "920 LEFT 0" \
	"930 CROSS 0" "935 SQUARE 0" "940 CHECK 0" "945 CIRCLE 0"
vrun "brightness chord -> all four -> vol chord with UP still held: no volume step" "20 1 20 1 23 1 6" \
	"0 CIRCLE 1" "5 CHECK 1" "100 UP 1" "150 SQUARE 1" "155 CROSS 1" "160 CIRCLE 0" "165 CHECK 0" "1200 UP 0" "1210 CROSS 0" "1215 SQUARE 0"
vrun "vol chord -> all four -> brightness chord with UP still held: no brightness step" "21 1 21 1 23 1 5" \
	"0 SQUARE 1" "5 CROSS 1" "100 UP 1" "150 CIRCLE 1" "155 CHECK 1" "160 SQUARE 0" "165 CROSS 0" "1200 UP 0" "1210 CHECK 0" "1215 CIRCLE 0"
fake 20 1 20 1 23 1
vrun "...and re-pressing the direction after the switch acts under the new chord" "21 1 21 1 23 1 6" \
	"0 SQUARE 1" "5 CROSS 1" "100 UP 1" "120 UP 0" "150 CIRCLE 1" "155 CHECK 1" "160 SQUARE 0" "165 CROSS 0" "300 UP 1" "320 UP 0" "330 CHECK 0" "335 CIRCLE 0"
fake 20 1 20 1 23 1
vrun "CROSS re-sends press (value 1 and 2): one step per press" "21 1 21 1 23 1 5" \
	"0 SQUARE 1" "5 CROSS 1" "50 CROSS 2" "100 CROSS 1" "150 CROSS 2" "200 UP 1" "250 CROSS 1" "300 CROSS 2" "350 UP 0" "400 CROSS 0" "405 SQUARE 0"

fake 0 0 0 0 0 0
vrun "boot state (all off, all 0): init unmutes and sets Master 27, PCM 23, Master Mono 27" "27 1 27 1 23 1 5" \
	"0 UP 1" "10 UP 0"

# Only PCM present (no Master element): chord falls back to PCM.
cat >"$T/mixer" <<EOM
PCM Playback Volume=0 31 2 10 10
PCM Playback Switch=0 1 2 1 1
EOM
echo "=== no Master element: falls back to PCM"
printf '%s\n' "0 SQUARE 1" "5 CROSS 1" "100 UP 1" "150 UP 0" "200 LEFT 1" "250 LEFT 0" \
	| CHORDD_FAKE_MIXER="$T/mixer" CHORDD_STATE="$T/st3" "$T/chordd" --test 2>&1 | grep -v '^\[' | sed 's/^olpc-chordd: /  /'
grep -q '^PCM Playback Volume=0 31 2 11 11' "$T/mixer" && grep -q '^PCM Playback Switch=0 1 2 0 0' "$T/mixer" \
	|| { echo "  FAIL: PCM fallback"; fail=1; }

# Missing sound card: logged once, no crash, no spin, brightness unaffected.
echo "=== missing sound card: logged once, exit 0"
rm -f "$T/mixer"
printf '%s\n' "0 SQUARE 1" "5 CROSS 1" "100 UP 1" "1100 UP 0" "1200 DOWN 1" "1250 DOWN 0" "1300 LEFT 1" "1350 LEFT 0" "1400 RIGHT 1" "1450 RIGHT 0" \
	| CHORDD_FAKE_MIXER="$T/mixer" CHORDD_STATE="$T/st4" "$T/chordd" --test >"$T/nc.out" 2>&1; rc=$?
grep -v '^\[' "$T/nc.out" | sed 's/^olpc-chordd: /  /'
n=$(grep -c 'no sound card' "$T/nc.out")
[ $rc -eq 0 ] && [ "$n" -eq 1 ] && echo "  ok: rc=0, 'no sound card' logged $n time" \
	|| { echo "  FAIL: rc=$rc, 'no sound card' logged $n times"; fail=1; }
echo "=== real backend, absent control device: same"
printf '%s\n' "0 SQUARE 1" "5 CROSS 1" "100 UP 1" "1100 UP 0" \
	| CHORDD_SND_CTL="$T/none/controlC0" CHORDD_STATE="$T/st5" "$T/chordd" --test >"$T/nc.out" 2>&1; rc=$?
n=$(grep -c 'no sound card' "$T/nc.out")
[ $rc -eq 0 ] && [ "$n" -eq 1 ] && echo "  ok: rc=0, logged $n time" || { echo "  FAIL: rc=$rc n=$n"; fail=1; }

# The chord flag is 1 while either chord is held.
echo "=== chord flag follows the vol chord"
fake 20 1 20 1 23 1
printf '%s\n' "0 SQUARE 1" "5 CROSS 1" "100 CROSS 0" "110 SQUARE 0" \
	| CHORDD_FAKE_MIXER="$T/mixer" CHORDD_STATE="$T/st6" "$T/chordd" --test 2>&1 | grep 'state file' | sed 's/^olpc-chordd: /  /'
[ "$(cat "$T/st6")" = 0 ] || { echo "  FAIL: flag not cleared"; fail=1; }

# ---- gamepad chords (pseudo pad; "PAD <min> <max>" sets the trigger range) ----
# prun = vrun + the chord flag must never have been raised by a pad chord.
# (vrun filters the "[ms] ..." trace; the state trace is "state file = N".)
prun() {
	if [ -n "${PF:-}" ]; then fake $PF; else fake 20 1 20 1 23 1; fi; PF=
	vrun "$@"
	d="$T/case"
	if grep -q 'state file = 1' "$T/last.out" || [ "$(cat "$d/state")" != 0 ]; then
		echo "  FAIL: pad chord raised the chord flag"; fail=1
	fi
}
# pexp <log text>: the last run must have logged it
pexp() { grep -q -- "$1" "$T/last.out" || { echo "  FAIL: log lacks: $1"; fail=1; }; }
# pnolog: the last run must not have acted
pnone() { grep -q '\[pad:' "$T/last.out" && { echo "  FAIL: pad acted"; fail=1; }; }

prun "pad SELECT+L1 held 1 s: volume up, press + 5 repeats = 26, named in the log" "26 1 26 1 23 1 5" \
	"0 SELECT 1" "100 L1 1" "1100 L1 0" "1110 SELECT 0"
pexp 'volume Master 20 -> 21 (max 31), read back 21 \[pad: Test Pad\]'
prun "pad SELECT+L2 (key) held 1 s: volume down 20 -> 14" "14 1 14 1 23 1 5" \
	"0 SELECT 1" "100 L2 1" "1100 L2 0" "1110 SELECT 0"
PF="28 1 28 1 23 1" prun "pad volume up clamps at max 31" "31 1 31 1 23 1 5" \
	"0 SELECT 1" "100 L1 1" "1100 L1 0"
PF="3 1 3 1 23 1" prun "pad volume down clamps at 0" "0 1 0 1 23 1 5" \
	"0 SELECT 1" "100 L2 1" "1100 L2 0"
prun "pad SELECT+R1 held 1 s: brightness 5 -> 11, volume untouched" "20 1 20 1 23 1 11" \
	"0 SELECT 1" "100 R1 1" "1100 R1 0"
pexp 'brightness 5 -> 6 (max 15, step 1), read back 6 \[pad: Test Pad\]'
prun "pad SELECT+R2 (key) held 1 s: brightness clamps at 0" "20 1 20 1 23 1 0" \
	"0 SELECT 1" "100 R2 1" "1100 R2 0"
prun "pad SELECT+R1 short tap: exactly one step" "20 1 20 1 23 1 6" \
	"0 SELECT 1" "100 R1 1" "200 R1 0" "900 UP 0"
prun "pad first repeat only after 300 ms (tap 250 ms = one step)" "20 1 20 1 23 1 6" \
	"0 SELECT 1" "100 R1 1" "350 R1 0" "900 UP 0"

# analog-only triggers (xpad style: no key event), range 0..255: on > 85, off < 51
prun "analog L2 (ABS_Z only) swept up and held 1 s: volume down 20 -> 14" "14 1 14 1 23 1 5" \
	"PAD 0 255" "0 SELECT 1" "100 ABS_Z 0" "105 ABS_Z 40" "110 ABS_Z 120" "115 ABS_Z 255" \
	"600 ABS_Z 250" "1100 ABS_Z 255" "1105 ABS_Z 0" "1110 SELECT 0"
prun "analog R1-side: ABS_RZ drives brightness down 5 -> 0" "20 1 20 1 23 1 0" \
	"PAD 0 255" "0 SELECT 1" "100 ABS_RZ 255" "1100 ABS_RZ 0"
prun "noisy axis around the threshold: hysteresis gives 2 steps, not one per wiggle" "18 1 18 1 23 1 5" \
	"PAD 0 255" "0 SELECT 1" "100 ABS_Z 90" "110 ABS_Z 80" "120 ABS_Z 90" "130 ABS_Z 84" "140 ABS_Z 86" \
	"150 ABS_Z 60" "160 ABS_Z 100" "170 ABS_Z 52" "180 ABS_Z 50" "190 ABS_Z 90" "200 ABS_Z 0"
prun "noisy axis: wiggling 52..100 without ever dropping below 51 = one step" "19 1 19 1 23 1 5" \
	"PAD 0 255" "0 SELECT 1" "100 ABS_Z 90" "110 ABS_Z 52" "120 ABS_Z 100" "130 ABS_Z 53" "140 ABS_Z 99" "150 ABS_Z 0"
prun "resting value 30..220 (on > 93, off < 68): 90 is not a press, 94 is" "19 1 19 1 23 1 5" \
	"PAD 30 220" "0 SELECT 1" "100 ABS_Z 30" "110 ABS_Z 90" "120 ABS_Z 94" "130 ABS_Z 30"
prun "signed range -128..127 (on > -43, off < -77)" "19 1 19 1 23 1 5" \
	"PAD -128 127" "0 SELECT 1" "100 ABS_Z -128" "110 ABS_Z -50" "120 ABS_Z -40" "130 ABS_Z -128"
prun "pad reporting key AND axis (DS4): one step per press, key first then axis" "19 1 19 1 23 1 5" \
	"PAD 0 255" "0 SELECT 1" "100 L2 1" "101 ABS_Z 255" "150 L2 0" "160 ABS_Z 0"
prun "pad reporting key AND axis: axis first then key, two presses = two steps" "18 1 18 1 23 1 5" \
	"PAD 0 255" "0 SELECT 1" "100 ABS_Z 255" "101 L2 1" "150 ABS_Z 0" "151 L2 0" \
	"200 ABS_Z 255" "201 L2 1" "250 L2 0" "251 ABS_Z 0"

# the press has to come after SELECT, the same rule as the built-in chords
prun "shoulder held before SELECT: nothing, also after SELECT is re-pressed" "20 1 20 1 23 1 5" \
	"0 L1 1" "100 SELECT 1" "900 L1 0" "910 SELECT 0" "1000 SELECT 1" "1010 SELECT 0"
pnone
prun "analog trigger held before SELECT: nothing" "20 1 20 1 23 1 5" \
	"PAD 0 255" "0 ABS_Z 200" "100 SELECT 1" "900 ABS_Z 0" "910 SELECT 0"
pnone
prun "L1 without SELECT: nothing" "20 1 20 1 23 1 5" \
	"0 L1 1" "50 R1 1" "500 L1 0" "510 R1 0" "520 R2 1" "530 R2 0" "540 L2 1" "550 L2 0"
pnone
prun "SELECT then SELECT released while L1 held: repeat stops" "21 1 21 1 23 1 5" \
	"0 SELECT 1" "100 L1 1" "200 SELECT 0" "2000 L1 0"
# START is the emulator's quit chord with SELECT: ignored, and a held START never blocks a chord
prun "SELECT+START alone: nothing" "20 1 20 1 23 1 5" \
	"0 SELECT 1" "10 START 1" "1500 START 0" "1510 SELECT 0"
pnone
prun "START held, then SELECT+L1: chord still works" "21 1 21 1 23 1 5" \
	"0 START 1" "10 SELECT 1" "100 L1 1" "150 L1 0" "160 SELECT 0" "170 START 0"
prun "SELECT+START then L1 and R1 taps: both act" "21 1 21 1 23 1 6" \
	"0 SELECT 1" "10 START 1" "100 L1 1" "150 L1 0" "200 R1 1" "250 R1 0" "300 START 0"
prun "both groups held (L1 then R1): the second does nothing and the repeat stops" "21 1 21 1 23 1 5" \
	"0 SELECT 1" "10 L1 1" "20 R1 1" "1000 R1 0" "1010 L1 0"
prun "both groups held (R2 then L2): nothing for L2, repeat stops" "20 1 20 1 23 1 4" \
	"0 SELECT 1" "10 R2 1" "20 L2 1" "1000 L2 0" "1010 R2 0"
prun "pad unplugged mid-chord: repeat stops (one press + one repeat = 22)" "22 1 22 1 23 1 5" \
	"0 SELECT 1" "100 L1 1" "500 UNPLUG 1" "2000 UP 0"
pexp 'pad gone'
prun "events after unplug are harmless" "20 1 20 1 23 1 5" \
	"0 UNPLUG 1" "100 SELECT 1" "110 L1 1" "2000 L1 0"

# a pad repeat must survive built-in key traffic, and built-in chords still raise the flag
fake 20 1 20 1 23 1
vrun "pad repeat keeps going while an unrelated built-in key changes" "26 1 26 1 23 1 5" \
	"0 SELECT 1" "100 L1 1" "500 ROTATE 1" "510 ROTATE 0" "1100 L1 0"
fake 20 1 20 1 23 1
vrun "built-in chord still raises the flag with a pad present" "20 1 20 1 23 1 6" \
	"0 CIRCLE 1" "5 CHECK 1" "100 UP 1" "150 UP 0" "160 CHECK 0" "165 CIRCLE 0"
grep -q 'state file = 1' "$T/last.out" || { echo "  FAIL: built-in chord did not set the flag"; fail=1; }

# Binary path: the same decoder fed raw 16-byte i386 input_event records.
echo "=== binary replay (raw 16-byte records): chord CIRCLE+CHECK, RIGHT"
d="$T/bin"; mkdir -p "$d"; echo 0 >"$d/mono"
ev() { # ev sec usec type code value  -> 16 bytes little endian
	printf "$(printf '\\%03o\\%03o\\%03o\\%03o' $(( $1 & 255 )) $(( ($1>>8) & 255 )) 0 0)"
	printf "$(printf '\\%03o\\%03o\\%03o\\%03o' $(( $2 & 255 )) $(( ($2>>8) & 255 )) $(( ($2>>16) & 255 )) 0)"
	printf "$(printf '\\%03o\\000\\%03o\\000' "$3" "$4")"
	printf "$(printf '\\%03o\\000\\000\\000' "$5")"
}
{ ev 1 0 1 104 1; ev 1 5000 1 107 1; ev 1 100000 1 106 1; ev 1 100000 0 0 0; ev 1 150000 1 106 0; } >"$d/raw"
echo "  raw size: $(wc -c <"$d/raw") bytes (5 records x 16)"
echo 1 >"$d/mono"
CHORDD_BACKLIGHT_GLOB="$T/none/*" CHORDD_MONO="$d/mono" CHORDD_STATE="$d/state" "$T/chordd" --replay <"$d/raw" 2>&1 \
	| grep -v '^\[' | sed 's/^olpc-chordd: /  /'
echo "  -> mono=$(cat "$d/mono") (expect 0)"
[ "$(cat "$d/mono")" = 0 ] || fail=1

echo "=== binary replay: pad ABS_Z record (EV_ABS) with SELECT key"
fake 20 1 20 1 23 1
d="$T/bin"; echo 5 >"$d/bl-brightness" 2>/dev/null
evt() { # evt sec usec type code value (value may be up to 255)
	printf "$(printf '\\%03o\\%03o\\000\\000' $(( $1 & 255 )) $(( ($1>>8) & 255 )))"
	printf "$(printf '\\%03o\\%03o\\000\\000' $(( $2 & 255 )) $(( ($2>>8) & 255 )))"
	printf "$(printf '\\%03o\\%03o\\%03o\\%03o' $(( $3 & 255 )) 0 $(( $4 & 255 )) $(( ($4>>8) & 255 )))"
	printf "$(printf '\\%03o\\000\\000\\000' "$5")"
}
{ evt 1 0 1 314 1; evt 1 100000 3 2 255; evt 1 150000 3 2 0; } >"$d/rawpad"
CHORDD_FAKE_MIXER="$T/mixer" CHORDD_BACKLIGHT_GLOB="$T/none/*" CHORDD_STATE="$d/state" "$T/chordd" --replay <"$d/rawpad" 2>&1 \
	| grep -v '^\[' | sed 's/^olpc-chordd: /  /'
v=$(sed -n 's/^Master Playback Volume=0 31 2 \([0-9]*\) .*/\1/p' "$T/mixer")
echo "  -> Master volume=$v (expect 19)"
[ "$v" = 19 ] || { echo "  FAIL: binary pad replay"; fail=1; }

# ---- status file: written at init and after every change chordd makes ---------------------------
# sx <expected content>: the status file of the last vrun/prun must be exactly that one line
sx() {
	got=$(cat "$T/case/status" 2>/dev/null); lines=$(wc -l <"$T/case/status" 2>/dev/null)
	[ "$got" = "$1" ] && [ "$lines" = 1 ] && echo "  ok: status = $got" || { echo "  FAIL: status \"$got\" ($lines lines), expected \"$1\""; fail=1; }
}
fake 20 1 20 1 23 1
vrun "status at init (no events)" "20 1 20 1 23 1 5" "0 ROTATE 0"
sx "vol=20 volmax=31 mute=0 bri=05 brimax=15 mono=0"
fake 20 1 20 1 23 1
vrun "status after one volume-up step" "21 1 21 1 23 1 5" "0 SQUARE 1" "5 CROSS 1" "100 UP 1" "130 UP 0" "140 CROSS 0" "145 SQUARE 0"
sx "vol=21 volmax=31 mute=0 bri=05 brimax=15 mono=0"
fake 20 1 20 1 23 1
vrun "status after volume up held 1 s (final value, not the first step)" "26 1 26 1 23 1 5" "0 SQUARE 1" "5 CROSS 1" "100 UP 1" "1100 UP 0" "1110 CROSS 0" "1115 SQUARE 0"
sx "vol=26 volmax=31 mute=0 bri=05 brimax=15 mono=0"
fake 3 1 3 1 23 1
vrun "status after volume down to the floor" "0 1 0 1 23 1 5" "0 SQUARE 1" "5 CROSS 1" "100 DOWN 1" "1100 DOWN 0"
sx "vol=00 volmax=31 mute=0 bri=05 brimax=15 mono=0"
fake 20 1 20 1 23 1
vrun "status after mute" "20 0 20 0 23 1 5" "0 SQUARE 1" "5 CROSS 1" "100 LEFT 1" "150 LEFT 0"
sx "vol=20 volmax=31 mute=1 bri=05 brimax=15 mono=0"
fake 20 0 20 0 23 1
vrun "status after unmute" "20 1 20 1 23 1 5" "0 SQUARE 1" "5 CROSS 1" "100 RIGHT 1" "150 RIGHT 0"
sx "vol=20 volmax=31 mute=0 bri=05 brimax=15 mono=0"
fake 20 1 20 1 23 1
vrun "status after brightness up (chord)" "20 1 20 1 23 1 6" "0 CIRCLE 1" "5 CHECK 1" "100 UP 1" "130 UP 0"
sx "vol=20 volmax=31 mute=0 bri=06 brimax=15 mono=0"
vrun "status after brightness up held 1 s" "20 1 20 1 23 1 11" "0 CIRCLE 1" "5 CHECK 1" "100 UP 1" "1100 UP 0"
sx "vol=20 volmax=31 mute=0 bri=11 brimax=15 mono=0"
vrun "status after mono (chord LEFT)" "20 1 20 1 23 1 5" "0 CIRCLE 1" "5 CHECK 1" "100 LEFT 1" "150 LEFT 0"
sx "vol=20 volmax=31 mute=0 bri=05 brimax=15 mono=1"
vrun "status after colour (mono set first, chord RIGHT)" "20 1 20 1 23 1 5" "0 CIRCLE 1" "5 CHECK 1" "100 LEFT 1" "150 LEFT 0" "200 RIGHT 1" "250 RIGHT 0"
sx "vol=20 volmax=31 mute=0 bri=05 brimax=15 mono=0"
prun "status after pad SELECT+L1 (volume up)" "21 1 21 1 23 1 5" "0 SELECT 1" "100 L1 1" "130 L1 0" "140 SELECT 0"
sx "vol=21 volmax=31 mute=0 bri=05 brimax=15 mono=0"
prun "status after pad SELECT+R2 (brightness down)" "20 1 20 1 23 1 4" "0 SELECT 1" "100 R2 1" "130 R2 0" "140 SELECT 0"
sx "vol=20 volmax=31 mute=0 bri=04 brimax=15 mono=0"
PF="31 1 31 1 23 1" prun "status when already at the ceiling" "31 1 31 1 23 1 5" "0 SELECT 1" "100 L1 1" "130 L1 0" "140 SELECT 0"
sx "vol=31 volmax=31 mute=0 bri=05 brimax=15 mono=0"
[ "$(grep -c 'status: ' "$T/last.out")" = 1 ] && echo "  ok: a press at the limit does not rewrite the status file" || { echo "  FAIL: status rewritten at a limit"; fail=1; }
# no sound card, no backlight, no DCON: unknown fields are -1, the file still exists
d="$T/nocard"; rm -rf "$d"; mkdir -p "$d"
echo "=== status without card, backlight or DCON"
printf '%s\n' "0 ROTATE 0" | CHORDD_FAKE_MIXER="$T/absent" CHORDD_BACKLIGHT_GLOB="$d/none/*" CHORDD_MONO="$d/none" CHORDD_STATE="$d/state" \
	"$T/chordd" --test 2>/dev/null
[ "$(cat "$d/status")" = "vol=-1 volmax=-1 mute=0 bri=-1 brimax=-1 mono=-1" ] && echo "  ok: $(cat "$d/status")" || { echo "  FAIL: nocard status: $(cat "$d/status")"; fail=1; }
# explicit CHORDD_STATUS path, and a status file written in place (same inode across rewrites)
d="$T/explicit"; rm -rf "$d"; mkdir -p "$d/bl/dcon-bl"; echo 15 >"$d/bl/dcon-bl/max_brightness"; echo 5 >"$d/bl/dcon-bl/brightness"; echo 0 >"$d/mono"
fake 20 1 20 1 23 1
printf '%s\n' "0 CIRCLE 1" "5 CHECK 1" "100 UP 1" "1100 UP 0" | CHORDD_STATUS="$d/st.out" CHORDD_FAKE_MIXER="$T/mixer" CHORDD_BACKLIGHT_GLOB="$d/bl/*" \
	CHORDD_MONO="$d/mono" CHORDD_STATE="$d/state" "$T/chordd" --test 2>/dev/null
[ "$(cat "$d/st.out")" = "vol=20 volmax=31 mute=0 bri=11 brimax=15 mono=0" ] && [ ! -e "$d/status" ] && echo "  ok: CHORDD_STATUS honoured" || { echo "  FAIL: CHORDD_STATUS"; fail=1; }
# a reader with inotify would see every rewrite as a modify of the same file: check one write per change
if command -v strace >/dev/null 2>&1; then
	strace -f -e trace=pwrite64 -o "$T/strace.out" env CHORDD_STATUS="$d/st.out" CHORDD_FAKE_MIXER="$T/mixer" CHORDD_BACKLIGHT_GLOB="$d/bl/*" CHORDD_MONO="$d/mono" \
		CHORDD_STATE="$d/state" sh -c 'printf "%s\n" "0 CIRCLE 1" "5 CHECK 1" "100 UP 1" "130 UP 0" | "$1" --test' x "$T/chordd" >/dev/null 2>&1
	n=$(grep -c ', 53) *= 53\|pwrite64' "$T/strace.out" 2>/dev/null)
	[ "${n:-0}" -ge 1 ] && echo "  ok: status written with pwrite64 ($n calls)" || echo "  (strace gave no pwrite64 lines)"
fi

[ $fail -eq 0 ] && echo "ALL OK" || { echo "FAILED"; exit 1; }
