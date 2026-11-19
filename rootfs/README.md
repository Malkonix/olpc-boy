# rootfs notes

`rootfs/overlay/` is copied over a fresh Alpine 3.18 x86 root by `scripts/mkrootfs.sh`.

## Boot-time conventions (all scripts)

* Time is measured against `/proc/uptime` only; the wall clock may be 2013 or 1970.
  `sleep`/`timeout` returned at once on the first real boot, so scripts use
  `olpc_sleep` and `bounded` from `usr/libexec/olpcboy/common.sh`.
* Bulk dumps are written to tmpfs (`$STAGE`, `/run/olpcboy/stage`) and copied to the
  stick with `olpc_publish`; `sync` runs every ~3 s (`syncloop`) and at stage markers.
* The log partition is mounted with `flush`, never `sync`.
* The internal NAND is policy-blocked: `cafe_nand` is blacklisted and
  `install`-guarded in `/etc/modprobe.d/olpcboy.conf`; nothing opens `/dev/mtd*`,
  block-device enumeration goes through `olpc_blockdevs` (sd*/mmcblk*/vd*/hd*/nvme*),
  and the initramfs rewrites `/etc/fstab` with explicit devices.
* mtimes in the image are normalised to 1970 so OpenRC never reports clock skew.

## `olpc-chordd` (button chords)

Source: `src/olpc-chordd/olpc-chordd.c` (one file, libc only). Built by
`scripts/mkrootfs.sh` as a static i386 binary (`-Os -march=geode -mtune=geode`) into
`/usr/local/sbin/olpc-chordd`; OpenRC service `olpc-chordd` (default runlevel, after
`olpc-keymap` and `olpc-dcon`, backgrounded with a pidfile like `olpc-powerbtn`).

Behaviour:

* Opens every `/dev/input/event*` that advertises KEY_PAGEUP, KEY_END and the four
  arrow keys (SQUARE = KEY_HOME and CROSS = KEY_PAGEDOWN are read from the same devices). Never grabs (no `EVIOCGRAB`): other programs read the same events.
  `inotify` on `/dev/input` rescans, so USB keyboards plugged later are covered.
* Chord = CIRCLE (KEY_PAGEUP) and CHECK (KEY_END) both held. State is tracked per
  key from press/release edges; `value 2` and repeated `value 1` of an already-held
  key (the rotate/check keys re-send presses) change nothing.
* While the chord is held and the direction was pressed **after** the chord formed:
  UP / DOWN = backlight one step up / down (step = max/8, same as `olpc-dcon`, clamped
  to 0..max, 0 allowed), first repeat after 300 ms then every 150 ms while held (the
  latest of UP/DOWN wins; releasing it or breaking the chord stops the repeat);
  RIGHT = colour, LEFT = monochrome (no repeat). A direction held before the chord
  formed, or still held when it re-forms, does nothing.
* Second chord: SQUARE (KEY_HOME) and CROSS (KEY_PAGEDOWN) both held. Same rules (the
  direction must be pressed after the chord formed; repeat 300 ms then 150 ms; re-sent
  presses change nothing). UP / DOWN = volume up / down, step = `max(1, (max-min)/16)`
  (Master is 0..31, so 1), clamped; LEFT = mute, RIGHT =
  unmute (explicit, not a toggle; the volume level is kept while muted). With all four
  chord keys held nothing happens, and a repeat or a held direction never carries over
  from one chord to the other: the direction has to be pressed again.
* Volume goes through raw ALSA control ioctls on `/dev/snd/controlC0` (no alsa-lib, no
  fork/exec; the control device is opened once per action). Element lookup is by name:
  `Master Playback Volume` first, then `PCM Playback Volume`; mute uses the matching
  `... Playback Switch`. All channels get the same value. When the primary is `Master`,
  `Master Mono Playback Volume/Switch` is set to the same value so the built-in speaker
  path follows. The AD1888 (cs5535audio) powers up with every switch off and every
  level 0, so at daemon start (or at the first use if the card shows up later) any of
  `Master`, `PCM`, `Master Mono` that is muted/zero is unmuted and set to 88 % (PCM 75 %).
  This runs once per daemon start, so restarting the service after a manual mute undoes
  it. No card (QEMU) is logged once (`volume: no sound card`) and retried on the next use.
