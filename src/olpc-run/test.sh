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
mkdir -p "$tmp/ovl"
trap 'rm -rf "$tmp"' EXIT
fail=0
bad() { echo "  FAIL: $*"; fail=1; }

# The container CPU is not a Geode: build the host copies for a plain i586.
HCF="-O2 -march=i586 -Wall -Wextra"
$CC $HCF -o "$tmp/selftest" "$here/selftest.c" "$here/scale.c" "$here/resample.c" "$here/input.c" "$here/audio.c" "$here/diag.c" "$here/saver.c" "$here/overlay.c" "$here/state.c" "$here/pause.c" "$here/../common/status.c" "$here/../common/gfx.c" -lm -pthread || exit 1
$CC $HCF -o "$tmp/fbcheck" "$here/fbcheck.c" || exit 1
$CC $HCF -o "$tmp/mkzip" "$here/mkzip.c" || exit 1
$CC $HCF -o "$tmp/olpc-run" "$here/olpc-run.c" "$here/scale.c" "$here/audio.c" "$here/input.c" "$here/resample.c" "$here/diag.c" "$here/saver.c" "$here/overlay.c" "$here/state.c" "$here/pause.c" "$here/../common/status.c" "$here/../common/gfx.c" -ldl -pthread || exit 1

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
	# b) fake framebuffer: the whole video path.  The in-game strip (right border) is on by default; its
	# rectangle is computed here, independently of the frontend: 96x452 at y=16, centred in the border.
	border=$((1200 - x0 - w)); sarg=""; sx=0
	if [ $border -ge 104 ]; then sx=$((x0 + w + (border - 96) / 2)); sarg="$sx 16 96 452"; fi
	rm -f "$tmp/fb.raw"
	OLPC_RUN_FAKEFB="$tmp/fb.raw" "$tmp/olpc-run" --libdir "$cores" --core "$core" --max-scale "$cap" --no-audio --frames 600 \
		--status-file "$tmp/ovl/status" --wifi-file "$tmp/ovl/wifi.status" --wireless-file "$tmp/ovl/none" \
		--stats "$tmp/s.txt" --savedir "$tmp/saves" --input-dir "$tmp/noinput" "$sys" "$rom" >"$tmp/log" 2>&1
	rc=$?
	[ $rc = 0 ] || { bad "fake-fb run rc=$rc"; tail -n 5 "$tmp/log"; }
	grep 'olpc-run: video: source' "$tmp/log" | sed 's/^/  /'
	res=$("$tmp/fbcheck" "$tmp/fb.raw" "$x0" "$y0" "$w" "$h" $sarg)
	inside=${res#inside=}; inside=${inside%% *}; outside=${res##*outside=}; outside=${outside%% *}; strip=${res##*strip=}
	echo "  fake fb: $res expected rect ${w}x${h}+${x0}+${y0} (of $((w*h)) pixels), strip rect: ${sarg:-none (border $border px)}"
	[ "${inside:-0}" -gt 0 ] || bad "no non-black pixel inside the rectangle"
	[ "${outside:-1}" = 0 ] || bad "$outside non-black pixels outside the rectangle and the strip"
	if [ -n "$sarg" ]; then [ "${strip:-0}" -gt 500 ] || bad "strip empty (strip=$strip)"; else [ "$(stat_get "$tmp/s.txt" overlay)" = 0 ] || bad "overlay reported on without room"; fi
	grep -q "at $x0,$y0\$" "$tmp/log" || bad "log does not report the expected position $x0,$y0"
	# same run with --no-overlay: the game rectangle is identical, the border stays black
	cp "$tmp/fb.raw" "$tmp/fb-ovl.raw"; cp "$tmp/s.txt" "$tmp/s-ovl.txt"
	rm -f "$tmp/fb.raw"
	OLPC_RUN_FAKEFB="$tmp/fb.raw" "$tmp/olpc-run" --libdir "$cores" --core "$core" --max-scale "$cap" --no-audio --no-overlay --frames 600 \
		--stats "$tmp/s.txt" --savedir "$tmp/saves" --input-dir "$tmp/noinput" "$sys" "$rom" >"$tmp/log" 2>&1 || bad "--no-overlay run failed"
	res2=$("$tmp/fbcheck" "$tmp/fb.raw" "$x0" "$y0" "$w" "$h")
	[ "${res2##*outside=}" = 0 ] || bad "--no-overlay: pixels outside the rectangle ($res2)"
	[ "$(stat_get "$tmp/s.txt" overlay)" = 0 ] && [ "$(stat_get "$tmp/s.txt" overlay_redraws)" = 0 ] || bad "--no-overlay: statistics report an overlay"
	d=$("$tmp/fbcheck" --diff "$tmp/fb-ovl.raw" "$tmp/fb.raw" "$x0" "$y0" "$w" "$h" $sarg)
	echo "  --no-overlay: $res2; pixels that differ with the overlay on: $d"
	case $d in "rect=0 other=0"*) ;; *) bad "the overlay changed pixels outside its strip: $d";; esac
	[ -n "$sarg" ] && [ "$(stat_get "$tmp/s-ovl.txt" overlay_redraws)" -ge 5 ] 2>/dev/null || [ -z "$sarg" ] || bad "overlay redraws missing in the statistics"
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

	if [ -n "$gb" ] && [ -f "$cores/gambatte_libretro.so" ]; then
		echo "== in-game strip: states, 12/24 h clock, changes while playing, statistics (gambatte 6x: strip at 1092,16)"
		mkdir -p "$tmp/cfg24" "$tmp/cfg12"; echo "AST4 24" >"$tmp/cfg24/TZ.CFG"
		printf 'wlan0: 0000   70.  -50.  -256 0 0 0 0 0 0\n' >"$tmp/ovl/w4"
		printf 'wlan0: 0000   30.  -80.  -256 0 0 0 0 0 0\n' >"$tmp/ovl/w1"
		SR="1092 16 96 452"; GR="120 18 960 864"
		# ovl_shot <name> <status> <wifi> <wireless file> <cfgdir> : one run, fake clock 15:07:10 AST
		ovl_shot() {
			printf '%s\n' "$2" >"$tmp/ovl/status"; printf '%s\n' "$3" >"$tmp/ovl/wifi.status"
			rm -f "$tmp/ovl-$1.raw"
			env -u TZ OLPC_RUN_FAKEFB="$tmp/ovl-$1.raw" OLPC_RUN_CFGDIR="$5" OLPC_RUN_TZFILE=/nonexistent OLPC_RUN_FAKETIME=1717268830 \
				"$tmp/olpc-run" --libdir "$cores" --core gambatte --no-audio --frames 300 --status-file "$tmp/ovl/status" \
				--wifi-file "$tmp/ovl/wifi.status" --wireless-file "$4" --stats "$tmp/ovl-$1.txt" --savedir "$tmp/saves" \
				--input-dir "$tmp/noinput" gb "$gb" >"$tmp/ovl-$1.log" 2>&1 || bad "ovl_shot $1 failed"
			[ -n "$FRAMES_DIR" ] && { mkdir -p "$FRAMES_DIR"; cp "$tmp/ovl-$1.raw" "$FRAMES_DIR/ovl-$1.raw"; }
		}
		ovl_vs() {   # <name> <base>: strips differ, nothing else does
			d=$("$tmp/fbcheck" --diff "$tmp/ovl-$2.raw" "$tmp/ovl-$1.raw" $GR $SR)
			case $d in "rect=0 other=0 strip=0") bad "$1 looks the same as $2 (strip unchanged)";; "rect=0 other=0 strip="*) echo "  $1 vs $2: $d";; *) bad "$1 vs $2 changed pixels outside the strip: $d";; esac
		}
		ST="vol=27 volmax=31 mute=0 bri=13 brimax=15 mono=0"; WF="state=connected ssid=Casa ip=10.0.0.2"
		ovl_shot base "$ST" "$WF" "$tmp/ovl/w4" "$tmp/cfg24"
		grep -q 'overlay: strip 96x452 at 1092,16' "$tmp/ovl-base.log" || bad "no strip placement in the log"
		grep -q 'overlay: time zone AST4, 24-hour clock' "$tmp/ovl-base.log" || bad "TZ.CFG not applied to the clock"
		ovl_shot h12 "$ST" "$WF" "$tmp/ovl/w4" "$tmp/cfg12";                                                  ovl_vs h12 base
		ovl_shot wifi1 "$ST" "$WF" "$tmp/ovl/w1" "$tmp/cfg24";                                                ovl_vs wifi1 base
		ovl_shot wifioff "$ST" "state=off" "$tmp/ovl/w4" "$tmp/cfg24";                                        ovl_vs wifioff base
		ovl_shot wifiwait "$ST" "state=connecting" "$tmp/ovl/w4" "$tmp/cfg24";                                ovl_vs wifiwait wifioff
		ovl_shot mute "vol=27 volmax=31 mute=1 bri=13 brimax=15 mono=0" "$WF" "$tmp/ovl/w4" "$tmp/cfg24";   ovl_vs mute base
		ovl_shot vol0 "vol=0 volmax=31 mute=0 bri=13 brimax=15 mono=0" "$WF" "$tmp/ovl/w4" "$tmp/cfg24";    ovl_vs vol0 base
		ovl_shot bri3 "vol=27 volmax=31 mute=0 bri=3 brimax=15 mono=0" "$WF" "$tmp/ovl/w4" "$tmp/cfg24";    ovl_vs bri3 base
		ovl_shot mono "vol=27 volmax=31 mute=0 bri=13 brimax=15 mono=1" "$WF" "$tmp/ovl/w4" "$tmp/cfg24";   ovl_vs mono base
		ovl_shot none "" "" "$tmp/ovl/w4" "$tmp/cfg24";                                                       ovl_vs none base
		for b in "100 0 0" "50 0 0" "15 0 0" "5 0 0" "50 0 1" "100 1 0"; do
			set -- $b
			ovl_shot "bat$1-$2$3" "$ST bat=$1 ac=$2 chg=$3" "$WF" "$tmp/ovl/w4" "$tmp/cfg24";           ovl_vs "bat$1-$2$3" base
		done
		ovl_shot batnone "$ST bat=-1 ac=-1 chg=0" "$WF" "$tmp/ovl/w4" "$tmp/cfg24"
		d=$("$tmp/fbcheck" --diff "$tmp/ovl-base.raw" "$tmp/ovl-batnone.raw" $GR $SR)
		[ "$d" = "rect=0 other=0 strip=0" ] && echo "  battery unknown looks like no battery field at all" || bad "bat=-1 differs from a status line without battery: $d"
		grep -q '^overlay=1' "$tmp/ovl-base.txt" && grep -q '^overlay_rect=96x452+1092+16' "$tmp/ovl-base.txt" || bad "overlay statistics missing"
		for k in overlay_full_paints overlay_redraws overlay_redraw_us_total overlay_redraw_us_max overlay_ticks overlay_tick_us_total overlay_tick_us_max overlay_period; do
			grep -q "^$k=" "$tmp/ovl-base.txt" || bad "stats lack $k"
		done
		echo "  base run: redraws=$(stat_get "$tmp/ovl-base.txt" overlay_redraws) total_us=$(stat_get "$tmp/ovl-base.txt" overlay_redraw_us_total) max_us=$(stat_get "$tmp/ovl-base.txt" overlay_redraw_us_max) ticks=$(stat_get "$tmp/ovl-base.txt" overlay_ticks) (host speed)"
		[ "$(stat_get "$tmp/ovl-base.txt" overlay_redraws)" = 6 ] || bad "a run with constant values must redraw each of the 6 slots once (got $(stat_get "$tmp/ovl-base.txt" overlay_redraws))"

		# values change while playing: only the strip is repainted, the result equals a run that had the final values all along
		printf '%s\n' "$ST" >"$tmp/ovl/status"; printf '%s\n' "$WF" >"$tmp/ovl/wifi.status"
		rm -f "$tmp/ovl-live.raw"
		( sleep 0.7
		  printf '%s\n' "vol=27 volmax=31 mute=1 bri=13 brimax=15 mono=1" >"$tmp/ovl/status.new"; mv "$tmp/ovl/status.new" "$tmp/ovl/status"
		  printf 'state=off\n' >"$tmp/ovl/wifi.status.new"; mv "$tmp/ovl/wifi.status.new" "$tmp/ovl/wifi.status" ) &
		env -u TZ OLPC_RUN_FAKEFB="$tmp/ovl-live.raw" OLPC_RUN_CFGDIR="$tmp/cfg24" OLPC_RUN_TZFILE=/nonexistent OLPC_RUN_FAKETIME=1717268830 \
			"$tmp/olpc-run" --libdir "$cores" --core gambatte --audio-dev /nonexistent/pcm --frames 120 --overlay-period 6 \
			--status-file "$tmp/ovl/status" --wifi-file "$tmp/ovl/wifi.status" --wireless-file "$tmp/ovl/w4" --stats "$tmp/ovl-live.txt" \
			--savedir "$tmp/saves" --input-dir "$tmp/noinput" gb "$gb" >"$tmp/ovl-live.log" 2>&1 || bad "live run failed"
		wait
		printf '%s\n' "vol=27 volmax=31 mute=1 bri=13 brimax=15 mono=1" >"$tmp/ovl/status"; printf 'state=off\n' >"$tmp/ovl/wifi.status"
		rm -f "$tmp/ovl-ref.raw"
		env -u TZ OLPC_RUN_FAKEFB="$tmp/ovl-ref.raw" OLPC_RUN_CFGDIR="$tmp/cfg24" OLPC_RUN_TZFILE=/nonexistent OLPC_RUN_FAKETIME=1717268830 \
			"$tmp/olpc-run" --libdir "$cores" --core gambatte --audio-dev /nonexistent/pcm --frames 120 --overlay-period 6 \
			--status-file "$tmp/ovl/status" --wifi-file "$tmp/ovl/wifi.status" --wireless-file "$tmp/ovl/w4" --stats "$tmp/ovl-ref.txt" \
			--savedir "$tmp/saves" --input-dir "$tmp/noinput" gb "$gb" >"$tmp/ovl-ref.log" 2>&1 || bad "reference run failed"
		rd=$(stat_get "$tmp/ovl-live.txt" overlay_redraws)
		d=$("$tmp/fbcheck" --diff "$tmp/ovl-live.raw" "$tmp/ovl-ref.raw" $GR $SR)
		echo "  live changes: redraws=$rd (6 initial + 3 changed slots), final screen vs a run that had the final values all along: $d"
		[ "$rd" = 9 ] || bad "expected 9 slot redraws, got $rd"
		[ "$d" = "rect=0 other=0 strip=0" ] || bad "the strip after the change differs from the reference: $d"
		grep -q '^overlay_ticks=[1-9]' "$tmp/ovl-live.txt" || bad "no polls recorded"
		echo "  1 s polling default: overlay_period=$(stat_get "$tmp/ovl-base.txt" overlay_period)"
		[ "$(stat_get "$tmp/ovl-base.txt" overlay_period)" = 60 ] || bad "default poll period is not 60 frames"
	fi

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
			recover_prepare recover_reopen recover_clock incidents incidents_recovered test_stall_ms test_stalls_done saves_unfinished saves_written states_saved states_loaded pause_count xrun_events_total xrun_events_kept; do
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


