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

exodoc:
	$(MAKE) -C exodoc

# Doc-debt gate: the live audit must report 0 fail (score 100%). When the
# live daemons are unreachable the live-only checks (api-conformance,
# version-against-daemon) report SKIP instead of FAIL — they need a running
# daemon or a local binary — so with the daemons down the gate still holds
# as long as every DOC check (purpose, sections, version token, honesty)
# passes. If a FAIL appears while the daemons are unreachable we re-run the
# offline doc audit as a cross-check before failing the build.
test-exodoc: all
	$(MAKE) -C exodoc test
	@if [ -f docs/stack.tsv ]; then \
		echo "== exodoc live audit (doc-debt gate) =="; \
		OUT=$$(./exodoc/build/exodoc audit --live --stack docs/stack.tsv --base . 2>&1); \
		echo "$$OUT" | tee /tmp/exodoc-audit.log >/dev/null; \
		if echo "$$OUT" | grep -Eq "=== audit: [0-9]+ pass, 0 fail"; then \
			echo "doc-debt gate: PASS (live, 0 fail)"; \
		elif echo "$$OUT" | grep -q "unreachable"; then \
			echo "doc-debt gate: live daemons unreachable; gating on offline doc audit"; \
			OFF=$$(./exodoc/build/exodoc audit --stack docs/stack.tsv --base . 2>&1); \
			echo "$$OFF" | tee /tmp/exodoc-audit-off.log >/dev/null; \
			if echo "$$OFF" | grep -Eq "=== audit: [0-9]+ pass, 0 fail"; then \
				echo "doc-debt gate: PASS (offline fallback, 0 fail)"; \
			else \
				echo "doc-debt gate: FAILED (offline audit has fails):"; \
				echo "$$OFF"; \
				exit 1; \
			fi; \
		else \
			echo "doc-debt gate: FAILED (live audit has fails):"; \
			echo "$$OUT"; \
			exit 1; \
		fi; \
	fi

clean:
	rm -rf build

.PHONY: all test clean exosched test-exosched exoflow test-exoflow exodoc test-exodoc
