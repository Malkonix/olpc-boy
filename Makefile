# olpc-boy top-level build.  Everything heavy runs inside the olpc-boy-builder
# container (Alpine 3.18 x86); flash/qemu/logs run on the host.
#
#   make builder      build the docker image (once)
#   make image        initramfs -> kernel -> rootfs -> SSE check -> out/olpc-boy.img
#   make qemu         smoke-test the image in QEMU
#   make flash DEV=/dev/sdX [FLASH_ARGS=--remember]
#   make logs         copy logs/ from the stick into ./logs/<timestamp>/
#   make clean        remove outputs but keep the kernel source/build cache
#   make distclean    remove all of out/ (forces a full kernel rebuild)

BUILDER  := olpc-boy-builder
UID_     := $(shell id -u)
GID_     := $(shell id -g)
DOCKER   := docker run --rm -v $(CURDIR):/work -w /work
# kernel build as the host user, everything else as container root (+ ownership fix-up)
RUN_USER := $(DOCKER) -u $(UID_):$(GID_) $(BUILDER)
RUN_ROOT := $(DOCKER) -e HOST_UID=$(UID_) -e HOST_GID=$(GID_) $(BUILDER)

# File-based prerequisites so a small change does not rebuild the world.
INITRAMFS_SRC := $(shell find initramfs -type f 2>/dev/null) scripts/mkinitramfs.sh
KERNEL_SRC    := $(shell find kernel -type f 2>/dev/null)
ROOTFS_SRC    := $(shell find rootfs src -type f 2>/dev/null) scripts/mkrootfs.sh $(wildcard ssh_authorized_keys)

.PHONY: nand-image install-test install-rootfs install-check install-image restore-image qemu-install nandtest-test nandtest-rootfs nandtest-check nandtest-image qemu-nandtest backup-test backup-rootfs backup-check backup-image qemu-backup olpc-splash-test chordd-test fbbench-test olpc-run-test olpc-menu-test all builder initramfs kernel rootfs check image qemu flash logs clean distclean
all: image

builder:
	docker build -t $(BUILDER) docker/

initramfs: out/.initramfs.stamp
kernel:    out/kernel/bzImage
rootfs:    out/rootfs.tar
check:     out/.check.stamp
image:     out/olpc-boy.img

out/.initramfs.stamp: $(INITRAMFS_SRC)
	$(RUN_ROOT) bash scripts/mkinitramfs.sh
	@touch $@

# The kernel embeds out/initramfs/, hence the stamp dependency.
out/kernel/bzImage: out/.initramfs.stamp $(KERNEL_SRC)
	$(RUN_USER) bash kernel/build.sh
	@touch $@

# Modules (out/kernel/version marks the kernel build) go into the rootfs.
out/rootfs.tar: out/kernel/bzImage $(ROOTFS_SRC)
	$(RUN_ROOT) bash scripts/mkrootfs.sh
	@touch $@

out/.check.stamp: out/rootfs.tar scripts/check-sse.sh
	$(RUN_USER) bash scripts/check-sse.sh out/rootfs.tar
	@touch $@

out/olpc-boy.img: out/kernel/bzImage out/rootfs.tar out/.check.stamp boot/olpc.fth scripts/mkimage.sh
	$(RUN_ROOT) bash scripts/mkimage.sh

qemu: out/olpc-boy.img
	bash scripts/qemu-test.sh

flash: out/olpc-boy.img
	@test -n "$(DEV)" || { echo "usage: make flash DEV=/dev/sdX [FLASH_ARGS=--remember]"; bash scripts/flash.sh out/olpc-boy.img; exit 1; }
	bash scripts/flash.sh out/olpc-boy.img $(DEV) $(FLASH_ARGS)

logs:
	bash scripts/collect-logs.sh $(LOGDEV)

# out/ holds root-owned files from the container, so delete from inside it.
clean:
	$(RUN_ROOT) sh -c 'rm -rf out/olpc-boy.img out/olpc-boy-nandbackup.img out/backup-rootfs.tar out/backup-work out/backup-work.test.log out/qemu-backup-* out/olpc-boy-nandtest.img out/nandtest-rootfs.tar out/nand-root.* out/nand-initrd*.img out/install-rootfs.tar out/install-work out/install-work.test.log out/olpc-boy-install.img out/olpc-boy-restore.img out/qemu-install-* out/nandtest-work out/nandtest-work.test.log out/qemu-nandtest-* out/rootfs.tar out/rootfs-work out/initramfs out/kernel out/.*.stamp out/qemu-* out/test-*'

distclean:
	$(RUN_ROOT) sh -c 'rm -rf out/* out/.*.stamp'

