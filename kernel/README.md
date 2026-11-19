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
