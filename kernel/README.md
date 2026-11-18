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
- Not verifiable without hardware: that probe succeeds on a real XO-1 (needs the
  scx200_acb I2C bus to detect the DCON at 0x37 and fb registered first).
