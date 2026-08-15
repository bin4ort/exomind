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

exoqms:
	$(MAKE) -C exoqms
	$(MAKE) -C exoqms/ui

test-exoqms: all
	$(MAKE) -C exoqms test
	$(MAKE) -C exoqms/ui test

# QMS audit of the live stack: starts an exoqms daemon (own port 7692,
# shared exomind 7654 backend), runs the full ISO 19011 audit program
# (all five checks, ui target = the stack's own good.html fixture),
# prints the findings and greps the score line. Kills only the daemon
# it started; never touches the shared daemons (7654/7655/7656).
audit-stack: all exoqms
	@echo "== exoqms audit program against the live stack =="; \
	URL="http://127.0.0.1:7692"; DAEMON_PID=""; \
	if ! timeout 3 curl -s -m 2 "$$URL/ping" | grep -q pong; then \
		echo "starting exoqms on 7692 (state backend: shared exomind 7654)"; \
		setsid nohup ./exoqms/build/exoqms --port 7692 \
			--exomind http://127.0.0.1:7654 \
			--exosched http://127.0.0.1:7655 \
			--exodoc $(CURDIR)/exodoc/build/exodoc \
			--ui $(CURDIR)/exoqms/ui/build/exoqms-ui \
			--repo $(CURDIR) --agents a,b,b1,b2,b3,e \
			</dev/null >/tmp/exoqms-audit-stack.log 2>&1 & \
		DAEMON_PID=$$!; \
		for i in 1 2 3 4 5; do \
			timeout 3 curl -s -m 2 "$$URL/ping" | grep -q pong && break; \
			sleep 1; \
		done; \
	fi; \
	RESP=$$(timeout 60 curl -s -X POST "$$URL/audit?target=$(CURDIR)/exoqms/ui/fixtures/good.html" \
		--data-binary "$$(printf 'audit-stack\tcomponent-tests,doc-compliance,dogfood,ui-audit,metrics\ta,b,b1,b2,b3,e')"); \
	echo "$$RESP"; \
	echo "score line: $$(echo "$$RESP" | grep -oE '[0-9]+%')"; \
	ID=$$(echo "$$RESP" | awk '{print $$2}'); \
	echo "findings:"; \
	timeout 30 curl -s "$$URL/audit?id=$$ID"; \
	if [ -n "$$DAEMON_PID" ]; then kill "$$DAEMON_PID" 2>/dev/null || true; fi

clean:
	rm -rf build

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

.PHONY: all test clean exosched test-exosched exoflow test-exoflow exodoc test-exodoc exoqms test-exoqms audit-stack
