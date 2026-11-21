# kernel

Linux **6.1.192** (LTS), i386, tuned for the XO-1 (Geode LX, CS5536, OFW).
Build: `docker run --rm -u $(id -u):$(id -g) -v $PWD:/work -w /work olpc-boy-builder bash kernel/build.sh`
Outputs in `out/kernel/`: `bzImage`, `modules/lib/modules/<ver>/`, `version`, `config`.

- `config` - full .config consumed by build.sh. `config.frag` is the source it was
  generated from (`make ARCH=i386 allnoconfig KCONFIG_ALLCONFIG=config.frag`, then `olddefconfig`).
- `patches/*.patch` - applied in order to the pristine tarball (`patch -p1`).
- `INITRAMFS_DIR` (default `out/initramfs`) is embedded via `CONFIG_INITRAMFS_SOURCE`,
  root-owned (UID/GID 0). A directory source cannot create device nodes: `/dev` comes from
  devtmpfs (userspace must mount it; DEVTMPFS_MOUNT only applies to a real root).
  Re-running after only the initramfs changed relinks only (no re-extract).
- Compression: gzip. Decompresses fastest on a 433 MHz in-order CPU, needs nothing but
  the kernel's own boot code (OFW just loads the bzImage as bytes). Switch to
  `CONFIG_KERNEL_XZ` if image size matters more than boot time.

## DCON (display controller: backlight, color/mono, suspend)

- `drivers/staging/olpc_dcon` was marked `depends on BROKEN` in **5.19** (commit
  "staging: olpc_dcon: mark driver as broken", Jun 2022). Stated reason: fbdev
  `num_registered_fb` was about to become private to fbmem.c, and the driver
  was stalled in staging. Removed outright in 6.13. Not a GPIO reason.
- Independent bug (since the gpiod conversion): the XO-1 path calls
  `devm_gpiod_get("dcon_stat0" ...)` but no gpiod lookup table exists for the
  `cs5535-gpio` chip, so probe fails with -ENOENT. Also Kconfig says
  `GPIO_CS5535 && ACPI` (XO-1 has no ACPI; intended `||`).
- 6.6 and later: `registered_fb` is no longer declared in any header; 6.1 still
  defines the symbols in fbmem.c, so 6.1 is the newest LTS needing only a small patch.
- Patches: 0001 drops BROKEN, fixes the dependency, makes it bool, adds the GPIO
  lookup table (STAT0=5, STAT1=6, IRQ=7, LOAD=11, BLANK=12, from `asm/olpc.h`);
  0002 declares `registered_fb`/`num_registered_fb` locally (built-in only).
- Lookup table `dev_id`: gpiolib (`gpiod_find_lookup_table`) matches a NULL `dev_id`
  only to a NULL consumer device, so the first version never matched the I2C client
  and probe failed with -ENOENT ("failed to request dcon_stat0 GPIO: -2"). Patch 0001
  now fills `dev_id` from `dev_name(client)` (`0-000d`: adapter 0, address 0x0d) at
  init, registers the table once, removes it on failure, and a missing
  `cs5535-gpio` chip gives -EPROBE_DEFER (gpiod_find) instead of an error.
- Not verifiable without hardware: that probe now succeeds on a real XO-1 (needs
  the fb registered first and the scx200_acb adapter to detect the DCON at 0x0d).

## CAFE NAND (internal flash): never written by accident

- `CONFIG_MTD_NAND_CAFE=m`: nothing probes the NAND at boot. Userspace must not
  autoload `cafe_nand` (blacklist it); load it by hand when needed.
- Patch 0004. The CAFE keeps Reed-Solomon ECC in OOB bytes 0..13, so the generic
  "OOB byte 0 is the bad-block marker" test calls every programmed block bad (the
  first boot logged ~6700 of 8192). The driver now sets `NAND_BBT_NO_OOB_BBM`:
  the flash-BBT search no longer skips candidate blocks as bad
  (`bbt_block_checkbad`) and `nand_block_markbad_lowlevel` writes no OOB marker.
