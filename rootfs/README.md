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
  `Master`, `PCM`, `Master Mono` that is muted/zero is unmuted and set to 60 % (PCM 75 %).
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
