CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE
LDFLAGS ?=
SRCS = $(wildcard src/*.c)
OBJS = $(patsubst src/%.c,build/%.o,$(SRCS))

.PHONY: all test clean

all: exogpl

build:
	mkdir -p build

exogpl: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

test: exogpl
	./test/test.sh

clean:
	rm -f exogpl; rm -rf build
