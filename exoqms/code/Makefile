CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L
CFLAGS  += -MMD -MP

SRC := src/main.c src/tokenize.c src/analyze.c src/linemodes.c
OBJ := $(SRC:src/%.c=build/%.o)
DEP := $(OBJ:.o=.d)
BIN := build/exoqms-code

all: exoqms-code

exoqms-code: $(BIN)

build:
	mkdir -p build

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(DEP)

test: exoqms-code
	bash test/test.sh

clean:
	rm -rf build

.PHONY: all exoqms-code test clean
