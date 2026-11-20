# olpc-splash build.  Inside the builder container:  make -C src/olpc-splash
# Static: it also runs from the initramfs.
CC      ?= gcc
CFLAGS  ?= -Os -march=geode -mtune=geode -Wall -Wextra
OUT     ?= .
COM     := ../common

$(OUT)/olpc-splash: olpc-splash.c $(COM)/gfx.c $(COM)/status.c $(COM)/gfx.h $(COM)/status.h $(COM)/font8x16.h
	$(CC) $(CFLAGS) -ffunction-sections -Wl,--gc-sections -static -o $@ olpc-splash.c $(COM)/gfx.c $(COM)/status.c
	strip $@

test:
	CC="$(CC)" sh test.sh
.PHONY: test
