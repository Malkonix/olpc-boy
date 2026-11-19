#!/bin/sh
# olpc-run host tests: scaler, resampler, input mapping and diagnostics self-test, then (when
# out/cores has the cores and ROMs) full runs of every core through the real
# frontend with null sinks, a fake framebuffer file, zip loading, the clock
# pacing fallback, frame skipping and battery saves.  Run in the builder
# container (i386 userland); no framebuffer, sound card or input device needed.
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
cores=$root/out/cores
CC=${CC:-gcc}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
bad() { echo "  FAIL: $*"; fail=1; }

# The container CPU is not a Geode: build the host copies for a plain i586.
HCF="-O2 -march=i586 -Wall -Wextra"
$CC $HCF -o "$tmp/selftest" "$here/selftest.c" "$here/scale.c" "$here/resample.c" "$here/input.c" "$here/audio.c" "$here/diag.c" "$here/saver.c" -lm -pthread || exit 1
$CC $HCF -o "$tmp/fbcheck" "$here/fbcheck.c" || exit 1
$CC $HCF -o "$tmp/mkzip" "$here/mkzip.c" || exit 1
$CC $HCF -o "$tmp/olpc-run" "$here/olpc-run.c" "$here/scale.c" "$here/audio.c" "$here/input.c" "$here/resample.c" "$here/diag.c" "$here/saver.c" -ldl -pthread || exit 1

echo "== self-test (scaler, resampler, input mapping, audio plan, diagnostics)"
"$tmp/selftest" || exit 1

stat_get() { sed -n "s/^$2=//p" "$1"; }

