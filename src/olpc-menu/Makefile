# olpc-menu build.  Inside the builder container:  make -C src/olpc-menu
# Dynamically linked (musl) like olpc-run; it shares the evdev code of olpc-run.
CC      ?= gcc
CFLAGS  ?= -O2 -march=geode -mtune=geode -Wall -Wextra
OUT     ?= .
RUN     := ../olpc-run
SRCS    := olpc-menu.c menulib.c $(RUN)/input.c $(RUN)/diag.c

$(OUT)/olpc-menu: $(SRCS) menulib.h font8x16.h $(RUN)/input.h $(RUN)/diag.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)
	strip $@

# Host tests (any gcc, no hardware): unit checks, rendered frames, stub launch.
test:
	CC="$(CC)" sh test.sh
.PHONY: test
