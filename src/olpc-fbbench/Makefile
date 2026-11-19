# olpc-fbbench build.  Inside the builder container:  make -C src/olpc-fbbench
CC      ?= gcc
CFLAGS  ?= -O2 -march=geode -mtune=geode -Wall -Wextra
OUT     ?= .

$(OUT)/olpc-fbbench: olpc-fbbench.c
	$(CC) $(CFLAGS) -static -o $@ $<
	strip $@

# Host-side self-test of the scaling code (any gcc; no hardware, no -march=geode:
# the test machine is not a Geode, and the MMX path is exercised via CPUID).
test: olpc-fbbench.c test.sh
	CC="$(CC)" sh test.sh
.PHONY: test
