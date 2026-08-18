# exoqms — the Quality Management System for the AI-native stack

`exoqms` v0.4.0-alpha.1 is a QMS daemon (C11, zero dependencies: libc + pthread
only) that turns the ISO 9000 family into running code for the exomind
stack. It holds quality objectives, runs ISO 19011 audit programs
against the live stack, records non-conformities (NCs) with a
full corrective-action lifecycle, and publishes every milestone into
exomind's note feed. Its durable state lives entirely inside
[exomind](../README.md) under `exoqms:*` keys, so the QMS itself is
auditable and survives restarts like every other layer.

The audit program runs the fifteen checks defined in
[`standard.md`](standard.md) (the default `detect` criteria run nine of
them), invoking [`exodoc`](../exodoc/README.md)
(the documentation auditor), [`exoqms-ui`](ui/README.md) (the UI
quality auditor), [`exoqms-code`](code/README.md) (the code-safety
analyzer) and [`exoqms-svg`](svg/README.md) (the asset-logic analyzer)
as child processes under a hard 5-second timeout each.

Part of the [exomind stack](../README.md) — the main README is the full
stack reference (this file has the complete exoqms documentation).

## ISO mapping

| ISO document | principle | exoqms feature |
|--------------|-----------|----------------|
| ISO 9000:2015 | concepts and vocabulary: quality is conformance to stated requirements | the checks in `standard.md` section 5 define the stated requirements, machine-executable |
| ISO 9001:2015 §5.2 | quality policy — commitment to quality | the quality policy statement in `standard.md` section 2 |
| ISO 9001:2015 §6.2 | quality objectives | `POST /objectives` — measurable objectives with metric keys and targets |
| ISO 9001:2015 §7.5 | documented information | `doc-compliance` check runs `exodoc audit --live` on every audit |
| ISO 9001:2015 §8.7 | control of non-conforming outputs | `POST /nc` — NCs with severity (`major`/`minor`) and source |
| ISO 9001:2015 §9.1 | monitoring, measurement, analysis and evaluation | `metrics` check — the `metric:iterN:tests_passing` trend |
| ISO 9001:2015 §10.2 | corrective action | the NC lifecycle `open → analysis → corrective → verify → closed` with mandatory evidence |
| ISO 9004:2018 | sustained success over the long term | the trend verdict (`up`/`flat`/`down`) and the stagnation flag in `GET /trends` and `GET /report` |
| ISO 19011:2018 | audit programs: planning, criteria, evidence, records | `POST /audit` — named audit programs, check criteria, per-check findings with evidence, durable records |

## quickstart

Build (from the repo root; the root Makefile wires the sub-project in):

```
make exoqms        # builds exoqms/build/exoqms AND exoqms/ui/build/exoqms-ui
```

Run against the shared swarm (the QMS stores its state in the shared
exomind so audits and NCs are visible to every agent):

```
./exoqms/build/exoqms --port 7691 \
    --exomind http://127.0.0.1:7654 \
    --exosched http://127.0.0.1:7655 \
    --exodoc ./exodoc/build/exodoc \
    --ui ./exoqms/ui/build/exoqms-ui \
    --code ./exoqms/code/build/exoqms-code \
    --svg ./exoqms/svg/build/exoqms-svg \
    --repo . --agents a,b,b1,b2,b3,e &
```

Then run an audit program:

```
curl -s -X POST "http://127.0.0.1:7691/audit?target=exoqms/ui/fixtures/good.html" \
  --data-binary $'stack audit\tcomponent-tests,doc-compliance,dogfood,ui-audit,metrics,code-safety,asset-logic\ta,b,b1,b2,b3,e'
# ok <audit-id> <score>%
```

## usage

```
./exoqms/build/exoqms [--serve] [--host <addr>] [--port <n>]
                      [--exomind <url>] [--exosched <url>]
                      [--exodoc <path>] [--ui <path>] [--code <path>]
                      [--svg <path>] [--kit <path>] [--repo <dir>]
                      [--agents <a,b,c>] [--notes24h <n>] [--token <t>]
```

Defaults: port 7657, exomind `http://127.0.0.1:7654`, exosched
`http://127.0.0.1:7655`, exodoc on `PATH`, repo `.`, agents
`a,b,b1,b2,b3`, notes24h 5. Set `--token` (or env `EXOQMS_TOKEN`) to
require `Authorization: Bearer <token>` on every request. `--ui`
points at the exoqms-ui binary and enables the `ui-audit` check;
`--code` enables `code-safety`; `--svg` enables `asset-logic`;
without a module's binary that check reports `skip`.

`--serve` (or an explicit `--port`) is the only way to run the HTTP
daemon; without it the binary never binds a port. With no arguments it
prints the same spec text `GET /` serves and exits 0.

### console operations

