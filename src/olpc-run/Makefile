# olpc-run build.  Inside the builder container:  make -C src/olpc-run
# Dynamically linked against musl: the cores are dlopen()ed.
CC      ?= gcc
CFLAGS  ?= -O2 -march=geode -mtune=geode -Wall -Wextra
OUT     ?= .
SRCS    := olpc-run.c scale.c audio.c input.c resample.c diag.c saver.c

$(OUT)/olpc-run: $(SRCS) scale.h audio.h input.h resample.h diag.h saver.h libretro.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) -ldl -pthread
	strip $@

# Host-side tests (any gcc, no hardware): scaler, resampler, input mapping and,
# when the cores are built, full runs of each core.  Run it in the builder container.
test:
	CC="$(CC)" sh test.sh
.PHONY: test