- Module parameters:
  - `allow_bbt_write=0` (default): `NAND_BBT_CREATE|NAND_BBT_WRITE` are removed from
    all four descriptors. An existing flash table (`Bbt0`/`1tbB`, last 4 blocks) is
    read. `check_create()` skips creation (`!(td->options & NAND_BBT_CREATE)`) and
    `write_bbt()` is never reached (guarded by `NAND_BBT_WRITE` in `check_create`,
    `nand_update_bbt`); `mark_bbt_region` only marks RAM. With no table on flash
    the RAM table stays zeroed: every block counts as good, no scan is done.
  - `allow_bbt_write=1`: stock behaviour (create + erase + write a table).
  - `skipbbt=1` (existing): no BBT at all; this does NOT make the device read-only,
    and `nand_block_isbad` then falls back to the (wrong) OOB marker test.
- Which table wins: `search_bbt()` scans downward from the last block and stops at
  the first block carrying the pattern, so an old table at blocks 8191/8190 is
  found before any table at 8189/8188 (the one the first boot wrote). Versions are
  only compared between main and mirror. If no old table exists, the table written
  at 8189/8188 (version 1, ~6700 false bad blocks) is the one read. Blocks 8188/8189
  were erased by that write.
- `CONFIG_MTD_CMDLINE_PARTS=y`: `mtdparts=cafe_nand:-(nand)ro` gives a read-only
  partition (the driver already probes `cmdlinepart`).
- This does not make `/dev/mtd*` read-only for userspace (flash_erase, JFFS2 rw...).

## Other patches / config notes

- 0005 `olpc_battery`: `olpc_bat_desc` had no `.type` so `type` read "Unknown".
- 0006 `cs5535audio`: `snd_cs5535audio_pcm_pointer()` read `ACC_BM0_PNTR` once and
  returned 0 when the value was out of range. The core calls `.pointer` from process
  context (write, `SNDRV_PCM_IOCTL_DELAY/STATUS`) and from the period interrupt; if a read
  comes back slightly earlier than the previous one, `snd_pcm_update_hw_ptr0()`
  (`sound/core/pcm_lib.c`, the `new_hw_ptr < old_hw_ptr` branch) takes it for a wrap of the
  ring, adds `buffer_size` to hw_ptr, `avail` reaches `stop_threshold` and the stream is
  stopped with an XRUN although it is full. Symptom on the XO-1: ~1.7 underruns/s at a
  128-frame period with no frame near draining the 47 ms fill, 4 in 20 s at 1024 frames
  (8x fewer PRD boundaries). The patch re-reads the register until two reads agree (at
  most five retries, like intel8x0/via82xx), keeps the last good position per dma, returns
  it instead of 0 when the value is out of range, and treats a step back of less than
  min(period, buffer/4) as a stale read (a real wrap goes back by nearly the whole
  buffer). Position is frame aligned; reset on prepare/start/resume. Locking: `.pointer`
  runs under the stream lock, as do trigger and (with the stream stopped) prepare, so
  `last_pos` needs no lock of its own. Not verified on hardware: that the register really
  does go backwards (the new `xrun_debug` runs will show `hw_ptr skipping` / the
  "pointer went back" warning if it does).
- 0007 `cs5535audio`: restart the bus master at the first descriptor on prepare. The
  hardware logs of the first runs showed that every real stall (a 135 ms hole against a 47 ms
  fill) was followed by a series of back-to-back underruns (`fill=0`, frames about 3.6 ms
  apart), 21 at a 128-frame period, 12 at 256, 5 at 528, 4 at 1024 in a 4096-frame buffer,
  i.e. about buffer/period, before and after 0006. Cause (from the source, not yet confirmed
  on hardware): `ACC_BM0_PRD` is the engine's running descriptor pointer (`cs5535audio_pm.c`
  saves it with `read_prd` for exactly that reason), `hw_params` programs it once with the
  address of the closing JMP descriptor (`cs5535audio_build_dma_packets`), and the XRUN
  path is `snd_pcm_stop(XRUN)` -> `TRIGGER_STOP` (`ACC_BM0_CMD = 0`) followed by
  PREPARE + START from userspace on the same hw_params (`sound/core/pcm_native.c`,
  `snd_pcm_xrun`/`snd_pcm_prepare`). So after an XRUN the engine restarts at the descriptor
  it had reached while ALSA's `hw_ptr` restarts at 0 (`snd_pcm_do_reset`): the first
  `.pointer` reads are somewhere in the ring, `hw_ptr` lands beyond `appl_ptr`
  (`snd_pcm_update_hw_ptr0` -> `snd_pcm_update_state`: `avail >= stop_threshold`), the stream
  stops again, and each retry advances the engine by about a period until the position wraps
  below the amount written. A close/reopen goes through `hw_params` and resets the pointer,
  which is why userspace falls back to it. The patch re-points the engine at the JMP
  descriptor from `prepare` (bus master disabled) and reads `ACC_BMx_STATUS` to acknowledge a
  pending EOP / EOP-error flag (the CS5535 pauses the bus master when a second EOP arrives
  before the first is read). Not verified: the datasheet behaviour of the PRD register and of
  the status flags (no datasheet here, no hardware), so the run after this patch must show
  one underrun per stall and `action=prepare rec_frames=2` in the stats. Userspace does not
  depend on it (`olpc-run` reopens the PCM when a second underrun follows within the same
  incident).