`argv[1]` starting with `/` runs one operation in-process through the
same dispatcher the daemon serves, prints the response body and exits
(0 ok, 1 the operation failed with an `error:` response, 2 usage —
unknown operation):

```
./exoqms/build/exoqms /objectives                         # GET list
printf 'iter10 passing\tmetric:iter10:tests_passing\t50\n' |
    ./exoqms/build/exoqms /objectives                     # POST add
./exoqms/build/exoqms /nc --body $'broken build\tmajor\tlog tail'
./exoqms/build/exoqms /audit?criteria=metrics             # run audit
./exoqms/build/exoqms /audit?id=<audit-id>                # report
./exoqms/build/exoqms /report /trends /audits /issues /ping
```

`/objectives`, `/nc` and `/audit` select POST when a body is supplied
(`--body <text>` wins; otherwise stdin, read only when it is not a
terminal); without a body they fall back to GET (list / detail).
`/audit?criteria=<a,b,c>` runs an audit program named `console` from
the query string; other options (`--exomind`, `--repo`, `--code`,
...) work as in daemon mode.

## endpoints

| method | path | purpose |
|--------|------|---------|
| GET | / | full spec (self-describing) |
| GET | /ping | liveness: `pong` |
| POST | /objectives | add objective (body below) |
| GET | /objectives | list objectives |
| POST | /nc | raise non-conformity (body below) |
| GET | /nc?id=<id> | NC detail |
| GET | /nc?status=<st> | list NCs, filtered |
| POST | /nc?id=<id>&action=<a> | NC lifecycle transition |
| POST | /audit | run an audit program (body below) |
| GET | /audit?id=<id> | audit report with findings |
| GET | /audits | audit program list |
| GET | /issues | detection registry (issue:&lt;check&gt; records) |
| GET | /report | consolidated quality picture |
| GET | /trends | metric trend + verdict |

Add `json=1` to listing endpoints for JSON. Errors are
`error: <reason>` with HTTP 4xx/5xx; request bodies are tab-separated
fields.

## objectives (ISO 9001 §6.2)

`POST /objectives` body: `title<TAB>metric_key<TAB>target`, with an
optional fourth field `period` (default `iter`). `met` means
`value >= target` for numeric targets, equality for string targets; a
missing metric key yields `no-data`. Answer: `ok <id>`.

## non-conformities and corrective action (ISO 9001 §8.7, §10.2)

`POST /nc` body: `title<TAB>severity<TAB>description`, severity is
`major` (breach of a normative clause or regression) or `minor`.
Answer: `ok <id>`; the NC starts `open`.

Lifecycle:

```
open --analyse--> analysis --correct--> corrective --verify--> verify --close--> closed
```

`POST /nc?id=<id>&action=analyse|correct|verify|close` advances the
NC; every transition is written to the exomind note feed as an
audit-trail entry (`nc <id> transition: <from> -> <to> (<body>)`).
Invalid transitions are rejected with 400 naming the expected status.
Closure from any status requires a body of
`corrective_action<TAB>evidence` (a third field is appended as a note,
a fourth sets `closed_by`, default `api`); from `verify` a note alone
is enough. Closed NCs drop out of the open count in `GET /report`.

## audit programs (ISO 19011)

`POST /audit` body: `name<TAB>criteria` — criteria is a comma-separated
list of check ids (`component-tests,doc-compliance,dogfood,ui-audit,metrics,code-safety,asset-logic`;
empty = all seven), with an optional third field `agents` for the
dogfood check. Query `?target=<path>` feeds the `ui-audit` check (and
overrides the scan target of `code-safety` and `asset-logic`).
Answer: `ok <audit-id> <score>%`; `GET /audit?id=` prints the record
plus one findings line per check: `check<TAB>result<TAB>evidence`.
The score is `100 * pass / (pass + fail)` rounded, `skip` not counted.

| check | passes when |
|-------|-------------|
| component-tests | every manifest test command (5th column of `docs/stack.tsv`) exits 0 within the 5s budget |
| doc-compliance | `exodoc audit --live` reports 0 fail |
| dogfood | every listed agent has `agent:<id>:status`, and ≥ `notes24h` notes exist in the last 24h |
| ui-audit | `exoqms-ui` finds 0 findings on the `?target=` page |
| metrics | the `metric:iterN:tests_passing` trend is not `down` |
| code-safety | `exoqms-code` reports 0 **major** findings on the stack's own C source (default target: the manifest source dirs; minor findings non-fatal) |
| asset-logic | `exoqms-svg --shape auto` reports 0 **major** findings on the stack's own SVG assets (default target: the repo root; minor findings non-fatal) |

Each check runs under a hard 5s timeout; children that overrun are
SIGKILLed and the check fails with `timed out`.

## the universal project config (iter7, v0.2.0)

