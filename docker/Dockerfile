# Build environment: Alpine 3.18 x86 (last series whose gcc targets i586, no SSE2).
FROM --platform=linux/386 alpine:3.18
RUN apk add --no-cache \
    build-base bash bc bison flex perl xz tar cpio gzip findutils diffutils coreutils \
    elfutils-dev openssl-dev ncurses-dev linux-headers kmod curl git patch \
    e2fsprogs e2fsprogs-extra dosfstools mtools sfdisk mtd-utils busybox-static file binutils
WORKDIR /work
