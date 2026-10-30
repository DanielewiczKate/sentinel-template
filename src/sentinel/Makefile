# sentinel — the complete system: all four layers, one command.

CC     ?= gcc
CFLAGS ?= -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -g
BUILD  ?= $(abspath ../../build)

INC  := -Iinclude -I../../common -I../libsyscall/include -I../libproc/include \
        -I../libipc/include -I../libsync/include

# The supervisor of the sensors (SIGCHLD, restart) is not a part of a lab.
# The student repository gives it here, as supervise.c and
# include/sentinel/supervise.h. The reference builds it into libproc.a, and
# has no copy here, so the wildcard is empty there.
SUPERVISE := $(wildcard supervise.c)
BIN  := $(BUILD)/sentinel

# Link order matters for static archives: each library must come before the
# ones it depends on. sync -> ipc -> proc -> syscall -> common.
# The archives alone, and then the full link line. They are separate because
# a pattern rule whose prerequisites include "-lm" is silently discarded by
# make: the target then reports "no rule to make target", which is a puzzling
# way to be told that a prerequisite could not be resolved.
LIBA := $(BUILD)/libsync.a $(BUILD)/libipc.a $(BUILD)/libproc.a \
        $(BUILD)/libsyscall.a $(BUILD)/libcommon.a
LIBS := $(LIBA) -lm

TEST_SRCS := $(wildcard tests/test_*.c)
TESTS     := $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRCS))

.PHONY: all test clean

# The final program needs each of the four libraries. Until each one exists
# (a lab that is not complete builds no library), name the missing ones and
# build nothing, so that make does not stop with "No rule to make target".
MISSING_LIBS := $(filter-out $(wildcard $(LIBA)),$(LIBA))

ifeq ($(MISSING_LIBS),)
all: $(BIN)
else
all:
	@echo "  src/sentinel: no final program yet. Missing: $(notdir $(MISSING_LIBS))."
endif

$(BIN): main.c $(SUPERVISE) $(LIBA) | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(INC) -DSENTINEL_BUILD_DIR=\"$(BUILD)\" \
	      -o $@ main.c $(SUPERVISE) $(LIBS)

$(BUILD)/%: tests/%.c $(LIBA) | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(INC) -DSENTINEL_BUILD_DIR=\"$(BUILD)\" \
	      -o $@ $< $(LIBS)

# The acceptance test runs the assembled binary and attacks it, so it needs
# both tools built first. Nothing below libsync is enough on its own.
test: $(BIN) $(TESTS)
	@for t in $(TESTS); do echo "--- $$t"; $$t || exit 1; done

$(BUILD):
	mkdir -p $@

clean:
	rm -f $(BIN) $(TESTS)
