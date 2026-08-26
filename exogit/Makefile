CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE
LDFLAGS ?= -pthread
BUILD := build
OBJS := $(BUILD)/main.o $(BUILD)/util.o $(BUILD)/net.o $(BUILD)/json.o $(BUILD)/github.o

.PHONY: all test clean

all: exogit

$(BUILD):
	mkdir -p $(BUILD)

exogit: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/main.o: src/main.c src/exogit.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/util.o: src/util.c src/exogit.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/net.o: src/net.c src/exogit.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/json.o: src/json.c src/exogit.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/github.o: src/github.c src/exogit.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

test: exogit
	./test/test.sh

clean:
	rm -rf $(BUILD) exogit
