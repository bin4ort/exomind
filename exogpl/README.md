# exogpl v0.1.0 — AI-native GPL v3.0 compliance checker

A compliance checker for GPL v3.0 licensed projects: scans source trees for
GPL v3.0 license headers, checks dependency license compatibility, tracks
copyright attributions, and generates audit-ready compliance reports. It runs
with zero external dependencies (C11 + libc only) and stores no state — it is
a stateless scanner that can serve as a console operation or an HTTP daemon.

Part of the [exomind stack](../README.md) — the main README is the full stack
reference (this file has the complete exogpl documentation).

```
make exogpl         # builds exogpl/exogpl (from the repo root)
make test-exogpl    # runs exogpl/test/test.sh (8 checks, <1s)
```

## Build

`make exogpl` from the repo root (or `make -C exogpl` directly) produces
`exogpl/exogpl` — a single C11 binary, zero dependencies beyond libc +
pthread. No JS, no cookies, no accounts required.

## Usage

The same binary has two lives: one-shot **console operations** and the
**HTTP daemon**.

```
./exogpl/exogpl                     # prints the API guide and exits
./exogpl/exogpl /audit?path=src     # one-shot directory audit
./exogpl/exogpl /file?path=src/main.c  # one-shot file audit
./exogpl/exogpl /report             # one-shot compliance report
./exogpl/exogpl /deps?path=.        # one-shot dependency scan
./exogpl/exogpl --serve --port 7661
```

With no arguments the API guide (the same text the daemon's `GET /` serves)
is printed and nothing is bound. A first argument starting with `/` is a
console operation: it runs in-process through the internal dispatcher — no
server and no socket is ever opened — and prints the response body. Exit
status is `0` on success, `1` on an API error, `2` on a usage error.

Console operations default to `GET`; the mutating ones are `POST` (none
currently). A `POST` body comes from `--body <text>` when given, otherwise
from stdin (only when stdin is not a tty).

The daemon binds a port **only** when started with `--serve` (or an explicit
`--port`); with neither, no socket is opened:

```
./exogpl/exogpl --serve --port 7661
./exogpl/exogpl --serve --host 0.0.0.0 --port 7661
```

`--port` defaults to 7661, `--host` to 127.0.0.1. `--help`, `--version` and
the `--help <name>` stack help work as before.

## API

| method | path                                    | purpose                              |
|--------|-----------------------------------------|--------------------------------------|
| GET    | /                                       | full spec (self-describing)          |
| GET    | /ping                                   | liveness: `pong`                     |
| GET    | /audit?path=<dir>                       | audit a directory for GPL compliance |
| GET    | /file?path=<file>                       | audit a single source file           |
| GET    | /report                                 | generate compliance report           |
| GET    | /deps?path=<dir>                        | list dependencies and their licenses |
| GET    | /contributors?path=<dir>                | list contributors from git history   |

Errors are `error: <reason>` with HTTP 4xx/5xx.

## Endpoints

### /audit — Directory Scanner

Scans a directory recursively for source files (`.c`, `.h`, `.cpp`, `.hpp`,
`.cc`, `.cxx`, `.java`, `.go`, `.rs`, `.py`, `.sh`) and checks each for
GPL v3.0 license headers. Reports compliance status for every file scanned.

```
GET /audit?path=src
GET /audit?path=/path/to/project
```

The scan checks the first 100 lines of each file for `GNU GENERAL PUBLIC
LICENSE` with `Version 3` or `Version 3.`. It also extracts the first
`Copyright` or `COPYRIGHT` notice found. Files are capped at 10 000 entries
(`MAX_FILES`).

### /file — Source File Auditor

Audits a single source file for GPL v3.0 compliance. Returns the file path,
whether a GPL header was found, compliance status, and any copyright notice.

```
GET /file?path=src/main.c
```

### /report — Compliance Report

Generates a formatted GPL v3.0 compliance report using the result of the
most recent `/audit` run. The report includes a summary (total files,
compliant, missing, incompatible deps, errors) followed by per-file details.

```
GET /report
```

Requires a prior `/audit` call; otherwise returns `error: no audit run yet;
use /audit first`.

### /deps — Dependency License Scanner

Scans a directory for `LICENSE` and `COPYING` files, reads their first line
as a license identifier, and checks compatibility against a hardcoded list
of GPL v3.0-compatible and incompatible license identifiers.

Compatible: GPL, GPL-2.0, GPL-3.0, LGPL, LGPL-2.1, LGPL-3.0, AGPL,
AGPL-3.0, MPL, MPL-2.0, MIT, BSD, BSD-2, BSD-3, ISC, Apache, Apache-2.0,
Zlib, Unlicense, CC0, Public Domain, WTFPL, 0BSD.

Incompatible: Proprietary, Commercial, All Rights Reserved, EULA, SSPL,
BSL, Commons Clause.

```
GET /deps?path=.
```

## Console Operations

The binary can run a single API operation and exit — no daemon needed:

```sh
# Audit the src directory
./exogpl/exogpl /audit?path=src

# Audit a single file
./exogpl/exogpl /file?path=src/main.c

# Generate a compliance report (requires prior audit)
./exogpl/exogpl /report

# Scan dependencies
./exogpl/exogpl /deps?path=.
```

Perfect for scripting, CI pipelines, and one-off compliance checks.

## Limitations

- Contributors scan (`/contributors`) is not implemented — returns
  `error: contributors scan not yet implemented (git-based)`.
- Dependency scanning is simple: only `LICENSE` and `COPYING` files in the
  target directory are checked; no recursive or transitive dependency
  resolution.
- No git-based analysis: contributor tracking and commit-level license
  history are not available.
- License detection is limited to the first 100 lines of source files;
  headers placed deeper are not detected.
- Only substring matching is used for license compatibility — false positives
  are possible (e.g., "BSD" matches any license name containing those letters).

## Internals

`exogpl` is a single-file application (`src/main.c`, ~390 lines) with one
header (`src/exogpl.h`). The HTTP server dispatches incoming requests via
`exogpl_dispatch()`, which uses `path_match()` to prefix-match URL paths;
`path_match()` strips query strings by locating `'?'` before comparison.

Audit state is held in two static globals: `g_result` (an `audit_result_t`
struct) and `g_audited` (an `int` flag). The `/report` endpoint reads these
statics without re-running the scanner. Report generation writes into a
static buffer (returning the buffer pointer), making it re-entrant across
consecutive calls but not thread-safe.

Server mode runs a single-threaded accept loop in `main()`: each accepted
connection is served by `handle_client()`, which reads the request, calls
`exogpl_dispatch()`, and writes the response back.

## Tests

`make test-exogpl` runs the full suite; the QMS gate runs
`./exogpl/exogpl --version`.

`./exogpl/test/test.sh` runs the exogpl test suite: version flag, no-arg spec
print, ping, help, directory audit (self-audit of `src/`), report generation,
single-file audit, and missing-parameter error handling. Coverage includes
the console operation surface: no-args prints the guide without binding,
`/audit?path=src` runs in-process without a server, `/file?path=...`
audits a file, `/report` generates a report, and `--version` works.

## License

GPL-3.0-only — see [LICENSE](../LICENSE).
