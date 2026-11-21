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

## Running from the internal NAND

The XO-1 has 1 GiB of NAND (CaFe controller; 2048+64 byte pages, 128 KiB erase blocks, 8192 blocks,
13 known bad blocks; the bad block tables live in blocks 8188..8191 and are never written:
`cafe_nand` is always loaded with `allow_bbt_write=0`).  olpc-boy can be installed there so that it boots
without the stick, like the original OS: the **whole NAND is one JFFS2** (made with `mkfs.jffs2` +
`sumtool`, so the kernel mounts from summary nodes instead of scanning 1 GiB at 1.6 MB/s).
Open Firmware boots a USB stick before the NAND, so a stick always wins: that is the way back in.

Three separate boot sticks (the normal `make image` stick is unchanged):

| image | `make` target | does |
|-------|---------------|------|
| `out/olpc-boy-install.img` | `install-image` | checks the NAND against the backup, writes olpc-boy to it, verifies |
| `out/olpc-boy-restore.img` | `restore-image` | writes the original OS back from the backup stick (volume `OLPCNAND`) |
| `out/nand-root.jffs2` | `nand-image` | the NAND content itself (inside the install image); built from `out/rootfs.tar` |

```
make image            # normal rootfs first (the NAND content is built from it)
make install-image restore-image     # needs the NAND backup (NAND_BACKUP_DIR, default ~/olpc-nand-backup/run2-nandbackup-001, mounted read-only)
make install-test     # logic tests with a fake NAND (install, every abort path, re-install gate, restore)
make qemu-install     # QEMU + nandsim: install, boot from the NAND, restore, restore abort
scripts/flash.sh out/olpc-boy-install.img /dev/sdX     # same guards as `make flash` (pair the stick with --remember)
```

### NAND layout and boot

* `mtd0 "olpcboy"` = erase blocks 0..8187 (writeable, the JFFS2), `mtd1 "bbt"` = blocks 8188..8191 (read-only).
  Both come from the kernel command line `mtdparts=cafe_nand:1048064k(olpcboy),-(bbt)ro`; the installer and the
  initramfs refuse to write/mount read-write unless sysfs shows exactly that layout.
* JFFS2 content: `/boot/olpc.fth` (`boot/olpc-nand.fth`), `/boot/bzImage`, `/boot/initrd.img`, `/.olpcboy-nand` (marker),
  the root filesystem of the USB image, and `/data`.  Open Firmware reads `nand:\boot\olpc.fth`, loads
  `nand:\boot\bzImage` and `nand:\boot\initrd.img` (`" ..." to ramdisk`).  The initrd only holds `cafe_nand.ko`
  (the driver is a module and has to be loaded before the root exists); the kernel merges it over the embedded initramfs.
* Kernel flag `olpcboy.nand`: the initramfs loads the driver, checks the layout, mounts `mtd0` as JFFS2 (`rw,noatime`),
  creates the boot log directory, then binds `/data/olpclog` on `/mnt/olpclog` and `/data` on `/mnt/internal` before
  `switch_root`.  Without the flag nothing changes (USB boot).
* The stick's role is played by `/mnt/olpclog` (a directory of the root): `WIFI.CFG`, settings, `ssh.off` and
  `logs/boot-NNNN` (newest 20 kept) are where every script already looks.  Read the boot logs over SSH from
  `/mnt/olpclog/logs/` (or `/var/log/olpc-current/`, a link to the current one).  On the NAND the log is flushed every
  30 s instead of every 3 s (JFFS2 programs partial pages on every sync).
* Games: `/data/roms/{gb,gbc,nes,saves,states}` and `/data/covers`, visible as `/mnt/internal/roms` (a real entry in
  `/proc/mounts`, so the launcher and `olpc-covers` scan it like the SD card).  `~/ROMs` points at the SD card's `roms`
  when an SD card is mounted at boot, otherwise at the internal `roms` (service `olpc-nandroms`).
* The SSH host key lives in `/etc/dropbear` of the JFFS2 root, so it persists.

### Installing (about 12 minutes, nothing to type)

1. Plug `olpc-boy-install.img`'s stick in, power on.  It needs no keyboard; the stick carries `WIFI.CFG` and `roms/` if the
   build had them (copied into `/data`, the contents of `WIFI.CFG` are never read or logged).
2. Progress is on the splash; the result is a big screen.  **INSTALLED**: remove the stick, power off and on.
   **FAILED**: the screen says what state the NAND is in; the log is `logs\boot-NNNN\INSTALL.TXT` on the stick.