# Logic test of src/olpc-chordd in the builder container (no hardware).
chordd-test:
	$(RUN_USER) sh src/olpc-chordd/test.sh

# Host tests of src/olpc-run (needs out/cores for the core runs).
olpc-run-test:
	$(RUN_USER) sh src/olpc-run/test.sh

# Scaling-kernel self-test of src/olpc-fbbench in the builder container.
fbbench-test:
	$(RUN_USER) sh src/olpc-fbbench/test.sh

# Host test of src/olpc-splash (fake framebuffer, timing).
olpc-splash-test:
	$(RUN_USER) sh src/olpc-splash/test.sh

# Host tests of src/olpc-menu (unit checks, scripted UI against a fake framebuffer, stub launch).
olpc-menu-test:
	$(RUN_USER) sh src/olpc-menu/test.sh

# ---- NAND backup image (separate, read-only; the normal image is unaffected) ----------------
# make backup-image  ->  out/olpc-boy-nandbackup.img  (boot stick; dumps go to a second stick, FAT label OLPCNAND)
BACKUP_SRC   := $(shell find backup -type f 2>/dev/null) scripts/mkbackup.sh rootfs/overlay/usr/libexec/olpcboy/common.sh $(shell find src/olpc-splash src/common -type f 2>/dev/null)

backup-rootfs: out/backup-rootfs.tar
backup-check:  out/.backup-check.stamp
backup-image:  out/olpc-boy-nandbackup.img

out/backup-rootfs.tar: out/kernel/bzImage $(BACKUP_SRC)
	$(RUN_ROOT) bash scripts/mkbackup.sh
	@touch $@

out/.backup-check.stamp: out/backup-rootfs.tar scripts/check-sse.sh
	$(RUN_USER) bash scripts/check-sse.sh out/backup-rootfs.tar
	@touch $@

out/olpc-boy-nandbackup.img: out/kernel/bzImage out/backup-rootfs.tar out/.backup-check.stamp backup/olpc.fth scripts/mkimage.sh
	$(DOCKER) -e HOST_UID=$(UID_) -e HOST_GID=$(GID_) -e IMG_OUT=out/olpc-boy-nandbackup.img \
	  -e ROOTFS_TAR=out/backup-rootfs.tar -e FTH=backup/olpc.fth -e NO_USERDATA=1 -e FREE_MIB=8 \
	  $(BUILDER) bash scripts/mkimage.sh

qemu-backup: out/olpc-boy-nandbackup.img
	bash scripts/qemu-backup-test.sh

# Logic tests of the backup script against a fake MTD (needs zstd in the builder: apk add zstd).
backup-test:
	$(RUN_ROOT) sh -c 'apk add --no-cache zstd >/dev/null 2>&1; sh backup/test.sh'

# ---- NAND write-test image (separate; the normal image is unaffected) ----------------------
# make nandtest-image  ->  out/olpc-boy-nandtest.img: non-destructive write tests of an 8 MiB free
# window of the XO-1 NAND (everything else read-only), then a full compare with the NAND backup.
# The reference hashes are generated at build time from the backup (mounted read-only):
#   NAND_BACKUP_DIR=<dir with nand-data-ecc.bin and nand-raw-oob.bin>   (default below)
NAND_BACKUP_DIR ?= $(HOME)/olpc-nand-backup/run2-nandbackup-001
NANDTEST_SRC := $(shell find nandtest -type f 2>/dev/null) scripts/mknandtest.sh backup/src/nbhash.c backup/src/nbcmp.c rootfs/overlay/usr/libexec/olpcboy/common.sh $(shell find src/olpc-splash src/common -type f 2>/dev/null)

nandtest-rootfs: out/nandtest-rootfs.tar
nandtest-check:  out/.nandtest-check.stamp
nandtest-image:  out/olpc-boy-nandtest.img

out/nandtest-rootfs.tar: out/kernel/bzImage $(NANDTEST_SRC)
	@test -r "$(NAND_BACKUP_DIR)/nand-data-ecc.bin" -a -r "$(NAND_BACKUP_DIR)/nand-raw-oob.bin" || { \
	  echo "ERROR: NAND backup not found in '$(NAND_BACKUP_DIR)' (need nand-data-ecc.bin and nand-raw-oob.bin); set NAND_BACKUP_DIR=..." >&2; exit 1; }
	$(DOCKER) -v "$(NAND_BACKUP_DIR)":/nandbackup:ro -e HOST_UID=$(UID_) -e HOST_GID=$(GID_) $(BUILDER) bash scripts/mknandtest.sh
	@touch $@

out/.nandtest-check.stamp: out/nandtest-rootfs.tar scripts/check-sse.sh
	$(RUN_USER) bash scripts/check-sse.sh out/nandtest-rootfs.tar
	@touch $@

