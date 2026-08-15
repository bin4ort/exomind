CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L

SRC := src/main.c src/http.c src/util.c src/audit.c
OBJ := $(SRC:src/%.c=build/%.o)
BIN := build/exodoc

all: $(BIN)

build:
	mkdir -p build

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

build/%.o: src/%.c src/exodoc.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(BIN)
	bash test/test.sh

clean:
	rm -rf build

.PHONY: all test clean
