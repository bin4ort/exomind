# exodoc v0.4.0-alpha.1 — the documentation auditor for the AI-native stack

`exodoc` v0.4.0-alpha.1 is a batch command-line auditor (C11, zero dependencies:
libc only) that checks component documentation against the ISO 9001
§7.5-flavored standard in [standard.md](standard.md) — the stack's quality
gate. It reads the stack manifest `docs/stack.tsv` (one component per line:
`name<TAB>dir<TAB>port<TAB>...`), then for each component verifies that its
`README.md` satisfies the standard's clauses: identification (purpose
heading, version token), required sections (build, run, API, state, tests,
honesty), and — with `--live` — that the documented API and version agree
with the running daemon's self-describing `GET /` spec. It never crashes on
malformed input: documents and manifests are capped, control bytes are
stripped, and unreachable daemons are reported as `SKIP` (never fatal).

Part of the [exomind stack](../README.md) — the main README is the full
stack reference (this file has the complete exodoc documentation).

## Build

```
make exodoc        # from the repo root (root Makefile wires this in)
# or directly: make -C exodoc
```

Produces `exodoc/build/exodoc`. Requires only a C11 compiler.

## Usage

```
./exodoc/build/exodoc audit [--stack <manifest>] [--base <dir>]
                            [--exomind http://127.0.0.1:7654]
                            [--out <file>] [--live] [--json]
```

- `--stack` — manifest path (default `docs/stack.tsv`)
- `--base` — base dir for component dirs (default `.`)
- `--exomind` — persist `exodoc:audit:*` scores + a summary note to exomind
- `--out` — also write the report to a file
- `--live` — crawl daemons; verify version + API conformance against `GET /`
- `--json` — machine-readable report (for the future QMS component)

### Operation form (one console grammar for the whole stack)

```
./exodoc/build/exodoc /audit?live=1&stack=docs/stack.tsv&base=.&exomind=http://127.0.0.1:7654&out=report.txt
```

Runs one audit with the same options as the `audit` subcommand, using the
`/op?k=v…` syntax the daemon modules serve, so daemons and batch tools
speak one grammar:

| op | params | maps to |
|----|--------|---------|
| `/audit` | `stack`, `base`, `exomind`, `out`; flags `live=1`, `json=1` | `audit [--stack …] [--base …] [--exomind …] [--out …] [--live] [--json]` |

Values are literal (no URL decoding); `&` separates parameters. Flags
(`live`, `json`) accept off values `0`/`false`/`no`/`off`. Unknown
operations or parameters exit 2. Exit codes are the subcommand's — 0 for
a completed run regardless of findings (the gate is the report line).

Exit status is 0 for a completed run regardless of failures; the gate is
the report line `=== audit: N pass, M fail (score X%) ===` — integration
wiring greps that line for `0 fail`.

## API

`exodoc` is a batch tool, not a daemon: it has no HTTP endpoints, no port,
and no long-running process. Its "endpoints" are CLI subcommands:

| method | path | purpose |
|--------|------|---------|
| `audit` | `--stack <tsv>` | run all checks on the manifest's components |
| `audit` | `--live` | additionally verify against running daemons |
| `audit` | `--exomind <url>` | write scores + note into exomind |
| `audit` | `--json` | emit machine-readable report |
| `--version` | — | print `exodoc v<X.Y.Z>` |
| `--help` | — | print usage |

## Checks (implemented from standard.md)

| check | clause | what it verifies |
|-------|--------|------------------|
| c1 purpose | §2 | `# <component>` heading + non-empty intro paragraph |
| c2 build | §3 | Build/quickstart section, non-empty |
| c3 run | §3 | Run/usage section, non-empty |
| c4 api | §3 | API/endpoints section, non-empty |
| c5 state | §3 | Durability/architecture/design section |
| c6 tests | §3 | Tests section |
| c7 honesty | §3 | Limitations/roadmap section |
| c8 version | §2/§4 | version token present; matches daemon with `--live` |
| c9 api-conformance | §5 | documented endpoints == live `GET /` endpoints |

## Internals

- One pass over the manifest, then one pass over each component's README
  (headings indexed first, sections ranged to the next same-or-higher
  heading, endpoints normalized and deduplicated, version token scanned
  per the §4 rules).
- Live ground truth comes from `GET /` on the component's port (and the
  local `build/<name> --version` binary when present) — self-description
  is authoritative.
- Every check is independently `PASS` / `FAIL` / `SKIP`; `SKIP` never
  counts against a component's score, so a missing daemon degrades the
  report instead of breaking it.
- Stateful dogfooding: with `--exomind`, each component's score is stored
  as `exodoc:audit:<ts>:<component>` and a summary line lands in the note
  feed, so doc-debt history is itself queryable.

## Tests

```
make test-exodoc   # from the repo root: exodoc suite + live audit gate
bash exodoc/test/test.sh   # standalone: fixture-based, own temp dir
```

The suite runs a live audit against its own fake daemons (PASS/FAIL/SKIP
math), JSON validity, `--out` file writing, down-daemon SKIP behavior,
version mismatch detection via binary and via spec, API mismatch detection,
and garbage-doc robustness (NUL bytes, oversized lines) — `exodoc` must
never crash on them. Needs `curl` and `python3` (the fake daemons).

## Limitations

- `--live` verifies version and endpoint sets only; it does not exercise
  every endpoint's behavior — behavioral conformance is the job of each
  component's own test suite.
- The manifest format, section synonyms and endpoint normalization are
  fixed by `standard.md` v0.1.0; a doc that uses exotic heading wording
  outside the synonym table will be flagged even if a human finds it
  adequate.
- A component's README version token is the FIRST `X.Y.Z` in the document;
  docs must therefore state the version before any incidental numeric
  collocation.

## License

GPL-3.0-only — see [LICENSE](../LICENSE).
