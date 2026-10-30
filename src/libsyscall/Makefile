
CC ?= gcc
CFLAGS ?= -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -g -O1
BUILD ?= $(abspath ../../build)

TESTS := $(patsubst tests/%.c,$(BUILD)/%,$(wildcard tests/test_*.c))

SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:src/%.c=$(BUILD)/%.o)

.PHONY: all test clean

all: $(BUILD)/libsyscall.a $(BUILD)/demo_lab1

# This generates the .o files from the .c files
$(BUILD)/%.o: src/%.c
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -I../../common -c $< -o $@

# asserts the .o files for the .a lib
$(BUILD)/libsyscall.a: $(OBJS)
	ar rcs $@ $^

$(BUILD)/demo_lab1: tests/demo_lab1.c $(BUILD)/libsyscall.a $(BUILD)/libcommon.a
	$(CC) $(CFLAGS) -Iinclude -I../../common $< $(BUILD)/libsyscall.a $(BUILD)/libcommon.a -lm -o $@

# build one test program from one tests/test_*.c
$(BUILD)/test_%: tests/test_%.c $(BUILD)/libsyscall.a
	$(CC) $(CFLAGS) -Iinclude -I../../common $< $(BUILD)/libsyscall.a -o $@

# build every test, then run them, stopping at the first failure
test: $(TESTS)
	for t in $(TESTS); do $$t || exit 1; done

clean:
	rm -f $(OBJS) $(BUILD)/libsyscall.a $(BUILD)/demo_lab1 $(TESTS)
