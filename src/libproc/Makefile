# libproc — Lab 2. Sensors as processes.
#
# GIVEN. You do not change this file. It builds:
#
#   libproc.a    your files in src/, and the given files in given/
#   demo_lab2    from given/demo_lab2.c
#   each test    from tests/test_*.c (your tests)
#
# Until your three files exist (src/sensor.c, src/prio_channel.c and
# src/sched.c), it compiles each file that exists, so that you see the
# errors of the compiler, and it names the files that are missing. It
# builds no library, demo or test until all three exist: the given code
# calls each of your functions, so nothing could link.
#
# The same file builds the reference, which has more files in src/ and
# tests/. Each list below is a wildcard for that reason.

CC     ?= gcc
CFLAGS ?= -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -g
BUILD  ?= $(abspath ../../build)

INC  := -Iinclude -I../../common -I../libsyscall/include

# The tests and the demo start contactfeed. make runs them with this
# directory as the working directory, so a relative path does not work. A
# failed execvp is silent in the parent: the child exits 127 and the pipe
# closes. So a wrong path looks like "no reports", not like an error. Give
# the absolute path.
TEST_DEFS := -DSENTINEL_BUILD_DIR=\"$(BUILD)\"
LIB  := $(BUILD)/libproc.a
ODIR := $(BUILD)/libproc

YOURS    := src/sensor.c src/prio_channel.c src/sched.c
MISSING  := $(filter-out $(wildcard $(YOURS)),$(YOURS))

DEMO_SRC := given/demo_lab2.c
SRCS     := $(wildcard src/*.c) $(filter-out $(DEMO_SRC),$(wildcard given/*.c))
OBJS     := $(patsubst %.c,$(ODIR)/%.o,$(SRCS))

TEST_SRCS  := $(wildcard tests/test_*.c)
TESTS      := $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRCS))
BENCH_SRCS := $(wildcard tests/bench_*.c)
BENCHES    := $(patsubst tests/%.c,$(BUILD)/%,$(BENCH_SRCS))
DEMO       := $(BUILD)/demo_lab2

# The order of the archives matters: libproc uses libsyscall, which uses
# libcommon, so each one comes after the one that needs it.
LIBS := $(LIB) $(BUILD)/libsyscall.a $(BUILD)/libcommon.a -lm

.PHONY: all test clean

ifeq ($(MISSING),)

all: $(LIB) $(BENCHES) $(DEMO)

test: $(TESTS)
	@for t in $(TESTS); do echo "--- $$t"; $$t || exit 1; done

else

all: $(OBJS)
	@echo "  src/libproc: Lab 2 is not complete. Write $(MISSING)."
	@echo "  No library, demo or test is built until each of your three files exists."

test: all

endif

$(LIB): $(OBJS)
	ar rcs $@ $^

$(ODIR)/%.o: %.c include/sentinel/proc.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

$(DEMO): $(DEMO_SRC) $(LIB)
	$(CC) $(CFLAGS) $(INC) $(TEST_DEFS) -o $@ $< $(LIBS)

$(BUILD)/%: tests/%.c $(LIB)
	$(CC) $(CFLAGS) $(INC) $(TEST_DEFS) -o $@ $< $(LIBS)

clean:
	rm -rf $(ODIR) $(LIB) $(TESTS) $(BENCHES) $(DEMO)
