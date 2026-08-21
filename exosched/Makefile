CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -pthread -D_POSIX_C_SOURCE=200809L
CFLAGS  += -MMD -MP
LDFLAGS ?= -pthread

SRC := src/main.c src/http.c src/util.c src/timers.c src/exomind.c src/ws.c src/schedule.c src/delivery.c
OBJ := $(SRC:src/%.c=build/%.o) build/exo_common.o
DEP := $(OBJ:.o=.d)
BIN := build/exosched

all: $(BIN)

build:
	mkdir -p build

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/exo_common.o: ../common/exo.c ../common/exo.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(DEP)

test: $(BIN)
	bash test/test.sh

clean:
	rm -rf build

.PHONY: all test clean
