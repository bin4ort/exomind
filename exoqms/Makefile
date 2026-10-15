CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -pthread -D_POSIX_C_SOURCE=200809L
CFLAGS  += -MMD -MP
CPI_CFLAGS := $(CFLAGS) -I../src -I./src -I../common
LDFLAGS ?= -pthread

SRC := src/main.c src/http.c src/util.c src/exomind.c src/model.c src/checks.c src/project.c
CPI_SRC := cpi/src/cpi_pdca.c cpi/src/cpi_qc.c cpi/src/cpi_tools.c cpi/src/cpi_main.c \
           cpi/src/exo_agent.c cpi/src/exo_session.c cpi/src/cpi_flow_enforce.c cpi/src/cpi_simple.c
OBJ := $(SRC:src/%.c=build/%.o) $(CPI_SRC:cpi/src/%.c=build/%.o) build/exo_common.o build/exo_cli.o
DEP := $(OBJ:.o=.d)
BIN := build/exoqms
CPI_BIN := build/exocpi

all: $(BIN) $(CPI_BIN)

build:
	mkdir -p build

$(BIN): $(filter-out build/cpi_main.o,$(OBJ))
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(CPI_BIN): $(filter-out build/main.o,$(OBJ))
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: cpi/src/%.c | build
	$(CC) $(CPI_CFLAGS) -c -o $@ $<

build/exo_common.o: ../common/exo.c ../common/exo.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/exo_cli.o: ../common/exo_cli.c ../common/exo_cli.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(DEP)

test: $(BIN) $(CPI_BIN)
	bash test/test.sh
	bash cpi/test/test.sh

clean:
	rm -rf build

.PHONY: all test clean
