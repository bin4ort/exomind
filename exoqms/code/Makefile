CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L

SRC := src/main.c src/tokenize.c src/analyze.c
OBJ := $(SRC:src/%.c=build/%.o)
BIN := build/exoqms-code

all: exoqms-code

exoqms-code: $(BIN)

build:
	mkdir -p build

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

build/%.o: src/%.c src/code.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

test: exoqms-code
	bash test/test.sh

clean:
	rm -rf build

.PHONY: all exoqms-code test clean
