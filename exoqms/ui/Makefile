CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L

SRC := src/main.c src/util.c src/html.c src/css.c src/color.c src/layout.c \
       src/checks.c
OBJ := $(SRC:src/%.c=build/%.o)
BIN := build/exoqms-ui

all: exoqms-ui

exoqms-ui: $(BIN)

build:
	mkdir -p build

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

build/%.o: src/%.c src/exoqms.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

test: exoqms-ui
	bash test/test.sh

clean:
	rm -rf build

.PHONY: all exoqms-ui test clean
