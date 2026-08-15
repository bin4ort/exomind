CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L

SRC := src/main.c src/util.c src/geom.c src/svgparse.c src/checks.c
OBJ := $(SRC:src/%.c=build/%.o)
BIN := build/exoqms-svg

all: exoqms-svg

exoqms-svg: $(BIN)

build:
	mkdir -p build

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) -lm

build/%.o: src/%.c src/svg.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

test: exoqms-svg
	bash test/test.sh

clean:
	rm -rf build

.PHONY: all exoqms-svg test clean