# pause menu and save states, driven by --test-keys (synthetic key events through pipes)
K=""
# kt <b|p> <key> <ms>: press at ms, release 80 ms later
kt() { K="$K$3:$1:$2:1,$(($3 + 80)):$1:$2:0,"; }
pm_run() { # <sys> <rom> <core> <statedir> <stats> [more olpc-run args...]
	psys=$1; prom=$2; pcore=$3; pst=$4; pstats=$5; shift 5
	"$tmp/olpc-run" --libdir "$cores" --core "$pcore" --no-audio --max-seconds 30 --statedir "$pst" --savedir "$tmp/saves" \
		--input-dir "$tmp/noinput" --test-keys "${K%,}" --stats "$pstats" "$@" "$psys" "$prom" >"$tmp/log" 2>&1
}

pause_suite() { # <sys> <rom> <core>
	sys=$1; rom=$2; core=$3
	echo "== pause menu + save states ($core, $(basename "$rom"))"
	# a) holding rotate still quits and never opens the menu
	K="300:b:rot:1,"; pm_run $sys "$rom" $core "$tmp/st-a" "$tmp/s.txt" --no-video
	[ "$(stat_get "$tmp/s.txt" status)" = exit-button ] && [ "$(stat_get "$tmp/s.txt" pause_count)" = 0 ] || bad "rotate held: status=$(stat_get "$tmp/s.txt" status) pause_count=$(stat_get "$tmp/s.txt" pause_count)"
	# b) a short rotate tap opens the menu; Quit game leaves
	K=""; kt b rot 300; kt b down 450; kt b down 610; kt b down 770; kt b down 930; kt b a 1090
	pm_run $sys "$rom" $core "$tmp/st-b" "$tmp/s.txt" --no-video
	[ "$(stat_get "$tmp/s.txt" status)" = menu-quit ] && [ "$(stat_get "$tmp/s.txt" pause_count)" = 1 ] || bad "tap + Quit: status=$(stat_get "$tmp/s.txt" status) pause_count=$(stat_get "$tmp/s.txt" pause_count)"
	[ "$(stat_get "$tmp/s.txt" states_saved)" = 0 ] && [ "$(stat_get "$tmp/s.txt" states_loaded)" = 0 ] || bad "tap + Quit changed states"
	# c) pad: Select+Start tap opens, a second tap resumes, holding quits
	K="300:p:select:1,340:p:start:1,420:p:start:0,460:p:select:0,700:p:select:1,740:p:start:1,820:p:start:0,860:p:select:0,1200:p:select:1,1240:p:start:1,"
	pm_run $sys "$rom" $core "$tmp/st-c" "$tmp/s.txt" --no-video
	[ "$(stat_get "$tmp/s.txt" status)" = exit-button ] && [ "$(stat_get "$tmp/s.txt" pause_count)" = 1 ] || bad "pad: status=$(stat_get "$tmp/s.txt" status) pause_count=$(stat_get "$tmp/s.txt" pause_count)"
	# d) save slot 1, later save slot 2, load slot 1, save it as slot 3: 3 must equal 1, 2 must differ
	st="$tmp/st-d"; rm -rf "$st"
	K=""; kt b rot 300; kt b down 450; kt b a 610; kt b b 770
	kt b rot 1300; kt b right 1450; kt b down 1610; kt b a 1770; kt b left 1930; kt b down 2090; kt b a 2250
	kt b right 2410; kt b right 2570; kt b up 2730; kt b a 2890; kt b down 3050; kt b down 3210; kt b down 3370; kt b a 3530
	pm_run $sys "$rom" $core "$st" "$tmp/s.txt" --no-video
	b=$(basename "$rom"); b=${b%.*}
	echo "  round trip: status=$(stat_get "$tmp/s.txt" status) saved=$(stat_get "$tmp/s.txt" states_saved) loaded=$(stat_get "$tmp/s.txt" states_loaded) pauses=$(stat_get "$tmp/s.txt" pause_count); $(ls "$st" | tr '\n' ' ')"
	[ "$(stat_get "$tmp/s.txt" status)" = menu-quit ] && [ "$(stat_get "$tmp/s.txt" states_saved)" = 3 ] && [ "$(stat_get "$tmp/s.txt" states_loaded)" = 1 ] && [ "$(stat_get "$tmp/s.txt" pause_count)" = 2 ] || { bad "round trip counters"; tail -n 8 "$tmp/log"; }
	[ -s "$st/$b.st1" ] && [ -s "$st/$b.st2" ] && [ -s "$st/$b.st3" ] || bad "state files missing"
	# gambatte re-serializes one byte differently right after an unserialize (offset 378), so "equal" means
	# a handful of bytes at most, while a state saved after the game ran on differs in far more
	d13=$(cmp -l "$st/$b.st1" "$st/$b.st3" | wc -l); d12=$(cmp -l "$st/$b.st1" "$st/$b.st2" | wc -l)
	echo "  bytes differing: slot1 vs slot3 (load, save) = $d13, slot1 vs slot2 (game ran on) = $d12"
	[ "$d13" -le 4 ] || bad "load of slot 1 then save as 3: slot 3 differs from slot 1 in $d13 bytes"
	[ "$d12" -gt 4 ] || bad "slot 2 (saved later) is almost equal to slot 1 ($d12 bytes): the game did not advance"
	[ "$(cat "$st/$b.slot")" = 3 ] || bad "last slot not remembered: '$(cat "$st/$b.slot" 2>/dev/null)'"
	ls "$st" | grep -q '\.tmp$' && bad "temp file left in the state dir"
	# e) the remembered slot is where the next menu opens: Save with no slot change goes to slot 3
	st2="$tmp/st-e"; rm -rf "$st2"; mkdir -p "$st2"; cp "$st/$b.st1" "$st/$b.st2" "$st/$b.slot" "$st2/"
	K=""; kt b rot 300; kt b down 450; kt b a 610; kt b down 770; kt b down 930; kt b down 1090; kt b a 1250
	pm_run $sys "$rom" $core "$st2" "$tmp/s.txt" --no-video
	[ -s "$st2/$b.st3" ] && cmp -s "$st/$b.st1" "$st2/$b.st1" && cmp -s "$st/$b.st2" "$st2/$b.st2" || bad "slot persistence: save did not go to the remembered slot 3 alone"
	# f) loading an empty slot: a message, nothing loaded, nothing created
	st3="$tmp/st-f"; rm -rf "$st3"
	K=""; kt b rot 300; kt b down 450; kt b down 610; kt b a 770; kt b down 930; kt b down 1090; kt b a 1250
	pm_run $sys "$rom" $core "$st3" "$tmp/s.txt" --no-video
	grep -q 'state: load of .* empty' "$tmp/log" && [ "$(stat_get "$tmp/s.txt" states_loaded)" = 0 ] && [ -z "$(ls "$st3" 2>/dev/null)" ] || bad "empty slot load"
	# g) a state of the wrong size is refused
	st4="$tmp/st-g"; rm -rf "$st4"; mkdir -p "$st4"; head -c 100 "$st/$b.st1" >"$st4/$b.st1"
	K=""; kt b rot 300; kt b down 450; kt b down 610; kt b a 770; kt b down 930; kt b down 1090; kt b a 1250
	pm_run $sys "$rom" $core "$st4" "$tmp/s.txt" --no-video
	[ "$(stat_get "$tmp/s.txt" states_loaded)" = 0 ] && grep -q 'state: load of .* failed' "$tmp/log" || bad "short state file was not refused"
}

