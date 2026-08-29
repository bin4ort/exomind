# kit build: compile the C runner shim, then run exokit verification.
# `make` builds the runner; `make verify` / `make audit` run exokit.

EXOKIT ?= $(ROOT)/exokit/build/exokit
ROOT   := $(CURDIR)/..

runners/c/run: runners/c/run.c
	$(CC) -O2 -g -std=c11 -Wall -Wextra -pthread \
		-D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 \
		-DEXO_REPO_DIR_DEFAULT='"$(ROOT)"' \
		-I$(ROOT) runners/c/run.c -o runners/c/run

all: runners/c/run

verify: runners/c/run
	cd $(ROOT) && $(EXOKIT) verify

audit: runners/c/run
	cd $(ROOT) && $(EXOKIT) audit

.PHONY: all verify audit