3. Steps: layout check; **precheck** (whole NAND vs the per-block SHA-256 of the backup, ~10.5 min: if anything differs
   the installer aborts and nothing is written); `flash_erase -j`; `nandwrite -p` of the image (bad blocks skipped);
   read-back compare of the written region; JFFS2 mounted read-only and every file checked against the SHA-256 manifest;
   settings, saves, states, covers and ROMs copied to `/data`; unmount.
4. Re-installing over an existing install: the backup no longer describes the NAND, so the precheck is replaced by a
   marker check.  Create an empty file `REINSTALL` in the stick's root (or add `olpcboy.reinstall` to the stick's
   `olpc.fth`); it only applies when erase block 0 carries the olpc-boy marker or is erased (an interrupted install).
   On any other NAND the flag is ignored and the backup precheck decides.

### Going back to the original OS

1. Plug in the `OLPCNAND` backup stick (vfat; folder `nandbackup-NNN/` with `NANDCOPY.DAT` and `SHA256SUMS`) **and** the
   `olpc-boy-restore.img` stick; power on.  The backup stick is mounted read-only and only read.
2. The tool checks `NANDCOPY.DAT` against `SHA256SUMS`, runs `flash_erase -j` on `mtd0`, `nandwrite -p NANDCOPY.DAT`
   (what Open Firmware's `nd-copy-nand` does: the blocks one after the other into the good blocks, the rest erased with
   cleanmarkers), reads the whole partition back and compares, mounts the result read-only as a sanity check.
   About 15 minutes.  **RESTORED**: remove both sticks, power off and on: the original OS boots.
3. **Open Firmware only (no Linux needed):** copy `NANDCOPY.DAT` to the root of a FAT USB stick, get to the `ok` prompt
   (the way the backup was made), and run
   `nd-copy-nand u:\NANDCOPY.DAT` (optionally `verify-nand u:\NANDCOPY.DAT` afterwards).  It erases every good block first.
   Check the stick's copy with `sha256sum -c SHA256SUMS` on a PC first.

### Expected times on the XO-1 (from the nandtest measurements)

| step | estimate |
|------|----------|
| precheck: read + hash 1 GiB | ~10.5 min (1.6 MB/s) |
| erase 1 GiB (`flash_erase -j`) | ~20 s |
| write the image (~32 MB) | ~7 s at 5.5 MB/s (plus erase blocks skipped) |
| read-back of the image | ~20 s |
| mount + SHA-256 of every file (~58 MB) | 30-60 s |
| copy games (JFFS2 write 1.1 MB/s) | ~1 s per MB of ROMs |
| whole install | ~12 min; with REINSTALL ~2 min |
| restore: verify `NANDCOPY.DAT` (USB) ~1 min, erase 20 s, write 879 MB ~2.7 min, read back 1 GiB ~10.5 min | ~15 min |

### Open Firmware notes (NAND)

* **VERIFIED** (OLPC's Open Firmware tree, `ofw/fs/jffs2/`): the reader understands summary nodes and the compressions
  none, rtime and zlib only (no LZO), which is why `mkfs.jffs2` runs with its defaults (LZO disabled; the build checks).
* **VERIFIED** `nand:\boot\olpc.fth` is in the default `boot-device` list; kernel and initrd of the original OS are
  loaded with `" nand:\boot\vmlinuz" to boot-device` / `" nand:\boot\initrd.img" to ramdisk`.
* **UNVERIFIED on hardware** that this firmware reads our JFFS2 (node size 2 KiB, summary, no cleanmarkers on written
  blocks) from `nand:`, and how long it scans a 1 GiB device before running `olpc.fth`.  The tools of the original OS use
  the same image format, and Linux's mount of exactly this image is verified on the XO-1 (nandtest).
* **UNVERIFIED on hardware** how long Linux takes to mount the 1 GiB JFFS2 (empty blocks are checked through their
  cleanmarker only); the first boot log (`/mnt/olpclog/logs/boot-0001/timeline.log`, `initramfs:` lines) shows it.

## QEMU smoke test notes

QEMU 10 has no `pentium-mmx` CPU model, so `qemu-test.sh` uses `pentium,+mmx,+cmov` (override with
`QEMU_CPU`). QEMU has no Open Firmware: the kernel is booted with `-kernel` and the same command line
that `olpc.fth` sets, plus `console=ttyS0 olpcboy.nobuttons`. Only a boot on the real XO-1 verifies
`olpc.fth` itself.
