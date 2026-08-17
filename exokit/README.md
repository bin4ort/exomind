# exokit v0.4.0-alpha.1 — the behavioral development kit

Every software carries its own development kit: a `kit/` directory that
is the software's SDK-for-itself. Not a library, not a framework — a
tool + rules set that makes any development process (in any language, in
any combination) scaffold on a **behavioral contract** instead of on the
code.

The reason: standards (RFC 2119, PEP 8, …) limit implementation issues,
but not *logical* ones. A port (Rust+SvelteKit → C++ + vanilla JS/CSS)
can compile cleanly and still be a different program. exokit attacks
that at the root: the **contract is the truth, not the code**.

**License: GPL-3.0-only** (see the repository root).

Part of the [exomind stack](../README.md) — the main README is the full
stack reference (this file has the complete exokit documentation).

## Build

```sh
make            # produces exokit/build/exokit
make test       # 39 hermetic tests
```

Zero dependencies: C11, POSIX, no daemon, no server (batch tool).

## Run

```sh
# scaffold a kit into <dir>/kit (contract + examples + runner shims)
exokit init <dir>

# best-effort function inventory from C/C++ sources
exokit extract <src-dir> --out kit/contract.tsv

# run the behavioral ledger through your implementation's runner
exokit verify            # in the project root

# compare two inventories (translation completeness)
exokit diff contract.a.tsv contract.b.tsv --exact

# QMS-facing audit: completeness + ledger fidelity, JSON findings
exokit audit
```

### operation form (one console grammar for the whole stack)

The same subcommands also run through the `/op?k=v…` syntax the daemon
modules use, so the whole stack has one console grammar:

| op | params | maps to |
|----|--------|---------|
| `/init` | `dir` | `init <dir>` (default `.`) |
| `/extract` | `src`, `out`, flag `append=1` | `extract <src> [--out <f>] [--append]` |
| `/verify` | `kit`, `runner`, `fn` | `verify [--kit <d>] [--runner <c>] [--fn <n>]` |
| `/diff` | `a`, `b`, flag `exact=1` | `diff <a> <b> [--exact]` |
| `/audit` | `kit` | `audit [--kit <d>]` |

e.g. `exokit /verify?kit=kit` from the project root == `exokit verify`.
Values are literal (no URL decoding); `&` separates parameters. Flags
accept off values `0`/`false`/`no`/`off`. Exit codes are the
subcommand's (0 = pass, 1 = findings/bad args, 2 = usage error /
unknown operation).

## API

Plain-text TSV everywhere. The files:

| file | contents |
|------|----------|
| `kit/contract.tsv` | `fn<TAB>sig<TAB>pure(0/1)<TAB>side_effects<TAB>notes` |
| `kit/examples.tsv` | `fn<TAB>args<TAB>expected<TAB>desc<TAB>err(0/1)` |
| `kit/config` | `runner<TAB><cmd>` and `max_examples<TAB><n>` |
| `kit/runners/` | one executable shim per language |

The runner protocol is deliberately primitive and language-agnostic: the
runner reads one line `fn<TAB>args` on stdin and prints exactly one
result line on stdout (`<result>` or `error: <text>`). A shim is ~10
lines in any language; the kit scaffolds C, C++-ready, Rust, JS and
Python shims.

```sh
# the classic workflow
exokit extract src --out kit/contract.tsv   # 1. inventory
#   2. write examples for every entry (incl. error cases)
#   3. implement a runner for YOUR language
exokit verify                                # 4. ledger green?
exokit audit                                 # 5. gate
```

## Internals

- **Contract-first** — `extract` is a best-effort C/C++ scanner (skip
  `kit/`, entry points; flatten signatures); the inventory it produces is
  a starting point, the user curates it. Other languages write the
  inventory by hand — which is the point: the contract is a decision,
  not a dump.
- **Fidelity ledger** — `verify` forks the runner once per example (two
  pipes: stdin feed + stdout capture), compares byte-exact against the
  expected column, and treats `err=1` rows as pass only on an `error:`
  response. A runner that silently returns a different result is
  caught — this is the anti-drift mechanism for translations.
- **Audit** — `audit` enforces the rules: every contract entry needs ≥ 1
  example (R1/R2), every example must pass (R6). Findings are JSON with
  `severity: major`, the same contract as the QMS field modules, so
  `exoqms --kit` gates the whole stack on it.
- **Diff** — inventories are compared on the `fn` column; `missing`
  always fails, `extra` fails only with `--exact` (translation mode).

## Tests

`make test` covers scaffolding, extraction (multi-line, static, entry
points, kit self-exclusion), verification incl. intentional-drift
detection, `--fn`/`--runner` flags, audit completeness + fidelity
findings, and diff semantics (missing / extra / `--exact`).

## Limitations

- `extract` handles plain C/C++ shapes only; templates, macros and
  other languages need manual inventories (documented trade-off: the
  contract is a curated decision).
- `verify` spawns one process per example: large ledgers are slow
  (audits cap at 50 examples by default).
- No JSON arg encoding: args/expected are literal text without tabs or
  newlines (the shims' dispatch parses them).

## Rules (the development kit part)

- R1 contract-first: no public function without an inventory entry.
- R2 every contract entry has ≥ 1 example, including an edge/error case.
- R3 translate by regenerating from the contract, never line-by-line.
- R4 deliver in inventory slices, each slice verified before the next.
- R5 both implementations must pass the same examples ledger.
- R6 the ledger is the only source of truth for behavior.
