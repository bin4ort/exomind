CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -pthread -D_POSIX_C_SOURCE=200809L
LDFLAGS ?= -pthread

SRC := src/main.c src/http.c src/store.c src/util.c
OBJ := $(SRC:src/%.c=build/%.o)
BIN := build/exomind

all: $(BIN)

build:
	mkdir -p build

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

build/%.o: src/%.c src/util.h src/store.h src/http.h src/version.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(BIN)
	bash test/test.sh

exosched:
	$(MAKE) -C exosched

test-exosched: all
	$(MAKE) -C exosched test

exoflow:
	$(MAKE) -C exoflow

test-exoflow: all
	$(MAKE) -C exoflow test
	@if [ -f exoflow/test/test-integration.sh ]; then \
		bash exoflow/test/test-integration.sh; \
	fi

clean:
	rm -rf build

.PHONY: all test clean exosched test-exosched exoflow test-exoflow