Any project — inside the stack or foreign — can be audited via a
`.exoqms.json` at the repo root. The daemon loads it on startup (the
`--repo` directory) and it drives the audit program:

```json
{
  "languages": ["auto"],
  "rules": {"debt": true, "hygiene": true, "secrets": true, "code-safety": true},
  "thresholds": {"debt": 10},
  "test": ["ctest --test-dir build"],
  "docs": ["README.md", "CHANGELOG.md"],
  "ignore": ["build/", "install/", "assets/"]
}
```

All keys optional; malformed JSON degrades to defaults with a stderr
warning. `languages` pins the code-safety analyzer (`auto` = detect),
`rules` toggles the new checks, `thresholds.debt` is the maximum number
of `debt-*` findings that still passes, `test` runs whole-project test
commands (5s budget each, from the repo root) when there is no stack
manifest, `docs` names the required files for `doc-compliance` in that
mode, and `ignore` globs are handed to the analyzer scans.

The three universal checks share one `exoqms-code --rules` scan per
audit (memoized), partitioned by check-id prefix:

| check | passes when |
|-------|-------------|
| debt | `debt-*` findings ≤ `thresholds.debt` (default 10) |
| hygiene | 0 `hygiene-*` findings |
| secrets | 0 `secrets-*` findings (matched lines masked to `***`) |

The rule files live in the `--rules` directory, else `rules/` next to
the `--code` binary, else `<repo>/exoqms/code/rules`; without them the
checks report `skip` so a half-wired deployment degrades honestly.

## the field modules

Three sibling quality-audit engines live under `exoqms/`, each a
zero-dependency C11 batch binary with its own fixtures and test suite:

- [`exoqms/ui`](ui/README.md) — the UI quality auditor (7 defect
  classes: emoji icons, overlapping controls, misaligned siblings,
  corner mismatches, missing backgrounds, unstyled sdk-default
  controls, WCAG AA contrast). Permanent fixtures `good.html` (0
  findings) and `bad.html` (12 intentional findings).
- [`exoqms/code`](code/README.md) — the code-safety analyzer:
  error-handling defects in C source (unchecked returns of critical
  libc calls, missing error paths, null-deref paths). This is the
  deployment loop's fix step applied to code: the audit program runs
  it against the stack's own source, findings become NCs, and the
  fixes are verified by re-auditing.
- [`exoqms/svg`](svg/README.md) — the asset-logic analyzer: generated
  SVG shape rules (tree rule-set: stem, crown, proportions, symmetry,
  degeneracy). Fixtures: `tree-good.svg` (clean) and six deliberately
  broken trees.

The daemon invokes them through the `ui-audit`, `code-safety` and
`asset-logic` checks. Build all of them with `make exoqms` from the
repo root (or `make qms-modules` for just code + svg).

## durability (dogfooding exomind)

- Every objective, NC and audit program is persisted as an
  `exoqms:obj:*`, `exoqms:nc:*` or `exoqms:audit:*` key in exomind;
  config keys are `exoqms:config:agents` and `exoqms:config:notes24h`.
- On startup exoqms reloads all `exoqms:*` keys from exomind; if
  exomind is down the reload is retried every second in the background
  and the daemon serves requests with an empty registry meanwhile.
- Every objective, NC creation, NC transition and audit score is also
  published to the note feed — the audit trail is exomind's paper
  trail, complete by construction.
- Records written while exomind is down are rejected (500
  `error: exomind unavailable`) rather than lost silently.

## tests

```
make test-exoqms   # from the repo root: exoqms suite + module suites
```

`make -C exoqms test` runs the daemon's suite (`exoqms/test/test.sh`,
~60s — too slow for the 5s audit budget, so the stack manifest declares
`./build/exoqms --version` as the in-budget smoke command instead and
`make test-exoqms` remains the full-suite gate); the module suites are
`make -C exoqms/ui test` (28 checks), `make -C exoqms/code test` and
`make -C exoqms/svg test` (54 checks).

## limitations

- The check budget is 5s per check (normative, `standard.md` §5.2):
  only test commands that fit the budget may be declared in
  `docs/stack.tsv`; the full suites are run by `make test-exoqms`.
- The `ui-audit`, `code-safety` and `asset-logic` checks are static
  analyzers, not a browser / a compiler / a rendering engine: they
  catch defect classes, not semantics (see each module's README for
  the honest limitations).
- The `metrics` check needs at least two iterations of
  `metric:iterN:tests_passing`; with fewer it reports `skip`.
- Severity is assigned by the author of the NC; the daemon checks the
  vocabulary (`major`/`minor`), not the judgment.
- `GET /report` is a one-shot picture, not a historical chart; history
  lives in the note feed and the `exoqms:*` keys.

## License

GPL-3.0-only — see [LICENSE](../LICENSE).