- `SND_DEBUG` + `SND_PCM_XRUN_DEBUG`: `/proc/asound/card0/pcm0p/xrun_debug`
  (bit 0 = 1: log XRUN and hw_ptr anomalies, bit 1 = 2: + stack dump, bit 2 = 4: jiffies check
  that also corrects a position that jumps; the boot test uses 5 = basic + jiffies check) and `xrun_injection`; `olpc-run --xrun-debug N` writes it. The entry belongs to the stream
  (`pcm0p/xrun_debug`, created in `snd_pcm_stream_proc_init`), not to the substream (`pcm0p/sub0/`). The first
  hardware runs never applied it: `audio_open` did not store the device path, so the path
  could not be derived and the call failed silently; fixed.
- `CONFIG_HZ` stays 100. The 4 ms average lateness of sleeps is not the tick:
  `nanosleep`/`poll` timeouts are hrtimers, but `timer-cs5535.c` registers the MFGPT clock
  event with `clockevents_config_and_register(..., 2048 Hz, min_delta 0xF, ...)`, so no
  timer can be programmed closer than 15/2048 s = 7.3 ms and every shorter sleep rounds up
  to that. HZ does not change it. HZ=250 would only make scheduler-tick preemption and
  jiffies-based ALSA checks finer (about +0.25 % CPU in ticks on a 433 MHz in-order core, no
  other cost; in the early-boot periodic mode the MFGPT would run at 256 Hz because
  2048/HZ is an integer division). Left at 100 so that the effect of 0006 is not mixed with
  a second change; lowering min_delta (a few ticks, ~1 ms) is the change that would
  actually fix short sleeps, and needs hardware testing.
- `CRYPTO_MANAGER_DISABLE_TESTS=y`: the algorithm self-tests ran inside
  `crypto_algapi_init` (late_initcall) and cost seconds on the Geode (1.8 s even
  under QEMU, ~5.5 s of silence on hardware). Also fixes the `geode-aes` fallback
  self-test noise; `CRYPTO_AES/CBC/ECB` are built in as geode-aes fallbacks.
- `LOG_BUF_SHIFT=19` (512 KiB). `SND_PROC_FS` (/proc/asound), `RTC_INTF_DEV/SYSFS/PROC`
  (/dev/rtc0, /sys/class/rtc) were missing because allnoconfig turns them off.
- Modules: `cafe_nand`, `usb8xxx` (+`libertas`, `cfg80211`), `geodewdt`, `geode-aes`,
  `cafe_ccic` (+`mcam-core`, videobuf2-*), `ov7670`.
- lxfb has no acceleration and no panning, and the framebuffer is mapped
  uncached: each console line scrolls by reading video memory. Keep the console
  quiet (`loglevel=4`); /dev/kmsg still records everything.

## USB: Sony DualShock 4 (054c:05c4) wired, no input device

Observed (logs/20261119-105316/boot-0001/dmesg.final.txt): the pad enumerates on the OHCI
root port `usb 2-4` at 2.0 s, `usb 2-4: New USB device found` at 2.28 s, and at 22.45 s
`usbhid 2-4:1.0: can't add hid device: -110` / `usbhid: probe of 2-4:1.0 failed with error
-110`. No `sony` line. Note the failing interface is **1.0**, not 3 (a DualSense has its HID on
interface 3; this DS4 revision has it on 0).

Which call returns -110 (Linux 6.1.192):

