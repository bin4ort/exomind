CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -pthread -D_POSIX_C_SOURCE=200809L
CFLAGS  += -D_XOPEN_SOURCE=700
CFLAGS  += -DEXO_REPO_DIR_DEFAULT=\"$(CURDIR)\"
CFLAGS  += -MMD -MP
LDFLAGS ?= -pthread

SRC := src/main.c src/http.c src/store.c src/util.c src/router.c src/update.c
OBJ := $(SRC:src/%.c=build/%.o) build/exo_common.o
DEP := $(OBJ:.o=.d)
BIN := build/exomind

all: $(BIN)

build:
	mkdir -p build

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/exo_common.o: common/exo.c common/exo.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(DEP)

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

exocrawl:
	$(MAKE) -C exocrawl

test-exocrawl: all
	$(MAKE) -C exocrawl test

exocontext:
	$(MAKE) -C exocontext

test-exocontext: all
	$(MAKE) -C exocontext test

exokit:
	$(MAKE) -C exokit

test-exokit: exokit exoflow
	$(MAKE) -C exokit test
	@if [ -f exoflow/test/test-integration.sh ]; then \
		bash exoflow/test/test-integration.sh; \
	fi

exodoc:
	$(MAKE) -C exodoc

exoqms:
	$(MAKE) -C exoqms
	$(MAKE) -C exoqms/ui
	@if [ -d exoqms/code ]; then $(MAKE) -C exoqms/code; else echo "exoqms/code not present (module branch not merged); skipped"; fi
	@if [ -d exoqms/svg ]; then $(MAKE) -C exoqms/svg; else echo "exoqms/svg not present (module branch not merged); skipped"; fi

# QMS field modules only (exoqms-code + exoqms-svg), independent of the
# daemon build. The module dirs land with their own branches
# (feat/qms-code, feat/qms-svg); a missing module is skipped, not fatal.
qms-modules:
	@if [ -d exoqms/code ]; then $(MAKE) -C exoqms/code; else echo "exoqms/code not present (module branch not merged); skipped"; fi
	@if [ -d exoqms/svg ]; then $(MAKE) -C exoqms/svg; else echo "exoqms/svg not present (module branch not merged); skipped"; fi

test-exoqms: exoqms
	$(MAKE) -C exoqms test
	$(MAKE) -C exoqms/ui test
	@if [ -d exoqms/code ]; then $(MAKE) -C exoqms/code test; else echo "exoqms/code suite skipped (module branch not merged)"; fi
	@if [ -d exoqms/svg ]; then $(MAKE) -C exoqms/svg test; else echo "exoqms/svg suite skipped (module branch not merged)"; fi

# QMS audit of the live stack: starts an exoqms daemon (own port 7692,
# shared exomind 7654 backend), runs the full ISO 19011 audit program
# (all seven checks, ui target = the stack's own good.html fixture),
# prints the findings and greps the score line. Kills only the daemon
# it started; never touches the shared daemons (7654/7655/7656).
audit-stack: all exoqms
	@echo "== exoqms audit program against the live stack =="; \
	URL="http://127.0.0.1:7692"; DAEMON_PID=""; \
	CODE_BIN=""; \
	if [ -x "$(CURDIR)/exoqms/code/build/exoqms-code" ]; then \
		CODE_BIN="$(CURDIR)/exoqms/code/build/exoqms-code"; \
	elif [ -n "$$EXOQMS_CODE_BIN" ]; then CODE_BIN="$$EXOQMS_CODE_BIN"; fi; \
	if ! timeout 3 curl -s -m 2 "$$URL/ping" | grep -q pong; then \
		echo "starting exoqms on 7692 (state backend: shared exomind 7654)"; \
		setsid nohup ./exoqms/build/exoqms --port 7692 \
			--exomind http://127.0.0.1:7654 \
			--exosched http://127.0.0.1:7655 \
			--exodoc $(CURDIR)/exodoc/build/exodoc \
			--ui $(CURDIR)/exoqms/ui/build/exoqms-ui \
			$${CODE_BIN:+--code "$$CODE_BIN"} \
			--svg $(CURDIR)/exoqms/svg/build/exoqms-svg \
			--repo $(CURDIR) --agents a,b,b1,b2,b3,e \
			</dev/null >/tmp/exoqms-audit-stack.log 2>&1 & \
		DAEMON_PID=$$!; \
		for i in 1 2 3 4 5; do \
			timeout 3 curl -s -m 2 "$$URL/ping" | grep -q pong && break; \
			sleep 1; \
		done; \
	fi; \
	RESP=$$(timeout 60 curl -s -X POST "$$URL/audit?target=$(CURDIR)/exoqms/ui/fixtures/good.html" \
		--data-binary "$$(printf 'audit-stack\tcomponent-tests,doc-compliance,dogfood,ui-audit,metrics,code-safety,asset-logic\ta,b,b1,b2,b3,e')"); \
	echo "$$RESP"; \
	echo "score line: $$(echo "$$RESP" | grep -oE '[0-9]+%')"; \
	ID=$$(echo "$$RESP" | awk '{print $$2}'); \
	echo "findings:"; \
	timeout 30 curl -s "$$URL/audit?id=$$ID"; \
	if [ -n "$$DAEMON_PID" ]; then kill "$$DAEMON_PID" 2>/dev/null || true; fi