if [ $found = 1 ] && [ -n "$gb" ] && [ -f "$cores/gambatte_libretro.so" ]; then
	pause_suite gb "$gb" gambatte
	[ -n "$nes" ] && [ -f "$cores/quicknes_libretro.so" ] && pause_suite nes "$nes" quicknes

	# the screen: the menu is drawn black on white in a centred box, and after a resume the picture is
	# exactly what an uninterrupted run shows (paused time costs no frames, the dirty-row cache is reset)
	echo "== pause menu: drawing, repaint after resume, timing (gambatte)"
	rm -f "$tmp/fbm.raw"; K=""; kt b rot 500
	OLPC_RUN_FAKEFB="$tmp/fbm.raw" "$tmp/olpc-run" --libdir "$cores" --core gambatte --no-audio --max-seconds 2 --statedir "$tmp/st-h" \
		--savedir "$tmp/saves" --input-dir "$tmp/noinput" --test-keys "${K%,}" --no-overlay --stats "$tmp/s.txt" gb "$gb" >"$tmp/log" 2>&1
	res=$("$tmp/fbcheck" "$tmp/fbm.raw" 300 214 600 472); inside=${res#inside=}; inside=${inside%% *}
	echo "  menu box 600x472+300+214: $res"
	[ "${inside:-0}" -gt 180000 ] || bad "menu box not drawn (inside=$inside of 283200)"
	[ "$(stat_get "$tmp/s.txt" pause_count)" = 1 ] || bad "menu did not open on the fake framebuffer"
	for v in ref menu; do
		rm -f "$tmp/fb-$v.raw"; K=""; [ $v = menu ] && { kt b rot 500; kt b b 900; }
		OLPC_RUN_FAKEFB="$tmp/fb-$v.raw" "$tmp/olpc-run" --libdir "$cores" --core gambatte --audio-dev /nonexistent/pcm --frames 150 --statedir "$tmp/st-h" \
			--savedir "$tmp/saves" --input-dir "$tmp/noinput" --test-keys "${K%,}" --no-overlay --stats "$tmp/s-$v.txt" gb "$gb" >"$tmp/log" 2>&1 || bad "$v run failed"
	done
	cmp -s "$tmp/fb-ref.raw" "$tmp/fb-menu.raw" || bad "screen after resume differs from an uninterrupted run"
	[ "$(stat_get "$tmp/s-menu.txt" pause_count)" = 1 ] && [ "$(stat_get "$tmp/s-menu.txt" frames_run)" = 150 ] || bad "paused run: pause_count=$(stat_get "$tmp/s-menu.txt" pause_count) frames_run=$(stat_get "$tmp/s-menu.txt" frames_run)"
	fm=$(stat_get "$tmp/s-menu.txt" frame_ms_max)
	echo "  after resume: screen identical to the uninterrupted run; frame_ms_max=$fm underruns=$(stat_get "$tmp/s-menu.txt" underruns)"
	awk -v m="$fm" 'BEGIN{exit !(m<60)}' || bad "a frame took $fm ms after the pause (the paused time was caught up?)"
fi

if [ $fail = 0 ]; then echo "olpc-run tests: ALL OK"; else echo "olpc-run tests: FAILED"; fi
exit $fail
