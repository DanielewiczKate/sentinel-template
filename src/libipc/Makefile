# libipc — Lab 3. Worker threads, the shared queue, the track picture.
#
# GIVEN. You do not change this file. It builds:
#
#   libipc.a     your files in src/, and the given files in given/
#   demo_lab3    from given/demo_lab3.c
#   each test    from tests/test_*.c (your tests)
#
# Until your two files exist (src/queue.c and src/pool.c), it compiles each
# file that exists, so that you see the errors of the compiler, and it names
# the files that are missing. It builds no library, demo or test until both
# exist: the given code calls each of your functions, so nothing could link.
#
# Lab 3 uses your Lab 2 library, build/libproc.a, and your Lab 1 library,
# build/libsyscall.a. The top-level Makefile builds them first.
#
# Threads need -pthread when you compile AND when you link.
#
# The same file builds the reference, which has more files in tests/. Each
# list below is a wildcard for that reason.

CC     ?= gcc
CFLAGS ?= -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -g
BUILD  ?= $(abspath ../../build)

INC  := -Iinclude -I../../common -I../libsyscall/include -I../libproc/include

# The demo starts contactfeed, and make runs it from this directory, so the
# path must be absolute. See src/libproc/Makefile for the reason.
TEST_DEFS := -DSENTINEL_BUILD_DIR=\"$(BUILD)\"
LIB  := $(BUILD)/libipc.a
ODIR := $(BUILD)/libipc

YOURS    := src/queue.c src/pool.c
MISSING  := $(filter-out $(wildcard $(YOURS)),$(YOURS))

DEMO_SRC := given/demo_lab3.c
SRCS     := $(wildcard src/*.c) $(filter-out $(DEMO_SRC),$(wildcard given/*.c))
OBJS     := $(patsubst %.c,$(ODIR)/%.o,$(SRCS))

TEST_SRCS  := $(wildcard tests/test_*.c)
TESTS      := $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRCS))
BENCH_SRCS := $(wildcard tests/bench_*.c)
BENCHES    := $(patsubst tests/%.c,$(BUILD)/%,$(BENCH_SRCS))
DEMO       := $(BUILD)/demo_lab3

# The order of the archives matters: each one comes before the ones that it
# uses. libipc uses libproc, which uses libsyscall, which uses libcommon.
LIBS := $(LIB) $(BUILD)/libproc.a $(BUILD)/libsyscall.a $(BUILD)/libcommon.a -lm

.PHONY: all test clean

ifeq ($(MISSING),)

all: $(LIB) $(BENCHES) $(DEMO)

test: $(TESTS)
	@for t in $(TESTS); do echo "--- $$t"; $$t || exit 1; done

else

all: $(OBJS)
	@echo "  src/libipc: Lab 3 is not complete. Write $(MISSING)."
	@echo "  No library, demo or test is built until each of your two files exists."

test: all

endif

$(LIB): $(OBJS)
	ar rcs $@ $^

$(ODIR)/%.o: %.c include/sentinel/ipc.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -pthread $(INC) -c -o $@ $<

$(DEMO): $(DEMO_SRC) $(LIB)
	$(CC) $(CFLAGS) -pthread $(INC) $(TEST_DEFS) -o $@ $< $(LIBS)

$(BUILD)/%: tests/%.c $(LIB)
	$(CC) $(CFLAGS) -pthread $(INC) $(TEST_DEFS) -o $@ $< $(LIBS)

clean:
	rm -rf $(ODIR) $(LIB) $(TESTS) $(BENCHES) $(DEMO)
