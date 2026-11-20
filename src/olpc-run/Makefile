# olpc-run build.  Inside the builder container:  make -C src/olpc-run
# Dynamically linked against musl: the cores are dlopen()ed.
CC      ?= gcc
CFLAGS  ?= -O2 -march=geode -mtune=geode -Wall -Wextra
OUT     ?= .
# ../common: status parsers, icon drawing and the font, shared with olpc-menu
COM     := ../common
SRCS    := olpc-run.c scale.c audio.c input.c resample.c diag.c saver.c overlay.c state.c pause.c $(COM)/status.c $(COM)/gfx.c

$(OUT)/olpc-run: $(SRCS) scale.h audio.h input.h resample.h diag.h saver.h overlay.h state.h pause.h libretro.h $(COM)/status.h $(COM)/gfx.h $(COM)/font8x16.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) -ldl -pthread
	strip $@

# Host-side tests (any gcc, no hardware): scaler, resampler, input mapping and,
# when the cores are built, full runs of each core.  Run it in the builder container.
test:
	CC="$(CC)" sh test.sh
.PHONY: test
