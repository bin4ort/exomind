# exokit standard v0.4.0-alpha.1 — the development-kit standard for the AI-native stack

Normative document. This standard defines the requirements for the
`exokit` tool — the behavioral development kit that makes any software
its own SDK-for-itself through a behavioral contract — covering its
operations, file formats, verification semantics, and limitations. Every
requirement below is checkable by `exodoc audit` (doc checks c1–c7, c8)
or by the QMS `kit-fidelity` check.

## 1. Scope

This standard applies to every deployment of the `exokit` component of
the exomind stack. `exokit` is a batch tool (no daemon, no server) that
scaffolds, extracts, verifies, and audits a behavioral contract for any
software. The contract is the source of truth for behavior, not the code.

## 2. Identification

- **c1 purpose** — this document opens with a top-level heading naming
  `exokit` followed by a non-empty first paragraph describing its
  purpose (behavioral development kit: contract-first development).
- **c8 version** — the document contains a version token matching `v?X.Y.Z`
  (the first such token: `v0.4.0-alpha.1`).

## 3. Operations

| command | purpose |
|---------|---------|
| `init <dir>` | scaffold a kit directory (contract + examples + runner shims) |
| `extract <src-dir>` | best-effort function inventory from C/C++ sources |
| `verify` | run the behavioral ledger through runner shims |
| `diff <a> <b>` | compare two inventories (translation completeness) |
| `audit` | QMS-facing audit: completeness + ledger fidelity, JSON findings |

Console-operation form: `exokit /init?dir=...`, `exokit /extract?src=...`,
`exokit /verify?kit=...`, `exokit /diff?a=...&b=...`, `exokit /audit?kit=...`.

## 4. File formats

| file | contents |
|------|----------|
| `kit/contract.tsv` | `fn<TAB>sig<TAB>pure(0/1)<TAB>side_effects<TAB>notes` |
| `kit/examples.tsv` | `fn<TAB>args<TAB>expected<TAB>desc<TAB>err(0/1)` |
| `kit/config` | `runner<TAB><cmd>` and `max_examples<TAB><n>` |
| `kit/runners/` | one executable shim per language |

The runner protocol is language-agnostic: the runner reads one line
`fn<TAB>args` on stdin and prints exactly one result line on stdout
(`<result>` or `error: <text>`).

## 5. Verification

`verify` forks the runner once per example (two pipes: stdin feed + stdout
capture), compares byte-exact against the expected column, and treats
`err=1` rows as pass only on an `error:` response. A runner that returns
a different result is caught — this is the anti-drift mechanism for
translations.

## 6. Audit rules

`audit` enforces the following rules:
- **R1** every contract entry needs at least one example.
- **R2** every contract entry needs at least one edge/error-case example.
- **R6** every example must pass (runner output matches expected).

Findings are emitted as JSON with `severity: major`. The audit is the
gate for the QMS `kit-fidelity` check.

## 7. Development rules

- **R1** contract-first: no public function without an inventory entry.
- **R2** every contract entry has at least one example, including edge/error.
- **R3** translate by regenerating from the contract, never line-by-line.
- **R4** deliver in inventory slices, each verified before the next.
- **R5** both implementations must pass the same examples ledger.
- **R6** the ledger is the only source of truth for behavior.

## 8. Diff semantics

Inventories are compared on the `fn` column. `missing` always fails.
`extra` fails only with `--exact` (translation mode). The diff tool
supports `--exact` flag for strict bidirectional completeness checking.

## 9. Tests

`make test` covers scaffolding, extraction (multi-line, static, entry
points, kit self-exclusion), verification including intentional-drift
detection, `--fn`/`--runner` flags, audit completeness + fidelity
findings, and diff semantics (missing / extra / `--exact`).

## 10. Limitations

- `extract` handles plain C/C++ shapes only; templates, macros, and
  other languages need manual inventories.
- `verify` spawns one process per example: large ledgers are slow
  (audits cap at 50 examples by default).
- No JSON arg encoding: args/expected are literal text without tabs or
  newlines.

## 11. Robustness

As a batch tool, `exokit` exits with code 0 on success (ledger green,
audit pass), 1 on findings or bad arguments, and 2 on usage errors.
The `audit` command never crashes on malformed input: oversized files
are truncated, control bytes are stripped, and missing files are reported
as findings rather than causing a crash.
