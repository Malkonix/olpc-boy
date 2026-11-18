# olpc-boy

Minimal Alpine Linux 3.18 (x86) for an OLPC XO-1 (AMD Geode LX, i586 + MMX/3DNow/CMOV, **no SSE**).
The built-in keyboard is dead, so the USB stick must boot unattended: Open Firmware runs
`boot/olpc.fth`, which loads the kernel; the kernel's embedded initramfs mounts the partitions by label.

## Loop

```
make builder                       # once: docker image olpc-boy-builder
make image                         # initramfs -> kernel -> rootfs -> SSE check -> out/olpc-boy.img
make qemu                          # smoke test (CPU without SSE), logs from the stick copy
make flash                         # no DEV: lists removable USB disks
make flash DEV=/dev/sdX FLASH_ARGS=--remember   # first time pairs the stick's serial (.flash-device)
make flash DEV=/dev/sdX            # later flashes; refuses any other stick
# plug into the XO-1, power on, wait, power off, plug back into the PC
make logs                          # -> ./logs/<timestamp>/ + per-boot summary
```

Scripts can be used directly: `scripts/flash.sh <img> [dev] [--remember] [--dry-run]`
(`--dry-run` runs all guards and prints the `dd` it would run; nothing is touched),
`scripts/collect-logs.sh [dev|mountpoint]`, `scripts/qemu-test.sh [seconds]`,
`scripts/check-sse.sh <rootfs.tar|dir>` (exit 1 only on `ERROR`; reviewed CPUID-guarded files are listed in `scripts/sse-allow.txt`; see header for the
ERROR/WARN rules; NOPL = multi-byte NOP is reported as a warning).

`make clean` keeps the kernel source cache (`out/src`); `make distclean` removes it too.

## Disk layout (MBR, 1 MiB aligned, image ~460 MiB)

| # | FS | type | flags | label | contents |
|---|----|------|-------|-------|----------|
| 1 | FAT32, 256 MiB, 2 KiB clusters | 0x0c | bootable | `OLPCLOG` | `boot/OLPC.FTH`, `boot/BZIMAGE`, `logs/`, `roms/` |
| 2 | ext4 | 0x83 | | `OLPCROOT` | rootfs.tar content + 200 MiB free |

Names on the FAT are 8.3 upper-case short names (Open Firmware matches case-insensitively, so
`last:\boot\bzImage` finds `BZIMAGE`). The kernel gets no `root=`; its initramfs finds both partitions by label.

## Open Firmware evidence

Primary source: the Open Firmware tree at github.com/quozl/openfirmware (OLPC's fork), fetched raw.

- **VERIFIED** The boot file is run as Forth text only if it begins with `\ ` (or `purpose: ` / `id: `);
  otherwise "Unrecognized program format" (`ofw/core/ofwcore.fth`, `execute-buffer`). Hence the first
  line `\ OLPC boot script`. Buildroot's own script starts with `\ OLPC XO boot script`; same rule.
- **VERIFIED** XO-1 (Geode LX) default `boot-device` is
  `disk:\boot\olpc.fth sd:\boot\olpc.fth nand:\boot\olpc.fth /prober /usb/ethernet /usb/wlan`
  (`cpu/x86/pc/olpc/fw.bth`), so a stick carrying `\boot\olpc.fth` is found without typing, and the
  script is expected to set the kernel path/cmdline itself (default `boot-file` is empty).
- **VERIFIED** Which partition of a disk: with no explicit partition number, the FAT driver picks the
  **first partition whose boot flag is 0x80, else partition 1** (`ofw/disklabel/methods.fth`,
  `fdisk-map` + `bootable?` in `ofw/fs/fatfs/partition.fth`). Our FAT partition is both first and
  bootable, so either rule selects it.
- **VERIFIED** Partition types 0x0b and 0x0c (FAT32 CHS/LBA) are recognised, and the FAT code handles
  FAT12/16/32 (`fat16`/`fat32` constants in `ofw/fs/fatfs/bpb.fth`). FAT16 would work too, but FAT32 type
  0x0c is fine and allows 256 MiB. Directory lookup compares upper-cased 8.3 names
  (`ofw/fs/fatfs/lookup.fth`), so long names are avoided.
- **UNVERIFIED** That the alias `disk:` enumerates USB before the SD slot on every Q2F build (Gentoo
  wiki states USB, then SD, then internal NAND for `/boot/olpc.fth`; not confirmed from source). The
  script uses `last:` for the kernel, which is independent of that order.
- **UNVERIFIED** The exact Q2F20 firmware revision matches the quozl master tree; wiki.laptop.org
  returns 401, so no wiki page could be cited.
- Ramoops/`memmap=` reservation was deliberately **left out** of the command line: a safe RAM address
  beside Open Firmware's own top-of-memory allocations on the 256 MB XO-1 could not be verified, and a
  wrong reservation could corrupt the boot. Add it later once the kernel's `PSTORE_RAM` is confirmed.

## QEMU smoke test notes

QEMU 10 has no `pentium-mmx` CPU model, so `qemu-test.sh` uses `pentium,+mmx,+cmov` (override with
`QEMU_CPU`). QEMU has no Open Firmware: the kernel is booted with `-kernel` and the same command line
that `olpc.fth` sets, plus `console=ttyS0 olpcboy.nobuttons`. Only a boot on the real XO-1 verifies
`olpc.fth` itself.
