# libsync — Lab 4. A mutex, two semaphores and a lock order.
#
# GIVEN. You do not change this file. It builds:
#
#   libsync.a    your files in src/, and the given files in given/
#   each test    from tests/test_*.c (your test)
#
# The final program, build/sentinel, is built by src/sentinel/Makefile, with
# this library and the three libraries below it.
#
# Your three files come as stubs, so they exist from the start, and the
# library builds at once. If one of them is missing, this Makefile compiles
# each file that exists, names the missing file, and builds no library or
# test: the given code calls each of your functions, so nothing could link.
#
# Lab 4 uses your libraries of Labs 1, 2 and 3. The top-level Makefile builds
# them first.
#
# Threads need -pthread when you compile AND when you link.
#
# Each list of files below is a wildcard, so a new test in tests/ needs no
# change here.

CC     ?= gcc
CFLAGS ?= -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -g
BUILD  ?= $(abspath ../../build)

INC  := -Iinclude -I../../common -I../libsyscall/include \
        -I../libproc/include -I../libipc/include

TEST_DEFS := -DSENTINEL_BUILD_DIR=\"$(BUILD)\"
LIB  := $(BUILD)/libsync.a
ODIR := $(BUILD)/libsync

YOURS    := src/safetrack.c src/sem_queue.c src/resources.c
MISSING  := $(filter-out $(wildcard $(YOURS)),$(YOURS))

SRCS     := $(wildcard src/*.c) $(wildcard given/*.c)
OBJS     := $(patsubst %.c,$(ODIR)/%.o,$(SRCS))

TEST_SRCS  := $(wildcard tests/test_*.c)
TESTS      := $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRCS))
BENCH_SRCS := $(wildcard tests/bench_*.c)
BENCHES    := $(patsubst tests/%.c,$(BUILD)/%,$(BENCH_SRCS))

# The order of the archives matters: each one comes before the ones that it
# uses. libsync uses libipc, which uses libproc, libsyscall and libcommon.
LIBS := $(LIB) $(BUILD)/libipc.a $(BUILD)/libproc.a \
        $(BUILD)/libsyscall.a $(BUILD)/libcommon.a -lm

.PHONY: all test clean

ifeq ($(MISSING),)

all: $(LIB) $(BENCHES)

test: $(TESTS)
	@for t in $(TESTS); do echo "--- $$t"; $$t || exit 1; done

else

all: $(OBJS)
	@echo "  src/libsync: Lab 4 is not complete. Write $(MISSING)."
	@echo "  No library or test is built until each of your three files exists."

test: all

endif

$(LIB): $(OBJS)
	ar rcs $@ $^

$(ODIR)/%.o: %.c include/sentinel/sync.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -pthread $(INC) -c -o $@ $<

$(BUILD)/%: tests/%.c $(LIB)
	$(CC) $(CFLAGS) -pthread $(INC) $(TEST_DEFS) -o $@ $< $(LIBS)

clean:
	rm -rf $(ODIR) $(LIB) $(TESTS) $(BENCHES)