# Universal QMS audit of the stack's own repo (iter7): the .exoqms.json
# config adds the three universal rules checks (debt, hygiene, secrets)
# on top of the classic seven, with the code-safety scan language-adaptive
# (--lang auto). Runs on a private daemon port 7691 against the SHARED
# exomind 7654 so the audit record and any NCs live in the swarm's memory.
# Kills only the daemon it started; never touches 7654/7655/7656/7657.
qms-universal-test: all exoqms
	@echo "== universal QMS audit of the stack repo (.exoqms.json) =="; \
	URL="http://127.0.0.1:7691"; DAEMON_PID=""; \
	if ! timeout 3 curl -s -m 2 "$$URL/ping" | grep -q pong; then \
		echo "starting exoqms on 7691 (state backend: shared exomind 7654)"; \
		setsid nohup ./exoqms/build/exoqms --port 7691 \
			--exomind http://127.0.0.1:7654 \
			--exosched http://127.0.0.1:7655 \
			--exodoc $(CURDIR)/exodoc/build/exodoc \
			--ui $(CURDIR)/exoqms/ui/build/exoqms-ui \
			--code $(CURDIR)/exoqms/code/build/exoqms-code \
			--svg $(CURDIR)/exoqms/svg/build/exoqms-svg \
			--repo $(CURDIR) --agents a,b,b1,b2,b3,e \
			</dev/null >/tmp/exoqms-universal-stack.log 2>&1 & \
		DAEMON_PID=$$!; \
		for i in 1 2 3 4 5; do \
			timeout 3 curl -s -m 2 "$$URL/ping" | grep -q pong && break; \
			sleep 1; \
		done; \
	fi; \
	RESP=$$(timeout 90 curl -s -X POST "$$URL/audit?target=$(CURDIR)" \
		--data-binary "$$(printf 'audit-stack-universal\tcomponent-tests,doc-compliance,dogfood,ui-audit,metrics,code-safety,debt,hygiene,secrets,asset-logic\ta,b,b1,b2,b3,e')"); \
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
test-exodoc: exodoc
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

.PHONY: all all-stack test clean exosched test-exosched exoflow test-exoflow exodoc test-exodoc exoqms qms-modules test-exoqms audit-stack qms-universal-test exocrawl test-exocrawl exocontext test-exocontext exokit test-exokit install

# build every module of the stack (parallel-safe: make -j all-stack)
all-stack: all exosched exoflow exodoc exoqms exocrawl exocontext exokit qms-modules

# install all modules as `exo<name>` console binaries + `<name>-server`
# symlinks (MCP mode) + batch tools. Usage:
#   make install PREFIX=~/.local     (default /usr/local)
install: all exosched exoflow exodoc exoqms exocrawl exocontext exokit
	install -d $(DESTDIR)$(PREFIX)/bin
	for b in exomind exosched exoflow exodoc exoqms exocrawl exocontext exokit; do \
		case "$$b" in \
			exomind) src=build/exomind ;; \
			exosched) src=exosched/build/exosched ;; \
			exoflow) src=exoflow/build/exoflow ;; \
			exodoc) src=exodoc/build/exodoc ;; \
			exoqms) src=exoqms/build/exoqms ;; \
			exocrawl) src=exocrawl/build/exocrawl ;; \
			exocontext) src=exocontext/build/exocontext ;; \
			exokit) src=exokit/build/exokit ;; \
		esac; \
		install -m 0755 "$$src" "$(DESTDIR)$(PREFIX)/bin/$$b"; \
		ln -sf "$$b" "$(DESTDIR)$(PREFIX)/bin/$$b-server"; \
	done
	@echo "installed to $(DESTDIR)$(PREFIX)/bin (console: exo<name>, MCP: <name>-server)"

