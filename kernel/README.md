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
