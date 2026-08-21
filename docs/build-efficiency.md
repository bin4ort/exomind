# Build efficiency report

Date: 2026-08-21 · Repo: `deb639a` + this pass · Scope: the whole stack
(11 C11 modules: exomind + exosched + exoflow + exodoc + exoqms +
exocrawl + exocontext + exokit + exoqms-ui + exoqms-code + exoqms-svg)

## Verdict

The build was **not efficient**: a full clean build took ~28 s serially,
every incremental relink wasted a full extra translation-unit compile of
`common/exo.c`, and — the worst part — **shared-header changes never
triggered rebuilds at all**, so `make` could report success while the
`build/` binaries silently diverged from the source (the exact class of
"unexpected bug from mid-development changes" the process rules exist
to prevent). All of it is fixed in this pass; the numbers below are the
before/after evidence.

## What was measured (before)

| measurement | before | after | notes |
|-------------|--------|-------|-------|
| clean full-stack build (serial) | 28.4 s | 28.2 s | build-time dominated by 11×~6 compiles |
| clean full-stack build (`-j8`) | 6.9 s (manual 4-cmd dance) | 3.9 s (`make -j8 all-stack`) | one command now builds everything |
| single-edit module rebuild | 0.81 s | 0.18 s | relink no longer recompiles `exo.c` |
| edit `common/exo.c` (shared) | **nothing rebuilt (silent stale)** | 8.9 s (all 6 dependents rebuild) | correctness fix, not speed |
| edit `common/exo.h` (shared) | **nothing rebuilt (silent stale)** | exact dependents rebuild | was completely invisible to make |

## Findings (before)

1. **`common/exo.c` compiled on every relink.** Every module Makefile
   used `OBJ := $(SRC:src/%.c=build/%.o)`; the `src/%` pattern does not
   match `../common/exo.c`, so the literal source path stayed in `OBJ`
   and landed on the link command line. `cc` then compiled it to a temp
   object **on every relink** — six binaries (exomind, exosched,
   exoflow, exoqms, exocrawl, exocontext) each paid a ~0.5 s compile per
   source edit. Verified: `cc ... -o build/exosched build/*.o ../common/exo.c -pthread`.

2. **No dependency tracking (`-MMD`/`.d`).** Each object rule listed only
   its own module header manually (`build/%.o: src/%.c src/exosched.h`).
   `common/exo.h` is included by 12 translation units across the tree
   (via `#include "../../common/exo.h"`), but no rule listed it —
   `touch common/exo.h; make -C exosched` → *"Nothing to be done"*.
   A shared-API change could go entirely unnoticed by the build; the
   repo's `build/` binaries and any `make install` would ship the OLD
   contract while the source says otherwise. This is the highest-impact
   finding: it makes incremental builds *wrong*, not just slow.

3. **Unnecessary exomind builds in test targets.** `test-exodoc`,
   `test-exokit` and `test-exoqms` declared `all` as a prerequisite
   (building `build/exomind`) even though their suites never use it —
   only four suites (exosched, exoflow, exocrawl, exocontext) actually
   invoke the repo's `build/exomind`. Running the full gate line built
   exomind needlessly.

4. **No whole-stack build target.** `make` builds only exomind; the
   field modules (`exoqms-code`/`ui`/`svg`) were reachable only via
   `make -C`, and there was no single parallel-friendly "build
   everything" target. `install` built everything but serially.

5. **`install` missing from `.PHONY`** — a stray `install` file would
   silently defeat the target.

## Fixes applied (all in the repo, none touching the installed stack)

- `common/exo.c` is now compiled once into `build/exo_common.o` (per
  module) with its own rule and header prerequisite, and linked as a
  normal object — relinks no longer recompile it.
- `-MMD -MP` added to every module's `CFLAGS` plus `-include $(OBJ:.o=.d)`
  (11 Makefiles). Header changes now trigger exact rebuilds; the manual
  header lists are gone.
- Root `test-exodoc`/`test-exokit`/`test-exoqms` depend on their own
  module build instead of `all`.
- New top-level `all-stack` target (`make -j all-stack` builds all 11
  modules) and `.PHONY: install`.
- Exhaustive re-verification after the change: clean builds (serial and
  parallel), all 11 suites green, and the 11 binaries `--version`-clean.

## Remaining recommendations (not blocking)

- The suites themselves are the long pole of a full gate (~7 min serial);
  they are hermetic by design and could be parallelized safely (each has
  its own private ports/temp dirs — see ISSUE-004 mitigation) with
  `make -j test test-exosched test-exoflow ...`.
- `docs/stack.tsv` still declares `make test` (exodoc/exokit) and
  `--version` smokes as the QMS gate's in-budget commands; exocrawl's
  full suite (~17 s) stays outside the 5 s audit budget by design
  (ISSUE-023).
- A CI pass could enforce "no stale build" cheaply: `touch common/exo.h
  && make -j all-stack && make -j test-...` — now that header edits
  actually rebuild, this is a meaningful check.

## Verification evidence

- `make -n -C exosched -B` link line before: `... build/delivery.o ../common/exo.c -pthread`
  after: `... build/delivery.o build/exo_common.o -pthread`
- `touch common/exo.h; make -C exosched -n` before: *"Nothing to be
  done"* · after: rebuilds `main.o`, `http.o`, `exo_common.o` + relink.
- Clean parallel: `make -j8 all-stack` → 3.9 s, 11 binaries, all
  `--version` = `v0.4.0-alpha.1`.
- Suites after the change: exomind ALL TESTS PASSED (371), exosched
  82/0, exoflow 139/0, exoqms 160/0, exocrawl 68/0, exocontext 51/0,
  exodoc 36/0, exokit 50/0, exoqms-ui 35/0, exoqms-code 58/0,
  exoqms-svg 61/0.