- `usbhid_probe()` (`drivers/hid/usbhid/hid-core.c:1356`) prints that message at line 1446
  when `hid_add_device()` (line 1443) fails with anything but -ENODEV.
- `hid_add_device()` (`drivers/hid/hid-core.c:2836`) can only return an error from
  `hdev->ll_driver->parse(hdev)` (line 2863) or from `device_add()` (line 2889). A failing
  *driver* probe does not propagate out of `device_add()`, so hid-sony's own errors
  (`parse failed`, `hw start failed`, the DS4 feature-report errors) could never produce
  this line: consistent with "no sony message at all".
- `.parse` is `usbhid_parse()` (`hid-core.c:980`). Its only transfers are `hid_set_idle()`
  (line 1050, result ignored) and `hid_get_class_descriptor()` for the HID report
  descriptor (line 1052-1054, error -> `goto err`, the -110 is returned as is).
- `hid_get_class_descriptor()` (`hid-core.c:663-676`) does
  `usb_control_msg(..., USB_CTRL_GET_TIMEOUT)` up to 4 times while `result < size`;
  `USB_CTRL_GET_TIMEOUT` is 5000 ms (`include/linux/usb.h:1855`); a timeout is -ETIMEDOUT
  (`drivers/usb/core/message.c:77`). 4 x 5 s = 20 s: the log shows 22.453 - 2.285 = 20.17 s.
  SET_IDLE (5 s timeout too, `USB_CTRL_SET_TIMEOUT`) must therefore have returned at once
  (a STALL, normal for the DS4), otherwise the total would be 25 s. So the pad answers
  control requests to the interface, but the data stage of GET_DESCRIPTOR(HID report) -- a
  507-byte reply, 8 packets of 64 -- never completes. Nothing else in the probe path can
  give -110.

