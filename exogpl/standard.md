# exogpl standard v0.1.0 — the GPL compliance checker standard for the AI-native stack

Normative document. This standard defines the requirements for the `exogpl`
component — a GPL v3.0 compliance checker covering directory scanning, source
file auditing, dependency license checking, and report generation. Every
requirement below is checkable by `exodoc audit` (doc checks c1–c7, c8) or
by the QMS `component-tests` check.

## 1. Scope

This standard applies to every deployment of the `exogpl` component of the
exomind stack. `exogpl` is a GPL v3.0 compliance checker. It provides
directory scanning, source file auditing, dependency license checking, and
compliance report generation. It has no durable state — it is a stateless
scanner tool.

## 2. Identification

- **c1 purpose** — this document opens with a top-level heading naming
  `exogpl` followed by a non-empty first paragraph describing its purpose
  (GPL v3.0 compliance checker).
- **c8 version** — the document contains a version token matching `v?X.Y.Z`
  (the first such token is the documented version: `v0.1.0`).

## 3. API surface

| method | path                                    | purpose                              |
|--------|-----------------------------------------|--------------------------------------|
| GET    | /                                       | full spec (self-describing)          |
| GET    | /ping                                   | liveness: `pong`                     |
| GET    | /audit?path=<dir>                       | audit a directory for GPL compliance |
| GET    | /file?path=<file>                       | audit a single source file           |
| GET    | /report                                 | generate compliance report           |
| GET    | /deps?path=<dir>                        | list dependencies and their licenses |
| GET    | /contributors?path=<dir>                | list contributors from git history   |

Every endpoint is self-describing at `GET /`. Errors are `error: <reason>`
with appropriate HTTP 4xx/5xx status codes.

## 4. Build and run

Build: `make exogpl` from the repo root produces `exogpl/exogpl`.
Run: `./exogpl/exogpl --serve --port 7661`.
Console mode: `./exogpl/exogpl /audit?path=src`.

## 5. Audit endpoint

`GET /audit?path=<dir>` scans a directory recursively for source files and
checks each for GPL v3.0 license headers. Scanned extensions: `.c`, `.h`,
`.cpp`, `.hpp`, `.cc`, `.cxx`, `.java`, `.go`, `.rs`, `.py`, `.sh`.

For each file, the scanner checks the first 100 lines for the presence of
`GNU GENERAL PUBLIC LICENSE` followed by `Version 3` or `Version 3.`.
It also extracts the first `Copyright` or `COPYRIGHT` notice found in the
first 100 lines. Results are capped at `MAX_FILES` (10 000) entries.

The response is a formatted compliance report including: total files scanned,
compliant count, missing header count, incompatible dependency count,
and per-file detail lines with `[GPL]` or `[NO GPL] NON-COMPLIANT` markers.

## 6. File audit endpoint

`GET /file?path=<file>` audits a single source file for GPL v3.0 compliance.
Returns a structured response including: file path, whether a GPL header
was found (`Has GPL header: yes|no`), compliance status (`Compliant:
yes|no`), and any detected copyright notice.

```
GET /file?path=src/main.c
```

## 7. Report endpoint

`GET /report` generates a formatted GPL v3.0 compliance report using the
result of the most recent `/audit` invocation. The report is stored in a
static buffer and includes: target path, file count, compliant count,
missing header count, incompatible dependency count, and per-file details
with copyright notices truncated to 80 characters.

```
GET /report
```

If no audit has been run yet, returns `error: no audit run yet; use /audit
first`.

## 8. Dependency scan endpoint

`GET /deps?path=<dir>` scans a directory for `LICENSE` and `COPYING` files.
For each found file, reads its first line as a license identifier and checks
compatibility against a hardcoded compatibility list.

Compatible licenses (substring match): GPL, GPL-2.0, GPL-3.0, LGPL,
LGPL-2.1, LGPL-3.0, AGPL, AGPL-3.0, MPL, MPL-2.0, MIT, BSD, BSD-2,
BSD-3, ISC, Apache, Apache-2.0, Zlib, Unlicense, CC0, Public Domain,
WTFPL, 0BSD.

Incompatible licenses (substring match): Proprietary, Commercial, All
Rights Reserved, EULA, SSPL, BSL, Commons Clause.

```
GET /deps?path=.
```

## 9. Contributors endpoint

`GET /contributors?path=<dir>` is a reserved endpoint for git-based
contributor tracking. Currently not implemented — always returns
`error: contributors scan not yet implemented (git-based)`.

## 10. Build version

The module version is defined by `EXO_VERSION` (default `"0.1.0"` in
`src/exogpl.h`) and is reported by `--version` and embedded in the self-
describing spec at `GET /`.

## 11. Internals

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

## 12. Tests

`make test-exogpl` runs the full suite (8 checks, <1s). The QMS gate
runs `./exogpl/exogpl --version`.

`./exogpl/test/test.sh` runs the exogpl test suite: version flag, no-arg spec
print, ping, help, directory audit (self-audit of `src/`), report
generation, single-file audit, and missing-parameter error handling.
Coverage includes the console operation surface: no-args prints the guide,
`/audit?path=src` runs in-process without a server, `/file?path=...`
audits a file, `/report` generates a report, and `--version` works.

## 13. Limitations

- Contributors scan (`/contributors`) is not implemented — requires git
  history analysis which is out of scope for v0.1.0.
- Dependency scanning only checks `LICENSE` and `COPYING` files in the
  target directory root; no recursive or transitive dependency resolution.
- No git-based analysis: no commit history, author tracking, or license
  change detection.
- License detection is limited to the first 100 lines of source files.
- License compatibility uses simple substring matching — ambiguous
  license names may produce false positives.
- No support for license exceptions (e.g., GCC Runtime Exception,
  Linking Exception).

## 14. Robustness

The daemon SHALL never crash on malformed input: oversized paths are
rejected (400), unknown paths return `error: unknown op` (served as 400
with HTTP 200 — the dispatch function returns error strings directly),
and malformed requests are handled gracefully with `error: bad request`
(HTTP 400). The dispatcher handles missing `path=` parameters with
`error: missing path= parameter` (400). The audit path is checked for
readability; non-readable directories return `error: path not readable`
(400) for the deps endpoint. The scanner skips hidden files (names starting
with `.`) and non-regular files during recursive traversal.

## 15. Error handling

All errors follow the `error: <reason>` format with appropriate HTTP
status codes:

- 400: malformed request, missing parameters, non-readable paths
- 404: unknown operations (returned as `error: unknown op`)

Error messages are plain text, human-readable, and include actionable
information where possible (e.g., `error: missing path= parameter`,
`error: no audit run yet; use /audit first`).