* sysfs is written directly: first dir of `/sys/class/backlight/*` that has
  `max_brightness`, and `/sys/devices/platform/dcon/monochrome`. Missing nodes (QEMU)
  are logged and ignored.
* One `poll()` over the devices and the inotify fd; the timeout is only finite while
  UP/DOWN repeats inside a chord, so the idle CPU use is zero.
* Log: one line per action on stderr (`/var/log/olpc-current/chordd.log`) and in
  `timeline.log` as `<uptime> chordd: <text>`, the same format as `tl`.

Chord state for other programs: `/run/olpcboy/chord` always holds two bytes, `1\n` or
`0\n` (rewritten in place, never truncated, so a reader never sees an empty file).
It is `1` while CIRCLE+CHECK or SQUARE+CROSS are held and also while a D-pad key is still down after
the chord broke (releasing CIRCLE first must not turn the tail into D-pad input), and
`0` otherwise. The flag is set on the press of the second chord key, i.e. before any
direction can be pressed on a human timescale; a consumer reading the same event
stream should check the file when it handles a D-pad event and ignore it while `1`.
CIRCLE and CHECK themselves still reach the consumer as normal keys.

Overrides (for tests): `CHORDD_INPUT_DIR`, `CHORDD_BACKLIGHT_GLOB`, `CHORDD_MONO`,
`CHORDD_STATE`, `CHORDD_TIMELINE`, `CHORDD_TAIL_MS`, `CHORDD_SND_CTL`
(`/dev/snd/controlC0`), `CHORDD_FAKE_MIXER` (file-backed fake mixer for tests; the ioctl
backend is never exercised by the test, and in `--test` mode it is pointed at a
non-existent path unless `CHORDD_SND_CTL` is set).

Tests without hardware: `make chordd-test` (or `sh src/olpc-chordd/test.sh` anywhere
with gcc). `olpc-chordd --test` replays text events from stdin (`<ms> <KEY> <0|1|2>`),
`--replay` replays raw 16-byte i386 `input_event` records (the struct is declared with
fixed widths, so a 64-bit build reads the same bytes); sysfs paths point at a temp
dir, and the repeat timer runs on a virtual clock taken from the event stream.
`mkrootfs.sh` runs this test and fails the build if it fails.

Not covered by the test: real evdev ioctls, inotify, hot-unplug, and timing on the
Geode itself.

## `olpc-run` (libretro frontend)

Source: `src/olpc-run/` (`olpc-run.c` main loop, ROMs, saves, stats; `scale.c` integer
scaler; `audio.c` + `resample.c` raw-ALSA sink; `input.c` evdev; `libretro.h` is the
MIT-licensed API header). Built by `scripts/mkrootfs.sh` as a **dynamically linked**
musl binary (`-O2 -march=geode -mtune=geode`, it `dlopen`s the cores) into
`/usr/local/bin/olpc-run`. Cores live in `/usr/lib/libretro/<name>_libretro.so`.

    olpc-run [options] <gb|gbc|nes|core.so> <rom | rom.zip>

