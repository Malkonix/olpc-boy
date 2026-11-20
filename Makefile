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
ROOTFS_SRC    := $(shell find rootfs src -type f 2>/dev/null) scripts/mkrootfs.sh

.PHONY: olpc-splash-test chordd-test fbbench-test olpc-run-test olpc-menu-test all builder initramfs kernel rootfs check image qemu flash logs clean distclean
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
	$(RUN_ROOT) sh -c 'rm -rf out/olpc-boy.img out/rootfs.tar out/rootfs-work out/initramfs out/kernel out/.*.stamp out/qemu-* out/test-*'

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
