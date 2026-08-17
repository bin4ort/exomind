# exoqms-code

**Multi-language QMS code-safety module** (v0.2.0): C/C++ analysis,
shell and python line-based adapters, and a generic text-rule engine —
audits any project in any language.

**Code-safety QMS module: static C analyzer for error-handling quality.**

The premise (production experience): when error paths are missing, a simple
var-rename or a dropped `fopen` result silently corrupts output and costs
hours to find. With proper "if not / else" error branches, the error points
at the exact function, and debugging covers only what leads up to it. This
analyzer finds the places where that structure is missing.

Part of the [exomind stack](../../README.md) — the main README is the
full stack reference.

## Usage

```
exoqms-code <file-or-dir>... [--json] [--ignore <glob>] [--version]
```

Findings, one per line:

```
<severity> <check-id> <file:line:col> <reason>
=== findings: N (M major) ===
```

Exit 0 = clean, 1 = findings, 2 = usage error. `--json` emits a JSON array
`{check,severity,file,line,col,reason}` for the QMS daemon.

## Checks

| id | severity | what it flags |
|----|----------|---------------|
| `missing-error-path` | major | value of an error-returning call (fopen, read, strdup, in-file pointer-returning fn) used later without an intervening if-not/else branch — the var-rename-class bug |
| `unchecked-deref-alloc` | major | malloc/calloc/realloc result dereferenced before any NULL check |
| `unchecked-return` | minor | statement `errfn(...);` with the result dropped (`(void)` disclaims) |
| `uninitialized-use` | minor | local read before assignment (in an expression or condition) |
| `swallowed-error` | minor | `if (errfn() != 0) { }` — failure checked with an empty branch |
| `empty-error-branch` | minor | `if (x != 0) { }` — error-style condition with an empty branch |

## Error-function model

- Known libc/POSIX list (fopen, read, write, malloc family, pread, socket
  family, ...). `snprintf`/`pthread_mutex_*` are deliberately excluded: their
  failure is a programming error, and checking them is not the norm; including
  them would drown real findings.
- In-file functions count as error sources only when they return a pointer
  (can be NULL); int/status-returning helpers are chaining patterns.
- Abort-on-fail allocators (`xmalloc`, `xrealloc`, `xcalloc`, `xstrdup`,
  `xstrndup`) are exempt: they never return NULL.
- A function whose contract guarantees non-NULL can be documented with a
  `/* @nonnull */` comment above its definition to clear it.

## Parser scope and honest limitations

Lexical analysis, no full parse. The checker stays silent when a pattern is
undeterminable (false positives are the enemy):

- No control-flow analysis: `p = f(); if (!p) ...; use(p)` is recognized, but
  a use in a sibling branch of an `if/else` is reported conservatively (the
  `util.c:363` style finding is a known single false positive).
- No cross-file analysis: typedefs from included headers are not collected
  (header typedefs in pointer contexts are treated as types by heuristic).
- No macro expansion; preprocessor lines are skipped.
- `(void)` casts, `free()`, `sizeof x`, `&x` output params, and
  `x && use(x)` / `x ? ... : ...` truthiness guards are all respected.

## Test

`make test` runs the fixture suite (24 checks): `fixtures/bad.c` fires every
check id with pinned counts, `fixtures/good.c` is clean, JSON validity,
`--ignore`, directory mode, NUL-byte / 10 MB inputs never crash.

## Real-run results (iteration 6, the stack's own C code)

After calibration, `exoqms-code` over all 7 components (~20k lines C):

- **majors: 99 -> 1** (the documented branch-blindness false positive)
- minors: 312 -> 101, all `unchecked-return` on `close()`/`fclose()`/
  `setsockopt()`/`clock_gettime()` in error paths and cleanup (deliberate
  drops; the error is already in flight — triage items, not defects)
- one real defect found and fixed in the module's own tokenizer (fread
  short-read silently truncated; now returns an error)
- the module's own source is self-audit clean

## Notes

- `@nonnull` annotations are also a documentation contract: `exodoc`-style
  readers see at a glance which functions cannot return NULL.
- Severity rule for the QMS daemon's `code-safety` check: **pass iff 0
  major**; minor findings are reported but non-fatal (see exoqms/standard.md
  §5.3 c6).