# run_core <label> <system> <rom> <core> <source WxH the core outputs> [max scale]
# The expected rectangle is computed here, independently of the frontend.
run_core() {
	label=$1; sys=$2; rom=$3; core=$4; sw=${5%x*}; sh=${5#*x}; cap=${6:-0}
	k=$((1200 / sw)); [ $((900 / sh)) -lt $k ] && k=$((900 / sh)); [ "$cap" -gt 0 ] && [ $k -gt $cap ] && k=$cap
	w=$((sw * k)); h=$((sh * k)); x0=$(((1200 - w) / 2)); y0=$(((900 - h) / 2))
	echo "== $label ($core, $(basename "$rom"), source ${sw}x${sh}, scale ${k}x)"
	# a) null sinks
	"$tmp/olpc-run" --libdir "$cores" --core "$core" --max-scale "$cap" --no-video --no-audio --frames 600 \
		--stats "$tmp/s.txt" --savedir "$tmp/saves" --input-dir "$tmp/noinput" "$sys" "$rom" >"$tmp/log" 2>&1
	rc=$?
	[ $rc = 0 ] || { bad "null-sink run rc=$rc"; tail -n 5 "$tmp/log"; }
	[ "$(stat_get "$tmp/s.txt" frames_run)" = 600 ] || bad "frames_run != 600"
	printf '  null sinks:  rc=%s frames=%s core_ms_avg=%s (host speed)\n' $rc "$(stat_get "$tmp/s.txt" frames_run)" "$(stat_get "$tmp/s.txt" core_ms_avg)"
	# b) fake framebuffer: the whole video path
	rm -f "$tmp/fb.raw"
	OLPC_RUN_FAKEFB="$tmp/fb.raw" "$tmp/olpc-run" --libdir "$cores" --core "$core" --max-scale "$cap" --no-audio --frames 600 \
		--stats "$tmp/s.txt" --savedir "$tmp/saves" --input-dir "$tmp/noinput" "$sys" "$rom" >"$tmp/log" 2>&1
	rc=$?
	[ $rc = 0 ] || { bad "fake-fb run rc=$rc"; tail -n 5 "$tmp/log"; }
	grep 'olpc-run: video: source' "$tmp/log" | sed 's/^/  /'
	res=$("$tmp/fbcheck" "$tmp/fb.raw" "$x0" "$y0" "$w" "$h")
	inside=${res#inside=}; inside=${inside%% *}; outside=${res##*outside=}
	echo "  fake fb: $res expected rect ${w}x${h}+${x0}+${y0} (of $((w*h)) pixels)"
	[ "${inside:-0}" -gt 0 ] || bad "no non-black pixel inside the rectangle"
	[ "${outside:-1}" = 0 ] || bad "$outside non-black pixels outside the rectangle"
	grep -q "at $x0,$y0\$" "$tmp/log" || bad "log does not report the expected position $x0,$y0"
	echo "  stats: drawn=$(stat_get "$tmp/s.txt" frames_drawn) skipped=$(stat_get "$tmp/s.txt" frames_skipped) dupe=$(stat_get "$tmp/s.txt" frames_dupe) rows_drawn_pct=$(stat_get "$tmp/s.txt" rows_drawn_pct) blit_ms_avg=$(stat_get "$tmp/s.txt" blit_ms_avg) (host speed)"
	cp "$tmp/fb.raw" "$tmp/fb-$label.raw"
}

# zip_case <system> <rom> <core>: archive with a name full of glob characters
zip_case() {
	echo "== zip loading ($1)"
	"$tmp/mkzip" "$tmp/z.zip" "Test Game [!] (World) *?.${2##*.}" "$2" || { bad "mkzip"; return; }
	"$tmp/olpc-run" --libdir "$cores" --core "$3" --no-video --no-audio --frames 60 --stats "$tmp/s.txt" \
		--savedir "$tmp/saves" --input-dir "$tmp/noinput" "$1" "$tmp/z.zip" >"$tmp/log" 2>&1
	rc=$?
	grep 'rom: zip entry' "$tmp/log" | sed 's/^/  /'
	[ $rc = 0 ] && grep -q 'rom: zip entry' "$tmp/log" && [ "$(stat_get "$tmp/s.txt" frames_run)" = 60 ] || { bad "zip run rc=$rc"; tail -n 5 "$tmp/log"; }
}

found=0
firstfile() { find "$1" -type f -iname "$2" 2>/dev/null | sort | head -n 1; }
for d in "$cores/test-roms" "$cores/real-roms"; do
	[ -d "$d" ] || continue
	gb=$(firstfile "$d" '*.gb'); gbc=$(firstfile "$d" '*.gbc'); nes=$(firstfile "$d" '*.nes')
	tag=$(basename "$d")
	if [ -f "$cores/gambatte_libretro.so" ]; then
		[ -n "$gb" ] && { run_core "$tag-gb" gb "$gb" gambatte 160x144; found=1; }
		[ -n "$gbc" ] && { run_core "$tag-gbc" gbc "$gbc" gambatte 160x144; found=1; }
	fi
	if [ -n "$nes" ]; then
		[ -f "$cores/quicknes_libretro.so" ] && { run_core "$tag-nes-quicknes" nes "$nes" quicknes 240x224; run_core "$tag-nes-quicknes-3x" nes "$nes" quicknes 240x224 3; found=1; }
		[ -f "$cores/fceumm_libretro.so" ] && { run_core "$tag-nes-fceumm" nes "$nes" fceumm 256x240; found=1; }
	fi
done
[ $found = 1 ] || echo "== core runs: SKIPPED (no cores/ROMs under $cores)"

if [ $found = 1 ]; then
	gb=$(firstfile "$cores/test-roms" '*.gb'); [ -n "$gb" ] || gb=$(firstfile "$cores/real-roms" '*.gb')
	nes=$(firstfile "$cores/test-roms" '*.nes'); [ -n "$nes" ] || nes=$(firstfile "$cores/real-roms" '*.nes')
	[ -n "$gb" ] && zip_case gb "$gb" gambatte
	[ -n "$nes" ] && zip_case nes "$nes" quicknes

	if [ -n "$gb" ]; then
		echo "== clock pacing fallback (no sound card): 120 frames must take about 2 s"
		"$tmp/olpc-run" --libdir "$cores" --no-video --audio-dev /nonexistent/pcm --frames 120 --stats "$tmp/s.txt" \
			--savedir "$tmp/saves" --input-dir "$tmp/noinput" gb "$gb" >"$tmp/log" 2>&1
		fps=$(stat_get "$tmp/s.txt" fps_effective); mode=$(stat_get "$tmp/s.txt" audio_mode)
		echo "  audio_mode=$mode fps_effective=$fps wall_s=$(stat_get "$tmp/s.txt" wall_s)"
		[ "$mode" = clock ] || bad "expected clock mode"
		awk -v f="$fps" 'BEGIN{exit !(f>=45 && f<=75)}' || bad "paced fps $fps outside 45..75"

		echo "== statistics keys, audio options, running marker (clock mode)"
		"$tmp/olpc-run" --libdir "$cores" --no-video --audio-dev /nonexistent/pcm --frames 120 --audio-period-ms 8 --audio-latency-ms 60 \
			--running-file "$tmp/running" --stats "$tmp/s.txt" --savedir "$tmp/saves" --input-dir "$tmp/noinput" gb "$gb" >"$tmp/log" 2>&1 &
		rpid=$!; n=0; seen=0
		while [ $n -lt 40 ] && kill -0 $rpid 2>/dev/null; do
			[ -s "$tmp/running" ] && { seen=1; [ "$(cat "$tmp/running")" = "$rpid" ] || seen=2; break; }
			sleep 0.05; n=$((n + 1))
		done
		wait $rpid
		[ $seen = 1 ] || bad "running marker not created with the run's pid (seen=$seen)"
		[ -e "$tmp/running" ] && bad "running marker left behind after exit"
		for k in frame_hist_lt14 frame_hist_14_18 frame_hist_18_22 frame_hist_22_30 frame_hist_30_40 frame_hist_gt40 \
			core_over12_frames blit_over12_frames audio_wait_over12_frames audio_fill_min audio_fill_max \
			audio_req_period_ms audio_req_latency_ms audio_period_ms audio_target_ms audio_avail_min audio_start_threshold rt mlockall \
			underruns_poll underruns_write underrun_recover_failed fill_query rt_policy rt_errno xrun_debug_req xrun_debug_applied xrun_debug_errno xrun_debug_path \
			recover_prepare recover_reopen recover_clock incidents incidents_recovered test_stall_ms test_stalls_done saves_unfinished saves_written xrun_events_total xrun_events_kept; do
			grep -q "^$k=" "$tmp/s.txt" || bad "stats lack $k"
		done
		hsum=0; for k in lt14 14_18 18_22 22_30 30_40 gt40; do hsum=$((hsum + $(stat_get "$tmp/s.txt" frame_hist_$k))); done
		[ "$hsum" = 120 ] || bad "histogram sums to $hsum, not 120"
		[ "$(stat_get "$tmp/s.txt" audio_req_period_ms)" = 8.00 ] && [ "$(stat_get "$tmp/s.txt" audio_req_latency_ms)" = 60.0 ] || bad "requested audio times not reported"
		[ "$(stat_get "$tmp/s.txt" rt)" = n/a ] || [ "$(stat_get "$tmp/s.txt" rt)" = no-rt ] || bad "SCHED_FIFO used without a sound card: rt=$(stat_get "$tmp/s.txt" rt)"
		echo "  hist: $(grep '^frame_hist' "$tmp/s.txt" | tr '\n' ' ') rt=$(stat_get "$tmp/s.txt" rt) mlockall=$(stat_get "$tmp/s.txt" mlockall)"
		"$tmp/olpc-run" --libdir "$cores" --no-video --no-audio --no-rt --frames 30 --stats "$tmp/s.txt" --savedir "$tmp/saves" \
			--input-dir "$tmp/noinput" gb "$gb" >"$tmp/log" 2>&1
		[ "$(stat_get "$tmp/s.txt" rt)" = no-rt ] || bad "--no-rt not honoured"

		echo "== --no-fill-query and --xrun-debug options (clock mode)"
		"$tmp/olpc-run" --libdir "$cores" --no-video --audio-dev /nonexistent/pcm --frames 60 --no-fill-query --xrun-debug 1 \
			--stats "$tmp/s.txt" --savedir "$tmp/saves" --input-dir "$tmp/noinput" gb "$gb" >"$tmp/log" 2>&1
		rc=$?
		[ $rc = 0 ] || bad "--no-fill-query run rc=$rc"
		[ "$(stat_get "$tmp/s.txt" fill_query)" = 0 ] && [ "$(stat_get "$tmp/s.txt" audio_fill_min)" = -1 ] \
			&& [ "$(stat_get "$tmp/s.txt" audio_fill_max)" = -1 ] || bad "fill statistics not -1 with --no-fill-query"
		[ "$(stat_get "$tmp/s.txt" xrun_debug_req)" = 1 ] || bad "xrun_debug_req not reported"
		"$tmp/olpc-run" --libdir "$cores" --no-video --no-audio --frames 5 --xrun-debug abc gb "$gb" >"$tmp/log" 2>&1
		[ $? = 2 ] || bad "--xrun-debug abc must be refused with rc 2"
		"$tmp/olpc-run" --libdir "$cores" --no-video --no-audio --frames 5 --xrun-debug 99 gb "$gb" >"$tmp/log" 2>&1
		[ $? = 2 ] || bad "--xrun-debug 99 must be refused with rc 2"

		echo "== statistics only at exit, live copy on the side (clock mode, 13 s)"
		rm -f "$tmp/s_exit.txt" "$tmp/live.txt"
		"$tmp/olpc-run" --libdir "$cores" --no-video --audio-dev /nonexistent/pcm --frames 800 --stats "$tmp/s_exit.txt" --stats-live "$tmp/live.txt" \
			--savedir "$tmp/saves" --input-dir "$tmp/noinput" gb "$gb" >"$tmp/log" 2>&1 &
		rpid=$!; sleep 11
		[ -e "$tmp/s_exit.txt" ] && bad "--stats written while the game runs (past the old 10 s period)"
		[ -s "$tmp/live.txt" ] || bad "--stats-live not written after 10 s"
		grep -q '^status=running' "$tmp/live.txt" || bad "live copy is not marked running"
		wait $rpid
		[ "$(stat_get "$tmp/s_exit.txt" status)" = frames ] || bad "no final statistics at exit"
		ls "$tmp" | grep -q 'tmp$' && bad "stats temp file left behind"
		echo "  live copy: $(stat_get "$tmp/live.txt" status) frames=$(stat_get "$tmp/live.txt" frames_run); final: $(stat_get "$tmp/s_exit.txt" status) frames=$(stat_get "$tmp/s_exit.txt" frames_run)"

		echo "== --test-stall-ms / --test-stall-at"
		"$tmp/olpc-run" --libdir "$cores" --no-video --audio-dev /nonexistent/pcm --frames 40 --test-stall-ms 60 --test-stall-at 5,10 \
			--stats "$tmp/s.txt" --savedir "$tmp/saves" --input-dir "$tmp/noinput" gb "$gb" >"$tmp/log" 2>&1
		[ "$(stat_get "$tmp/s.txt" test_stalls_done)" = 2 ] && [ "$(stat_get "$tmp/s.txt" test_stall_ms)" = 60 ] || bad "stalls not recorded: $(stat_get "$tmp/s.txt" test_stalls_done)"
		"$tmp/olpc-run" --libdir "$cores" --no-video --no-audio --frames 5 --test-stall-at 3,x gb "$gb" >"$tmp/log" 2>&1
		[ $? = 2 ] || bad "--test-stall-at 3,x must be refused with rc 2"
		"$tmp/olpc-run" --libdir "$cores" --no-video --no-audio --frames 5 --test-stall-ms abc gb "$gb" >"$tmp/log" 2>&1
		[ $? = 2 ] || bad "--test-stall-ms abc must be refused with rc 2"

		echo "== frame skipping: blit forced to 30 ms behind the clock"
		"$tmp/olpc-run" --libdir "$cores" --no-video --audio-dev /nonexistent/pcm --frames 200 --max-skip 4 \
			--test-blit-delay-ms 30 --stats "$tmp/s.txt" --savedir "$tmp/saves" --input-dir "$tmp/noinput" gb "$gb" >"$tmp/log" 2>&1
		sk=$(stat_get "$tmp/s.txt" frames_skipped); dr=$(stat_get "$tmp/s.txt" frames_drawn)
		echo "  frames_run=$(stat_get "$tmp/s.txt" frames_run) drawn=$dr skipped=$sk fps_effective=$(stat_get "$tmp/s.txt" fps_effective)"
		[ "${sk:-0}" -gt 0 ] || bad "no frame was skipped while 30 ms behind"
		[ "${dr:-0}" -ge 40 ] || bad "too few frames drawn ($dr): max-skip 4 not honoured"
	fi

	# battery saves: needs a ROM whose core exposes save RAM (the real GBC ROM, if present)
	sram=$(firstfile "$cores/real-roms" '*.gbc'); [ -n "$sram" ] || sram=$(firstfile "$cores/real-roms" '*.gb')
	if [ -n "$sram" ]; then
		echo "== battery saves ($(basename "$sram"))"
		mkdir -p "$tmp/rr/gb" && cp "$sram" "$tmp/rr/gb/game.${sram##*.}"
		run1() { "$tmp/olpc-run" --libdir "$cores" --no-video --no-audio --frames 200 --test-poke-save "$@" \
			--input-dir "$tmp/noinput" gbc "$tmp/rr/gb/game.${sram##*.}" >"$tmp/log" 2>&1; }
		run1; rc=$?
		grep -E 'saves:' "$tmp/log" | sed 's/^/  /'
		if grep -q 'saves: no .* yet' "$tmp/log"; then
			f="$tmp/rr/saves/game.srm"
			if [ -f "$f" ]; then
				sz=$(wc -c <"$f"); echo "  default savedir (<romdir>/../saves) wrote game.srm, $sz bytes"
				ls "$tmp/rr/saves" | grep -q '\.tmp$' && bad "temp file left behind"
				run1; grep -E 'saves: loaded' "$tmp/log" | sed 's/^/  2nd run: /' || bad "2nd run did not load the save"
				grep -q 'saves: loaded' "$tmp/log" || bad "2nd run did not load the save"
			else bad "no save written to $f"; fi
			echo "== save helper thread: a 3 s write must not stall the loop (clock mode, 5 s)"
			rm -rf "$tmp/rr2"; mkdir -p "$tmp/rr2/gb" && cp "$sram" "$tmp/rr2/gb/game.${sram##*.}"
			"$tmp/olpc-run" --libdir "$cores" --no-video --audio-dev /nonexistent/pcm --frames 300 --test-poke-save --test-save-period-s 1 \
				--test-save-delay-ms 3000 --stats "$tmp/s.txt" --input-dir "$tmp/noinput" gbc "$tmp/rr2/gb/game.${sram##*.}" >"$tmp/log" 2>&1
			grep -E 'saves:' "$tmp/log" | sed 's/^/  /'
			grep -q 'saves: wrote .*(periodic)' "$tmp/log" || bad "no periodic save"
			[ -f "$tmp/rr2/saves/game.srm" ] || bad "periodic save did not produce the file"
			ls "$tmp/rr2/saves" | grep -q '\.tmp$' && bad "temp file left behind"
			fm=$(stat_get "$tmp/s.txt" frame_ms_max); fps=$(stat_get "$tmp/s.txt" fps_effective)
			echo "  frame_ms_max=$fm fps_effective=$fps saves_written=$(stat_get "$tmp/s.txt" saves_written)"
			awk -v m="$fm" -v f="$fps" 'BEGIN{exit !(m<60)}' || bad "loop stalled by the save (frame_ms_max=$fm)"
		else
			echo "  core has no save RAM for this ROM: SKIPPED"
		fi
	else
		echo "== battery saves: SKIPPED (no real ROM)"
	fi
fi

if [ $fail = 0 ]; then echo "olpc-run tests: ALL OK"; else echo "olpc-run tests: FAILED"; fi
exit $fail