What is NOT known is why it times out here and not behind the PC's EHCI hub (full-speed
traffic through a transaction translator): see the ranked hypotheses below; the web search
found no report of this exact symptom on OHCI, only the same message for a DualSense on a PC
(`usbhid 1-8:1.3: can't add hid device: -110`, reddit r/linux_gaming "Problems with Dualsense
controller on Manjaro", content not retrievable), so there is no known upstream fix.

Checked in the source: `ohci_pci_quirks[]` in `drivers/usb/host/ohci-pci.c` has entries for
AMD756 (1022:740c), OPTi, any National Semiconductor vendor id (acts only next to an NSC87560
SuperIO), ZFMicro, Toshiba SCC, NEC, AMD700-era SB chipsets and Loongson/QEMU: nothing
matches the CS5536 OHCI, so no chipset quirk is applied to it upstream (and no
OLPC-specific OHCI patch exists in this tree; OLPC's own kernels were not consulted). The
`hid_lookup_quirk` tables have no DS4 entry for usbhid (`HID_QUIRK_NOGET` etc. only change
later traffic, not the descriptor fetch).

Hypotheses, most likely first (all unverified):

1. The pad is not ready or is browning out at ~2.3 s after VBUS: it answers the standard
   requests (enumeration) but not a long class-specific IN transfer. Discriminator: the
   `olpc-usbdiag` attempt 1 (rebind at >= 45 s uptime): success => timing; the usbmon capture
   of that rebind shows whether the same GET_DESCRIPTOR completes.
2. Power: a wired DS4 charging its battery draws up to ~500 mA (`bMaxPower` is logged) from a
   root port that the XO-1 limits. A VBUS sag would hit the pad mid-transfer. Discriminator:
   attempts that fail repeatedly while `lsusb -v` (user-space control transfers, logged with
   timing in `usbdiag/<dev>/lsusb-v.txt`) also fails; the same pad with a full battery; a
   second boot with the pad plugged in after boot.
3. The CS5536 OHCI mishandles this multi-packet control IN (8 x 64 B) although it handles the
   4-packet configuration descriptor. Discriminator: `usbdiag/<dev>/ohci-*-async-*.txt` and
   `registers` (OHCI debugfs; TD condition codes, HcRhDescriptorA power switching mode) and the
   usbmon status of the failing URB; `lsusb -v` failing the same way at any time.
4. USB autosuspend of the pad racing the probe (default delay here is 2 s, `CONFIG_PM=y`).
   The usb core holds a PM reference during the interface probe, so this is unlikely;
   `usbcore.autosuspend=-1` removes the variable.
5. "PS button needed": that applies to the DS3/Sixaxis (hid-sony does an operational-mode
   request); the DS4 enumerates and reports without it. Not related to a failure before
   hid-sony is even called.

What was added for the next hardware run:

- `CONFIG_USB_MON=y` and `CONFIG_DEBUG_FS=y` (also in `config.frag`): usbmon text API at
  `/sys/kernel/debug/usb/usbmon/<bus>u`, `/sys/kernel/debug/usb/devices` and the OHCI debugfs
  files. `olpc-usbdiag` mounts debugfs itself and unmounts it again.
- `olpc-usbdiag` (`rootfs/overlay/usr/local/sbin`, service of the same name in the default
  runlevel, background, nice 19, silent no-op without a VID 054c device): log in
  `<bootdir>/usbdiag/`. It waits for 45 s of uptime (the 20 s failure is over), dumps the
  state, runs `lsusb -v` on that device only, starts usbmon on its bus and tries, until an
  input device named `*Wireless Controller*` appears: (1) rebind of the HID interface, (2)
  `power/control=on` + rebind, (3) `authorized` 0->1, (4) `bConfigurationValue` 0->n + bind,
  (5) power-cycle of its root-hub port (sysfs `disable`), logging result and duration of each.
  Attempt (3) is NOT a device reset in 6.1: `usb_authorize_device()` (`hub.c:2739`) only
  unconfigures and sends SET_CONFIGURATION again; the real re-enumeration is attempt (5),
  which is skipped when another device shares the bus and may leave the pad detached until
  replugged when the controller ignores port power. Only the Sony device and its port are
  touched.
- Patch 0008 (hid-sony): for a wired DS4 only, failing feature reports 0x81/0x02/0xA3 are
  warnings instead of probe errors, the remaining reads are skipped after the first failure,
  and identity calibration is installed so `dualshock4_parse_report()` is safe. This cannot
  help with the observed failure (it happens before hid-sony is called); it protects the
  next failure point, because the same control-IN weakness could hit the feature reads
  (37/49-byte replies, 5 s each) once the descriptor fetch works.
- No usbhid patch: nothing in the source supports a specific fix for the descriptor fetch
  (a retry/delay would only repeat what attempt 1 does from user space). If attempt 1 or 2
  succeed, the follow-up is a deferred, retried `hid_add_device()` for 054c:05c4 in usbhid.
- Command line `usbcore.autosuspend=-1` (boot/olpc.fth): disables USB autosuspend globally
  (default 2 s). Safe for the boot stick (it only stays powered/active, which mass storage
  prefers); costs a little power.

### Update (boot 115836): the pad leaves the bus by itself, before any output report

Two plug-in cases on the same pad (`logs/20261119-115836/boot-0001/kmsg.log`):

- Plugged before power-on: `usb 2-4: new full-speed USB device number 2` at 1.94 s and then no
  line at all (no `New USB device found`, no error). `dev_err` lines of `hub.c` only appear
  when a try *ends*; the retries (`GET_DESCRIPTOR` 5 s timeouts, up to 4 port inits) are silent
  and the unplug at 90 s ended them with -ENODEV (also silent). A device that was never
  registered gives no `USB disconnect` line either.
- Hot-plugged at 90.69 s: enumeration and the report descriptor worked, then hid-sony's
  *first* control transfer, `GET_REPORT 0x81`, returned **-62 (ETIME) after ~0.4 s**. On OHCI
  `-ETIME` is condition code 5, "device not responding" (a missing handshake), which the
  controller reports quickly; a stalled-but-alive device gives -110 after 5 s as in the earlier
  boot. So the pad had already stopped answering *before hid-sony sent anything*; the
  disconnect line 0.8 ms after `input,hidraw0` is just the hub thread processing the port
  change after probe returned. The light-bar/LED output report (queued by `sony_leds_init()` ->
  `sony_set_leds()` -> `sony_schedule_work()`, sent by `sony_state_worker()` ->
  `dualshock4_send_output_report()`, `hid-sony.c:2296`) is sent *after* that point, so it cannot
  explain this drop. Over-current: the CS5536 OHCI is set up for per-port over-current reporting
  (`ohci-hcd.c:675`), `ohci-hub.c` clears OCIC/POCI on request, but nothing prints a line when
  it fires and `hub.c` only warns for the "over power budget" case; the bit is visible in the
  debugfs `registers` file (`POCI`, `OCIC`, `roothub.status OCI`), which `olpc-usbdiag` now
  records.

What hid-sony (6.1.192, with 0008) does after a wired DS4 probes, in order
(`sony_probe()` 3031, `sony_input_configured()` 2776):

1. `sony_set_device_id()`; `sony_check_add()` (2613): feature report 0x81 (control IN). With
   0008 a failure is a warning and the other two feature reads are skipped.
2. `sony_allocate_output_report()`; DS4 branch (2887): `dualshock4_get_calibration_data()`
   (0x02) and `dualshock4_get_version_info()` (0xA3) unless skipped, two sysfs attributes,
   `sony_register_touchpad()` and `sony_register_sensors()` (two extra input devices),
   `sony_init_output_report()`, which arms `state_worker`.
3. `sony_leds_init()` (2129; `SONY_LED_SUPPORT` covers DS4): `dualshock4_set_leds_from_id()`,
   global LED on, `sony_set_leds()` queues the worker -> **output report 0x05** (rumble 0,
   light bar colour from the device id, blink 0) as an interrupt-OUT transfer
   (`hid_hw_output_report`); four LED class devices (red/green/blue/global).
4. `sony_battery_probe()` (2496): `power_supply` `sony_controller_battery_*`; its properties
   come from the last input report, no polling traffic. Then `hid_hw_open()`: the interrupt IN
   pipe starts polling immediately (without a battery it would start at the first open of the
   input/hidraw node).
5. `sony_init_ff()`: a stub, `CONFIG_SONY_FF` is off.
`usbhid` itself sends no GET_REPORT at probe (`usbhid_init_reports()` is only called from
hiddev, not configured here).

#### Patch 0009: `hid_sony.ds4_usb_minimal`

Boolean module parameter, default 0 (behaviour unchanged). For a **wired DualShock 4 only**,
when set: no feature report at all (no 0x81/0x02/0xA3, no duplicate-connection check, no
`firmware_version`/`hardware_version` files); no output report ever (the worker is never armed,
so `sony_schedule_work()` is a no-op; no `sony_leds_init()`, so the light bar is left as the
pad powers up, no LED class devices, no rumble); no battery `power_supply`; no `hid_hw_open()`
at probe (the IN pipe is polled only while something has the input or hidraw node open, e.g.
`olpc-run`/`olpc-chordd`); no touchpad and no motion-sensor input device
(`dualshock4_parse_report()` skips that data when they do not exist). The main
`Wireless Controller` input device (buttons, sticks, triggers, D-pad) and hidraw are created as
before; `dmesg` shows `ds4_usb_minimal: input only, no output or feature reports`.

Exact spelling: the driver is built in (`CONFIG_HID_SONY=y`), its module name is `hid_sony`, so on
the kernel command line it is

    hid_sony.ds4_usb_minimal=1

(the same mechanism as the already used `usbcore.autosuspend=-1`). It is also writable at run
time, `echo 1 > /sys/module/hid_sony/parameters/ds4_usb_minimal`, and read at probe, so it
applies to the next plug-in.

Two-boot test matrix (no rebuild): `boot/olpc.fth` is unchanged and boots with the parameter
unset. For the second boot edit the FAT file `boot/olpc.fth` on the stick from a PC (or re-flash
a build) and append ` hid_sony.ds4_usb_minimal=1` inside the quotes of the one long line
`" console=tty0 ... i8042.dumbkbd=1" to boot-file`, i.e. the end becomes
`... i8042.dumbkbd=1 hid_sony.ds4_usb_minimal=1" to boot-file`. Boot A (default) and boot B
(parameter) with the same pad, same port, same power source, battery state noted.

How to read the results:

- Pad still vanishes in boot B with `usb 2-4: USB disconnect` and no `sony` warning: the drop
  does not depend on anything hid-sony does (it only sends nothing at all now) -> power or the
  link. Compare the usbmon ring: last lines before the disconnect are IN/Ci polls or
  descriptor reads that end in `-71` (EPROTO) / `-62` / `-108`, no OUT transfer.
- Pad works in B but not in A: the feature/output traffic matters; the usbmon ring of A shows
  the last transfer before the drop (an `Co:`/`Io:` 0x05 report vs a `Ci:` 0x81 read).
- In both boots `registers` shows `POCI` or `OCIC` in the port word around the event, or
  `roothub.status` `GLOBAL-OVER-CURRENT`: the port supply tripped; repeat with the XO on AC and a
  charged pad battery, and on the other root port.
- Clean disconnect (port word `CONNECTED` clear, no over-current bits, no `RESET`) at the time
  of the drop, XO battery `voltage_now`/`current_now` (in `state.txt`) unremarkable: the pad
  (or its cable/contacts) disconnected itself; a brown-out of the pad's own supply from
  charging inrush is still the leading explanation and cannot be told from a pad reset
  without a scope.

#### `olpc-usbdiag` (rewritten as a hot-plug watcher)

Persistent background loop (service `olpc-usbdiag`, `nice 19`, poll 2 s; 6 s while a game runs):

- usbmon text capture of the OHCI bus (bus found from sysfs: root hub whose PCI parent is bound
  to `ohci*`; bus 2 here) from service start, in tmpfs (`/run/olpcboy/stage/usbmon`) as a ring of
  8 x 256 KB chunks (`split` + `cat`, blocking reads, no CPU while the bus is idle); paused
  during a game (set `OLPC_USBDIAG_MON_IN_GAME=1` in the environment to keep it).
- Events come from the kernel-log copy `<bootdir>/kmsg.log` by sequence number (history at start
  is logged but takes no snapshot): `new <speed> USB device`, `New USB device found`, `USB
  disconnect`, enumeration errors, `sony`/`usbhid` error lines while a Sony device is present.
  A port with no result 10 s after `new ... USB device` is "stuck".
- Snapshots (6 per boot) in `<bootdir>/usbdiag/snap-NN-<reason>/` on: Sony appear, Sony
  disconnect, stuck port, `unable to enumerate`, a new device attaching to a still-pending port
  (the replug in the first case above), over-current line, recovery result. Each has
  `usbmon-bus2.txt.gz` (`zcat`) and `state.txt`: OHCI `roothub.a/b/status` and every port word
  decoded (CONNECTED ENABLED SUSPENDED OVER-CURRENT RESET POWERED + change bits), `async` and
  `periodic` schedules (the TDs of a stuck control transfer), sysfs attributes of the root hubs,
  their `power/` and port directories, device list, power supplies (XO battery state), input
  device names, usb/hid kernel lines. The stuck snapshot first does `echo w >/proc/sysrq-trigger`
  (blocked tasks with stacks, to the kernel log; `MAGIC_SYSRQ` and frame-pointer unwinding are
  on): this shows where the hub thread waits.
- Hub retry visibility: `CONFIG_DYNAMIC_DEBUG` is off and the retry/`debounce`/`ep0 maxpacket`
  messages of `hub.c` are `dev_dbg`, compiled out. No kernel change was made: usbmon shows every
  URB with its status and timing (a silent retry is a visible repeated `S Ci`/`C Ci -110`
  pair), and sysrq-w gives the wait point. Enabling dynamic debug for the whole kernel
  (size, many call sites) or patching `hub.c` was the more invasive alternative.
- Recovery (rebind, pm-on, authorized, config, port-cycle) only for a Sony device that is
  present, has had no `Wireless Controller` input device for 30 s and only while no game runs,
  once per plug-in; never when the pad works.
- Silent when idle: no files are written until the first event. Test:
  `docker run --rm -v $PWD:/work -w /work olpc-boy-builder sh scripts/test-olpc-usbdiag.sh`.

## JFFS2 on NAND and the NAND simulator

- `CONFIG_JFFS2_FS_WRITEBUFFER=y`: JFFS2 cannot run on NAND (CAFE) without the write buffer
  (page-sized writes); the normal image never mounts the NAND, so this only matters for the
  NAND write-test image.
- `CONFIG_MTD_NAND_NANDSIM=m` (+ `CONFIG_MTD_NAND_ECC_SW_HAMMING=y`, which nandsim needs): a
  simulated NAND for the QEMU run of the NAND write-test image (`make qemu-nandtest`). Never
  loaded by the normal image.
