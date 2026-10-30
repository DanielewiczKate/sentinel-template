
CC ?= gcc
CFLAGS ?= -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -g -O1 
BUILD  ?= build
TESTS ?= test

TESTS := $(patsubst tests/%.c,$(BUILD)/%,$(wildcard tests/test_*.c))

SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:src/%.c=$(BUILD)/%.o)

# This generates the .o files from the .c files
$(BUILD)/%.o: src/%.c                         
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

# asserts the .o files for the .a lib
$(BUILD)/libsyscall.a: $(OBJS)
	ar rcs $@ $^



# build one test program from one tests/test_*.c
$(BUILD)/test_%: tests/test_%.c $(BUILD)/libsyscall.a
	$(CC) $(CFLAGS) -Iinclude $< $(BUILD)/libsyscall.a -o $@

# build every test, then run them, stopping at the first failure
test: $(TESTS)
	for t in $(TESTS); do $$t || exit 1; done
