# exosched kit: build the C runner then verify/audit against the REAL
# exosched sources (schedule.c/util.c). ROOT points at the exomind tree so
# the runner can #include exosched/src/*.c.
ROOT := $(abspath ../..)
EXOKIT := $(ROOT)/exokit/build/exokit

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