`gb`/`gbc` use gambatte, `nes` uses quicknes; `--core fceumm` (or a name/path) overrides.
Options: `--stats FILE` (written at exit only), `--stats-live FILE` (also every 10 s; point it at tmpfs), `--frames N`, `--max-seconds N`, `--no-audio`, `--no-video`,
`--savedir DIR`, `--max-skip N` (default 4), `--max-scale N`, `--audio-period-ms X`
(default 4), `--audio-latency-ms X` (default 45), `--audio-target N` (frames, overrides
the latency), `--no-rt`, `--xrun-debug N` (writes N to the PCM's
`/proc/asound/cardX/pcmDp/xrun_debug`, 0..31: bit 0 = log XRUN and hw_ptr anomalies, bit 1 = also a stack dump, bit 2 = jiffies check; the boot test uses 5), `--test-stall-ms M --test-stall-at F[,F2..]` (sleeps M ms in the loop at those frames, to exercise the recovery), `--no-fill-query` (never calls the DELAY
ioctl: paced by `poll()` only, frame skipping falls back to "the previous frame's core +
blit took longer than a frame", fill statistics are -1), `--running-file F`, `--audio-dev PATH`, `--libdir DIR`, `--sysdir DIR`,
`--restore-console` (only give the text console back and clear the screen), `olpc-run`
without arguments prints them all. Exit code 0 for every deliberate exit (frame limit,
exit gesture, SIGTERM/SIGINT, time limit), 1 for load/runtime errors, 2 for usage.

* **ROMs**: plain files, or `.zip` (detected by the `PK` magic): the first entry ending
  in `.gb/.gbc/.nes/.sgb` is found with `unzip -l` and read from `unzip -p` through a
  pipe (busybox applets, fork/exec, no zip library); ROMs are capped at 16 MB. The
  extracted data goes to the core from memory; only a core that sets `need_fullpath`
  gets a temporary file in `/run/olpcboy`.
* **Saves**: `<savedir>/<rom basename without extension>.srm` (and `.rtc` when the core
  has one). Default savedir is `saves/` next to the ROM directory (`<romdir>/../saves`),
  created on demand. Loaded at start, written on exit and every 60 s when the contents
  changed, always as temp file + `fsync` + `rename` + directory `fsync`. A failed
  `mkdir`/write is logged and the game goes on. A crash (SIGSEGV, SIGKILL, power loss)
  loses what was not written yet. The boot test uses `/run/olpcboy/saves` (tmpfs).
* **Video**: largest integer scale that fits 1200x900, centred, borders cleared once and
  again on a geometry change. Each source row is expanded once and `memcpy`d for the
  repeated lines; rows equal to the previous drawn frame are skipped (a copy of the
  last drawn source is kept; the framebuffer is never read). RGB565, 0RGB1555 and
  XRGB8888 sources are converted to RGB565. Note quicknes outputs 240x224 (not
  256x240), so `auto` picks 4x (960x896); `--max-scale 3` gives 720x672. The console
  is in `KD_GRAPHICS` while running and always restored (text mode, screen cleared, VT
  bounce so fbcon repaints) on normal exit, SIGINT/SIGTERM/SIGHUP and the time limit;
  SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT restore text mode and clear but skip the VT
  bounce. SIGKILL cannot be handled: run `olpc-run --restore-console` afterwards (the
  boot script does). Without `/dev/fb0` the run continues with a null video sink.
* **Audio**: `/dev/snd/pcmC0D0p` (then `pcmC1D0p`) through raw ioctls, S16 stereo. It
  first asks for the core's own rate (32768 Hz for gambatte); if `hw_params` refuses it
  opens 48000 Hz and resamples (linear, 32.32 fixed point). `poll()` on the PCM only
  wakes on period interrupts, so the period is the pacing granularity: the default is 4 ms
  (period 128 frames at 32768 Hz, 192 at 48000 Hz, rounded to 16 frames and clamped to
  the hardware limits), in a buffer of at least 4096 frames (32 x 128, or 24 x 192). The
  card's clock is the pacing source: before handing a frame's audio over, `poll()` blocks
  until the fill is down to the target (`avail_min` = buffer - target, playback starts at
  `start_threshold` = target): 45 ms rounded up to whole periods, i.e. 1536 frames
  (46.9 ms) at 32768 Hz and 2304 (48 ms) at 48000 Hz. The previous layout (1024-frame
  periods, 2048 target) is `--audio-period-ms 31.25 --audio-latency-ms 62.5` at 32768 Hz.
  No `sleep` is used. The hardware may grant other values; the stats report the real
  ones. When the fill is lower than the expected
  core + blit time (moving averages, plus 128 frames of margin, device-rate frames
  whatever the period) the blit of that frame is skipped; the core still runs. At most
  `--max-skip` consecutive frames, and never during the first 30 frames. Duplicate frames
  from the core (`NULL` data) are counted as `frames_dupe`.
* **Priority**: with a real PCM, `olpc-run` calls `mlockall(MCL_CURRENT|MCL_FUTURE)` and
  switches itself to `SCHED_FIFO` priority 10 (the kernel is HZ=100 with voluntary
  preemption, so a background job could otherwise hold the CPU for a whole 10 ms tick). It
  blocks on the sound card for about 10 of every 16.6 ms, which leaves the rest to
  `olpc-chordd` and the shell jobs. If 90 frames in a row do not block at all (the core is
  slower than the card) it drops back to `SCHED_OTHER` and logs it. musl's
  `sched_setscheduler` is a stub that returns ENOSYS, so the raw syscall is used and the
  policy is read back with `sched_getscheduler` (`rt_policy`, 1 = FIFO; `rt_errno` on failure). No `SCHED_FIFO` in
  clock mode or with `--no-audio`; `--no-rt` also skips `mlockall`. Failures are logged and
  ignored; the stats say `rt=fifo|demoted|failed|no-rt|n/a`.
* **Running marker**: while the loop runs `olpc-run` keeps `/run/olpcboy/running` (its pid,
  `--running-file`). `syncloop` syncs every ~18 s instead of every ~3 s while the pid in it
  is alive; `olpc-tty1` removes the file after each run.
* **Input**: reads `/dev/input/event*` directly, no grab; `inotify` on `/dev/input`
  picks up pads plugged later. Built-in buttons (device "AT Translated Set 2 keyboard"):
  D-pad = D-pad, CHECK (KEY_END) = A, CROSS (KEY_PAGEDOWN) = B, CIRCLE (KEY_PAGEUP) =
  START, SQUARE (KEY_HOME) = SELECT. The chord keys overlap game buttons (CIRCLE+CHECK =
  START+A, SQUARE+CROSS = SELECT+B): accepted. USB pads are unchanged. Pads (any node with BTN_GAMEPAD or BTN_JOYSTICK):
  D-pad from ABS_HAT0X/Y, BTN_DPAD_* or the left stick ABS_X/Y (past half deflection),
  BTN_SOUTH = B, BTN_EAST = A, BTN_SELECT, BTN_START; generic clones: BTN_TRIGGER = B,
  BTN_THUMB = A, BTN_BASE3/BTN_BASE4 = SELECT/START. The tables are at the top of
  `input.c`. While `/run/olpcboy/chord` is `1` the built-in D-pad is not passed to the
  game. **Exit**: hold ROTATE (KEY_DIRECTION, 153) for 1 s, or pad SELECT+START for
  1 s, or SIGTERM. ROTATE re-sends presses while held; only the held state is tracked.
* **Storage never blocks the loop**: nothing in the frame loop writes to the USB stick. The
  statistics are kept in memory and written once at exit (`--stats`); `--stats-live` is a
  copy every 10 s meant for tmpfs, which `olpc-tty1` uses so a run killed by the hard timeout
  still leaves something (`run-<name>.live`). Diagnostic messages from inside the loop
  (`dlog`) are buffered in memory (8 KB) and printed at exit. The `/run/olpcboy/chord` flag
  is read with one `pread` on a descriptor kept open. Battery saves are written by a helper
  thread (normal priority, nice 10, signals blocked, started before `SCHED_FIFO` is taken):
  the loop only compares the save RAM with its private copy of what is on disk every 60 s,
  copies it into a snapshot buffer when it changed (a try-lock, never a wait) and the helper
  does temp file + `fsync` + `rename` + directory `fsync`. At exit the loop copies the RAM
  once more, unloads the core, restores the screen and then waits at most 8 s for the helper
  (`saves_unfinished` in the stats; a dead SD card cannot hang the exit).
* **Underrun recovery** (`diag.c` `recov_*`, performed in `audio.c`): an underrun opens an
  incident. First underrun: `DROP` + `PREPARE` + silence up to the target fill. A second one
  while the incident is open: close and reopen the PCM (full `hw_free`/`hw_params`). A third:
  clock pacing for the rest of the run. While an incident is open the loop is paced by the
  clock (sleeps to the frame deadline, no catch-up bursts) instead of running free. The
  incident closes at the first frame whose `poll()` really blocked (at least 1 ms) or after 6
  clean frames; the first event of each incident then gets `rec_frames` and `rec_ms`.
* **Statistics**: `--stats FILE`, text `status=running` only in the live copy and
  `status=<frames|exit-button|signal|timeout>` at exit.
  `key=value` lines: `frames_run`, `frames_drawn`, `frames_skipped`, `frames_dupe`,
  `wall_s`, `fps_effective`; per-frame `core_ms_*` (core only, blit excluded),
  `blit_ms_*` (average over drawn frames), `audio_wait_ms_*` (poll + write),
  `work_ms_*` (core + blit, the real headroom measure), `frame_ms_*`, each `_avg` and
  `_max`; `underruns`, `audio_stalls` (a 250 ms poll timeout), `audio_mode`
  (`alsa`/`clock`/`null`), `audio_core_rate`, `audio_device_rate`, `audio_period`,
  `audio_buffer`, `audio_target`, `audio_period_ms`, `audio_target_ms`, `audio_avail_min`,
  `audio_start_threshold`, the requested `audio_req_*`, `audio_fill_min`/`audio_fill_max`
  (frames and `_ms`; the fill when poll woke up / right after the write, after the first
  30 frames), `frame_hist_lt14`, `_14_18`, `_18_22`, `_22_30`, `_30_40`, `_gt40` (frame
  time buckets in ms), `core_over12_frames`, `blit_over12_frames`,
  `audio_wait_over12_frames` (frames where that phase took more than 12 ms), `rt`,
  `rt_policy`, `rt_errno`, `mlockall`, `fill_query`, `underruns_poll` / `underruns_write`
  (how each underrun was noticed: `POLLERR` from `poll()` or `EPIPE` from the write),
  `underrun_recover_failed`, `recover_prepare` / `recover_reopen` / `recover_clock` /
  `recover_reopen_failed`, `incidents`, `incidents_recovered`, `recovery_paced_frames`,
  `xrun_debug_req` / `xrun_debug_applied` / `xrun_debug_errno` / `xrun_debug_path`,
  `test_stall_ms`, `test_stalls_done`, `saves_written`, `saves_failed`, `saves_unfinished`,
  `xrun_events_total`,
  `xrun_events_kept` and up to 64 `xrun_event_N=frame=.. since_prev_ms=.. work_ms=a,b,c
  fill=.. via=poll|write action=prepare|reopen|clock rec_frames=.. rec_ms=..` lines (the last 64 underruns: frame number, time since the previous
  frame started, core + blit time of the previous three frames, last fill read before it).
  `video_mode` (`fb`/`fake`/`null`), `video_scale`,
  `video_rect`, `rows_drawn_pct` (share of source rows actually redrawn). The log on
  stderr states the chosen audio rate/buffer, video geometry and a final summary line.

Boot test (`olpc-tty1`, after the selftest; first `asound-proc.txt` = `ls -lR /proc/asound`
plus `pcm0p/xrun_debug` and `pcm0p/sub0/*`): `olpc-run` plays `bench/rom.<sys>` for 1200
frames (about 20 s) with video and audio, all with `--xrun-debug 5` (basic log + jiffies
check; 3 would add stack dumps), soft limit 30 s, hard `bounded` kill at 45 s:
`gb` (defaults; expected: no underruns), `gb-stall` (`--test-stall-ms 150 --test-stall-at
300,700`; expected: exactly two underruns, each recovered in a few frames), `gb-stall-p16`
(the same with `--audio-period-ms 16`), `gbc`, `nes-quicknes`. Each leaves
`$BOOTDIR/run-<name>.stats` (written at exit), `run-<name>.log` and, when the exit file is
missing, `run-<name>.live` (the tmpfs copy). After them the kernel's xrun/pcm/cs5535
messages are saved to `$BOOTDIR/xrun-dmesg.txt`. Then an open-ended GB session
(`run-session.*`, also `xrun-dmesg-session.txt`) until ROTATE is held for 1 s, then the
WiFi test, then the status screen. (The FCEUmm core stays installed, `--core fceumm`, but is
not in the sequence: too slow.)
Kernel command line flags:
`olpcboy.norun` skips the whole sequence, `olpcboy.buttons` and `olpcboy.soundtest`
re-enable the 60 s button capture and the 25 s sound test (off by default now),
`olpcboy.fbbench` and `olpcboy.corebench` the two older benchmarks. Without the Geode
framebuffer (QEMU) all five runs use `--no-video --no-audio` with 120 frames (the stall
frames become 30,90); the interactive session and the WiFi test are skipped.

Tests: `make olpc-run-test` (or `sh src/olpc-run/test.sh` in the builder container;
`mkrootfs.sh` runs it and fails the build on failure). Self-test of the scaler (all
source/destination formats, odd sizes, strides and offsets against a per-pixel reference,
dirty-row logic), the resampler (chunking independence, length, error against an ideal
sine), the input mapping (via pipes) and the audio buffer derivation (period, buffer,
target, `avail_min` from rate, requested ms and hardware limits), and the diagnostics helpers
(underrun ring buffer and its formatting, time-based skip criterion, strict option parsing, the recovery policy driven by a fake stream, clock pacing, the stall list), the save helper (content, snapshot isolation, no `.tmp`, a bounded exit wait with a writer blocked on a FIFO) and the in-place chord flag; then, if `out/cores` holds the cores and ROMs,
600 frames of every core through the real frontend with null sinks and with a fake
framebuffer (`OLPC_RUN_FAKEFB=<file of 1200x900x2 bytes>`; the file keeps the last
frame, and a checker counts non-black pixels inside and outside the expected
rectangle), zip loading with glob characters in the entry name, the clock fallback, forced
frame skipping (`--test-blit-delay-ms`) and save write/reload (`--test-poke-save`).
Not covered without hardware: the ALSA path, the real framebuffer and console modes,
evdev nodes, inotify and all timing on the Geode.

### WiFi
Put `wifi.cfg` in the repo root (git-ignored; `make image` copies it to WIFI.CFG in the FAT root, and it can be edited from any PC). One network per line, TAB-separated: `SSID<TAB>key<TAB>TYPE`, TYPE = WPA2|WPA|WEP|OPEN (case-insensitive); first line = highest priority; `#` comments, blank lines and CRLF are fine; spaces allowed in SSID and key; leave the key empty for OPEN.
`olpc-wifi up|down|status|check|run` loads usb8xxx, starts wpa_supplicant (nl80211, then wext fallback), runs DHCP and a one-shot NTP; `check` tests connectivity and downloads a test cover. Details are in `wifi.log` in the boot dir (never the key); timeline lines are prefixed `wifi:`. The boot test sequence calls `olpc-wifi run` after the play session; `olpcboy.nowifi` on the cmdline skips it. The `olpc-wifi` OpenRC service exists but is not enabled.

<!-- olpc-covers:begin -->
## `olpc-covers` (box art)
Sources: `src/olpc-covers/` (`png2obc.c`, `olpc-covers-match.c`, vendored `stb_image.h`, tests) and the script `/usr/local/sbin/olpc-covers`. Installed as `/usr/local/sbin/{olpc-covers,png2obc,olpc-covers-match}` (the two binaries are static, built with `-march=geode`, stb_image with `STBI_NO_SIMD`).

    olpc-covers fetch [--system gb|gbc|nes] [--force]     # runs at nice 19
    olpc-covers status                                    # prints /run/olpcboy/covers.status

For every `roms` directory (any case, top level of a filesystem mounted under `/mnt` or `/media`, with `gb`/`gbc`/`nes` inside) each ROM (`.zip .gb .gbc .nes .7z`, hidden files skipped) without `<roms>/../covers/<sys>/<rom name without extension>.obc` gets its libretro box art from `http://thumbnails.libretro.com/<System>/Named_Boxarts/`. If there is no default route it tries `olpc-wifi up`; still none: status `no network`, exit 5 (nothing is touched when no ROM needs a cover). The Apache index of each system is cached in `covers/.index/<sys>.html` for 30 days; `olpc-covers-match` maps the ROM name to an index entry with exact rules only (character substitution `&*/:\`"<>?\|` -> `_`; ignoring Rev/version/Beta/Proto tags; same base title with the best region, same region first, then World, USA, Europe, Japan; normalised title with articles and punctuation removed). There is no fuzzy matching: a wrong cover is worse than none. ROMs with no match go to `covers/<sys>/.missing` (epoch, date, name) and are retried after 30 days or with `--force` (which also refreshes the index). The PNG is downloaded to tmpfs (busybox wget, 20 s timeout, one retry), converted by `png2obc` and deleted.

`.obc`: 16 bytes (`OBC1`, u16le width, u16le height, 8 zero bytes) then RGB565 little-endian pixels, top row first; fits 480x480, never upscaled, area-average filter, alpha over RGB(24,24,24), written atomically (hidden temp file + rename). `png2obc in.png out.obc [maxw maxh]` also accepts JPEG and rejects anything above 4096 px per side or 8 Mpixel (exit 3).

Status line (`/run/olpcboy/covers.status`): `fetching 3/27 NAME`, `fetching index gb`, `done 21 ok 4 missing 2 failed`, `no network`, `game running`, `connecting`. One instance at a time (lock `/run/olpcboy/covers.lock`, exit 3); it refuses to start while `/run/olpcboy/running` holds a live pid (exit 4) and stops between files if a game starts. Log (names and results only): `/var/log/olpc-current/covers.log`. Exit: 0 finished, 2 usage, 3 busy, 4 game, 5 no network.

Tests: `sh src/olpc-covers/test.sh` in the builder container (no network: generated PNGs, synthetic indexes, fake wget); `mkrootfs` runs them.
<!-- olpc-covers:end -->

<!-- olpc-menu:begin -->
## `olpc-menu` (game launcher)
Sources: `src/olpc-menu/` (`olpc-menu.c` UI and main loop, `menulib.c` discovery/covers/repeat/state/text, `font8x16.h`, `selftest.c`, `test.sh`); it also compiles `src/olpc-run/input.c` and `diag.c` (same button maps as the emulator). Installed as `/usr/local/bin/olpc-menu`, dynamically linked musl, `-O2 -march=geode -mtune=geode`. tty1 starts it after the selftest and restarts it when it exits (1 s pause; after 5 runs shorter than 3 s in a row, or exit code 3 = no Geode LX framebuffer as in QEMU, it falls back to the status screen). `olpcboy.nomenu` on the kernel command line skips it. The timed runs, the play session and the WiFi test of `olpc-tty1` now run only with `olpcboy.tests`.

**ROMs.** On every filesystem directly under `/mnt` (and `/mnt/*`, `/media/*/*` for automounters) a top-level directory named `roms` (any case) with `gb`, `gbc`, `nes` directories (any case; others such as `GBA` are ignored). Files `.gb .gbc .nes .zip` (any case, hidden files and sub-directories skipped); the name shown is the file name without its last extension, sorted case-insensitively. The USB stick (`/mnt/olpclog`, FAT label OLPCLOG) has `roms/`; `make image` copies the repo's git-ignored `ROMs/GB|GBC|NES` zips into `roms/gb|gbc|nes` with their long names and fails when they do not fit the 256 MiB partition. An SD card's first partition (`/dev/mmcblk0p1`, vfat/ext2-4/exfat) is mounted read-write, noatime at `/mnt/sd` by `olpc-sdmount`: at boot (service `olpc-sd`, never fatal, no card = nothing) and from the menu when `/dev/mmcblk*` appears or disappears (inotify on `/dev`, 1 s debounce, then the menu rescans); a removed card is released with a lazy unmount. Only that one partition is ever touched, never an MTD device. Saves go to `<roms dir>/../saves` (olpc-run's default).

**Screen** (1200x900 RGB565, back buffer in RAM, only changed rectangles are copied to `/dev/fb0`, the fb is never read): title bar (`< system >`, game count, colour per system), list with the selection bar and a scroll bar (names fitted to 39 columns with `...`), cover panel on the right (480x480 frame) with the full name wrapped under it, hint bar. Font: Terminus 16 (8x16, regular) drawn at 2x and 3x, Latin-1 accents render, characters beyond Latin-1 degrade to ASCII lookalikes or `?`; Terminus is (c) 2010 Dimitar Toshkov Zhekov, SIL Open Font License 1.1, text in `src/olpc-menu/FONT-LICENSE.txt`. Covers: `<roms dir>/../covers/<system dir lowercased>/<name>.obc` (OBC1, see olpc-covers), loaded only after the selection rested 150 ms; missing or invalid (bad magic, 0 or > 480 px, short file) shows a placeholder with the system name. No ROMs at all: a screen that says to put files in `roms/gb`, `roms/gbc`, `roms/nes` on the SD card or USB stick and use Rescan.

**Controls.** UP/DOWN move; the repeat is computed from the held state (300 ms, then ~12 rows/s, after 1.5 s a page per 200 ms). LEFT/RIGHT switch system, A launches, START opens Options (Rescan ROMs, Download covers = `/usr/local/sbin/olpc-covers fetch` in the background with the last line of `/run/olpcboy/covers.status` shown (a second A stops it), WiFi on/off = `olpc-wifi up|down` in the background with its `status`, Power off and Reboot each ask for a second A), B or START closes. A, B, START act on release and only when pressed alone with no chord: CIRCLE+CHECK and SQUARE+CROSS are the olpc-chordd chords. While `/run/olpcboy/chord` is 1 the built-in D-pad is ignored. Built-in buttons and USB pads use the same maps as olpc-run. Idle = blocked in `ppoll()`; timers exist only for a held key, the cover rest, the SD debounce and a hot-plug rescan.

**Launch.** "Loading..." frame, then `fork`/`exec` `/usr/local/bin/olpc-run <system> <rom path>` with stdout/stderr in `/run/olpcboy/menu-run.err`; any covers download is stopped first (olpc-run refuses it anyway). The menu stays in `KD_GRAPHICS`; on return it drains the input, reads the real key state (`EVIOCGKEY`) and ignores every key still held until it is released, then repaints. A non-zero exit shows the last lines of the file on an error screen until a button is pressed. The selection (system + file name) is kept in `/run/olpcboy/menu.state` (written on launch and exit) with a best-effort copy `<roms dir>/../olpc-menu.state` written by a forked child at launch, so the UI never waits for the stick; at start the tmpfs copy wins, else the first copy found. SIGTERM/INT/HUP exit cleanly; fatal signals restore the console first.

**Tests.** `make olpc-menu-test` (or `sh src/olpc-menu/test.sh`; `mkrootfs.sh` runs it). Unit: discovery on a temp tree (case variants, GBA ignored, depth, `/media` depth 2, zip), sort order, repeat timing (300 ms delay, ~12/s, page steps, bounded catch-up), state round trip and atomicity, UTF-8/Latin-1 text, wrapping, cover validation. Program: with `OLPC_MENU_FAKEFB=<1200x900x2 file>` and `--script FILE` (lines `<ms> key UP|DOWN|LEFT|RIGHT|A|B|START|SELECT down|up|tap`, `<ms> dump FILE` writes the back buffer as raw RGB565, `<ms> chord 0|1`, `<ms> end`) it renders frames (keep them with `FRAMES_DIR=dir`), checks that the fake fb equals the back buffer, the chord flag, A+START not acting, repeat landing, page steps, a stub olpc-run (`OLPC_MENU_RUN`) with arguments and held-key handling, the error screen, power-off confirmation, zero CPU while idle, exit code 3 without the Geode framebuffer, and `olpc-sdmount` with stub `blkid`/`mount`. Other hooks: `OLPC_MENU_SCAN`, `OLPC_MENU_RUNDIR`, `OLPC_MENU_CHORD`, `OLPC_MENU_INPUTDIR`, `OLPC_MENU_DEVDIR`, `OLPC_MENU_COVERS_BIN`, `OLPC_MENU_WIFI_BIN`, `OLPC_MENU_SDMOUNT`, `OLPC_MENU_POWEROFF`, `OLPC_MENU_REBOOT`.
Not covered without hardware: the real framebuffer and console modes, evdev nodes and hot-plug, the SD card mount, drawing speed on the Geode.
<!-- olpc-menu:end -->