out/olpc-boy-nandtest.img: out/kernel/bzImage out/nandtest-rootfs.tar out/.nandtest-check.stamp nandtest/olpc.fth scripts/mkimage.sh
	$(DOCKER) -e HOST_UID=$(UID_) -e HOST_GID=$(GID_) -e IMG_OUT=out/olpc-boy-nandtest.img \
	  -e ROOTFS_TAR=out/nandtest-rootfs.tar -e FTH=nandtest/olpc.fth -e NO_USERDATA=1 -e FREE_MIB=8 \
	  $(BUILDER) bash scripts/mkimage.sh

# QEMU has no CAFE controller: boots the image once without NAND (must stop safely) and once with
# nandsim (simulated NAND, all six steps end to end).
qemu-nandtest: out/olpc-boy-nandtest.img
	bash scripts/qemu-nandtest-test.sh

# Logic tests of the write-test script against a file-backed fake NAND (builder: gcc, mtd-utils).
nandtest-test:
	$(RUN_ROOT) sh nandtest/test.sh

# ---- install olpc-boy on the internal NAND (separate sticks; the normal image is unaffected) ----
# make nand-image     ->  out/nand-root.jffs2 (+ initrd): the whole NAND as one JFFS2, from out/rootfs.tar
# make install-image  ->  out/olpc-boy-install.img: boot stick that installs it (needs the NAND backup, like nandtest)
# make restore-image  ->  out/olpc-boy-restore.img: boot stick that restores the original OS from the OLPCNAND stick
# make install-test   ->  logic tests of the installer/restore script against a fake NAND
# make qemu-install   ->  QEMU + nandsim: install, boot from the NAND, restore (needs both images)
NAND_SRC    := boot/olpc-nand.fth scripts/mknand.sh install/src/nbjffs.c
INSTALL_SRC := $(shell find install -type f 2>/dev/null) scripts/mkinstall.sh backup/src/nbhash.c backup/src/nbcmp.c nandtest/src/nbtool.c nandtest/test/fakenand.c rootfs/overlay/usr/libexec/olpcboy/common.sh $(shell find src/olpc-splash src/common -type f 2>/dev/null)

nand-image:      out/nand-root.jffs2
install-rootfs:  out/install-rootfs.tar
install-check:   out/.install-check.stamp
install-image:   out/olpc-boy-install.img
restore-image:   out/olpc-boy-restore.img

out/nand-root.jffs2: out/rootfs.tar out/kernel/bzImage $(NAND_SRC)
	$(RUN_ROOT) bash scripts/mknand.sh
	@touch $@

out/install-rootfs.tar: out/nand-root.jffs2 out/kernel/bzImage $(INSTALL_SRC)
	@test -r "$(NAND_BACKUP_DIR)/nand-data-ecc.bin" || { \
	  echo "ERROR: NAND backup not found in '$(NAND_BACKUP_DIR)' (need nand-data-ecc.bin); set NAND_BACKUP_DIR=..." >&2; exit 1; }
	$(DOCKER) -v "$(NAND_BACKUP_DIR)":/nandbackup:ro -e HOST_UID=$(UID_) -e HOST_GID=$(GID_) $(BUILDER) bash scripts/mkinstall.sh
	@touch $@

out/.install-check.stamp: out/install-rootfs.tar scripts/check-sse.sh
	$(RUN_USER) bash scripts/check-sse.sh out/install-rootfs.tar
	@touch $@

out/olpc-boy-install.img: out/kernel/bzImage out/install-rootfs.tar out/.install-check.stamp install/olpc.fth scripts/mkimage.sh
	$(DOCKER) -e HOST_UID=$(UID_) -e HOST_GID=$(GID_) -e IMG_OUT=out/olpc-boy-install.img \
	  -e ROOTFS_TAR=out/install-rootfs.tar -e FTH=install/olpc.fth -e FREE_MIB=16 \
	  $(BUILDER) bash scripts/mkimage.sh

out/olpc-boy-restore.img: out/kernel/bzImage out/install-rootfs.tar out/.install-check.stamp install/olpc-restore.fth scripts/mkimage.sh
	$(DOCKER) -e HOST_UID=$(UID_) -e HOST_GID=$(GID_) -e IMG_OUT=out/olpc-boy-restore.img \
	  -e ROOTFS_TAR=out/install-rootfs.tar -e FTH=install/olpc-restore.fth -e NO_USERDATA=1 -e FREE_MIB=16 \
	  $(BUILDER) bash scripts/mkimage.sh

install-test:
	$(RUN_ROOT) sh install/test.sh

qemu-install: out/olpc-boy-install.img out/olpc-boy-restore.img out/nand-initrd-sim.img
	bash scripts/qemu-install-test.sh
