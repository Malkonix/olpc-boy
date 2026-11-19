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

## Design note: `olpc-chordd` (not implemented yet)

Goal: button chords on the XO-1 (D-pad, four game keys, rotate, power: no keyboard)
that trigger actions such as `olpc-dcon brightness up|down`, `olpc-dcon mono|color`.

* Must be a small static C program (i386, no SSE), not a shell script: a shell
  loop adds tens of ms per event on the 430 MHz Geode and cannot keep up with
  two keys pressed within a few ms.
* Input: open every `/dev/input/event*` that advertises any of the wanted key
  codes (sysfs `capabilities/key`), `epoll` them, read `struct input_event`
  (16 bytes on i386). No `EVIOCGRAB`: the daemon only observes.
* State: bitmap of keys currently down; `value==2` (autorepeat) ignored.
  A chord fires on the press edge of its last key if all other members are
  already down, or went down less than ~150 ms earlier (window configurable).
  Optional hold time per chord; fire once per hold, re-arm when any member is
  released.
* Config `/etc/olpc-chordd.conf`, one chord per line, e.g.
  `ROTATE+UP  exec /usr/local/sbin/olpc-dcon brightness up`.
  Actions are `fork`+`execv` (no shell); at most one action in flight.
* Logging: one line per fired chord and per config problem, appended to
  `$BOOTDIR/timeline.log` (same `uptime message` format as `tl`).
* Service: OpenRC `olpc-chordd`, `need olpclog`, `after mdev hwdrivers`; rescan
  the device list once ~20 s after start (slow input devices), like `olpc-powerbtn`.
* Tests on a PC: replay the raw captures `buttons-evN.raw` (16-byte records)
  through the same decoder; no hardware needed.
* **Blocked on data**: the key codes of the game keys and the rotate button are
  unknown until the next boot's `buttons.log` / `buttons-ev*.raw` exist, and the
  first boot saw no input device for them at all (only power, e-book and lid).
