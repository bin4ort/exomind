CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -pthread -D_POSIX_C_SOURCE=200809L
LDFLAGS ?= -pthread

SRC := src/main.c src/util.c src/http.c src/html.c src/search.c src/pool.c ../common/exo.c
OBJ := $(SRC:src/%.c=build/%.o)
BIN := build/exocrawl

all: exocrawl

exocrawl: $(BIN)

build:
	mkdir -p build

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

build/%.o: src/%.c src/exocrawl.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

test: exocrawl
	bash test/test.sh

clean:
	rm -rf build

.PHONY: all exocrawl test clean
