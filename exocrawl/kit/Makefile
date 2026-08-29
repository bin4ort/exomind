# exocrawl kit: build the C runner then verify/audit against the REAL
# exocrawl sources (html.c/util.c/search.c). ROOT points at the exomind tree so
# the runner can #include exocrawl/src/*.c. Verification uses the SYSTEM exokit
# (~/.local/bin/exokit) — the stable toolchain developing the next version.
ROOT := $(abspath ../..)
EXOKIT := $(HOME)/.local/bin/exokit

.PHONY: build verify audit clean
build:
	cc -O2 -g -std=c11 -Wall -Wextra \
	    -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 \
	    -I$(ROOT) runners/c/run.c -o runners/c/run

verify: build
	$(EXOKIT) verify --kit .

audit: build
	$(EXOKIT) audit --kit .

clean:
	rm -f runners/c/run
