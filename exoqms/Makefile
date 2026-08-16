CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -pthread -D_POSIX_C_SOURCE=200809L
LDFLAGS ?= -pthread

SRC := src/main.c src/http.c src/util.c src/exomind.c src/model.c src/checks.c src/project.c
OBJ := $(SRC:src/%.c=build/%.o)
BIN := build/exoqms

all: $(BIN)

build:
	mkdir -p build

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

build/%.o: src/%.c src/exoqms.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(BIN)
	bash test/test.sh

clean:
	rm -rf build

.PHONY: all test clean
