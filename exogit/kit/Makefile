# exogit kit: build the C runner shim, then run exokit verification.
EXOKIT ?= $(ROOT)/exokit/build/exokit
ROOT   := $(CURDIR)/../..

runners/c/run: runners/c/run.c
	$(CC) -O2 -g -std=c11 -Wall -Wextra -pthread \
		-D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_GNU_SOURCE \
		-I$(ROOT) runners/c/run.c -o runners/c/run

all: runners/c/run

verify: runners/c/run
	cd $(ROOT) && $(EXOKIT) verify --kit exogit/kit

audit: runners/c/run
	cd $(ROOT) && $(EXOKIT) audit --kit exogit/kit

.PHONY: all verify audit
