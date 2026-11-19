# olpc-chordd build.  Inside the builder container:  make -C src/olpc-chordd
CC      ?= gcc
CFLAGS  ?= -Os -march=geode -mtune=geode -Wall -Wextra
OUT     ?= .

$(OUT)/olpc-chordd: olpc-chordd.c
	$(CC) $(CFLAGS) -static -o $@ $<
	strip $@

# Host-side logic test (any gcc, no hardware): make -C src/olpc-chordd test
test: olpc-chordd.c test.sh
	CC="$(CC)" sh test.sh
.PHONY: test
