# olpc-menu build.  Inside the builder container:  make -C src/olpc-menu
# Dynamically linked (musl) like olpc-run; it shares the evdev code of olpc-run and the status parsers,
# icon drawing and font in ../common with olpc-run's in-game strip.
CC      ?= gcc
CFLAGS  ?= -O2 -march=geode -mtune=geode -Wall -Wextra
OUT     ?= .
RUN     := ../olpc-run
COM     := ../common
SRCS    := olpc-menu.c menulib.c netui.c osk.c wallpaper.c $(COM)/status.c $(COM)/gfx.c $(RUN)/input.c $(RUN)/diag.c

$(OUT)/olpc-menu: $(SRCS) menulib.h netui.h osk.h wallpaper.h $(COM)/status.h $(COM)/gfx.h $(COM)/font8x16.h $(RUN)/input.h $(RUN)/diag.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)
	strip $@

# Host tests (any gcc, no hardware): unit checks, rendered frames, stub launch.
test:
	CC="$(CC)" sh test.sh
.PHONY: test
