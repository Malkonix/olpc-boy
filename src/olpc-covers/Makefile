# olpc-covers build.  Inside the builder container:  make -C src/olpc-covers OUT=dir
CC     ?= gcc
CFLAGS ?= -O2 -march=geode -mtune=geode -Wall -Wextra
OUT    ?= .

all: $(OUT)/png2obc $(OUT)/olpc-covers-match

$(OUT)/png2obc: png2obc.c stb_image.h
	$(CC) $(CFLAGS) -static -o $@ png2obc.c
	strip $@

$(OUT)/olpc-covers-match: olpc-covers-match.c
	$(CC) $(CFLAGS) -static -o $@ olpc-covers-match.c
	strip $@

# Host tests (no network): make -C src/olpc-covers test
test:
	CC="$(CC)" sh test.sh
.PHONY: all test
