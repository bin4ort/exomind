# exomind v0.3.0 — the AI-native software stack

A software stack designed for, built by, and used by AI agents — with
humans welcome as reviewers and operators. Six components, zero
dependencies, plain-text machine-first APIs, and a development loop that
uses the stack to build the stack: every component is developed while the
others run, audited by the quality system, and dogfooded in the process.

**License: GPL-3.0-only** (see [LICENSE](LICENSE)).
**Status: Early Alpha** — the first packaged release is `v0.4.0-alpha.1`
(pacman + apt packages, see [Releasing](packaging/release.sh)).

## The stack

| port | component | role | README |
|------|-----------|------|--------|
| 7654 | **exomind** | durable long-term memory: key/value, notes, ranked search, TTLs, snapshots, scoped tokens, vector recall | this file |
| 7655 | **exosched** | the alarm clock: scheduled reminders (`in 90s` / `at` / `every`), WebSocket push, state persisted in exomind | [exosched/](exosched/README.md) |
| 7656 | **exoflow** | swarm orchestrator: dependency-graph flows, claims, deadlines, claim timeouts, self-looping flows | [exoflow/](exoflow/README.md) |
| — | **exodoc** | documentation auditor: ISO 9001 §7.5-flavored standard, live API conformance | [exodoc/](exodoc/README.md) |
| 7657 | **exoqms** | universal Quality Management System: objectives, NCs, audit programs, trends, field modules for any language | [exoqms/](exoqms/README.md) |
| 7658 | **exocrawl** | AI-native research: independent private metasearch, token-efficient HTML→text extraction, concurrent scraping | [exocrawl/](exocrawl/README.md) |
| 7659 | **exocontext** | context continuity: bounded recency-ranked digest of an agent's state and notes | [exocontext/](exocontext/README.md) |
| — | **exokit** | behavioral development kit: every software carries its own contract + ledger + runner shims; translate by regenerating from the contract | [exokit/](exokit/README.md) |

All components: C11, POSIX, zero compile dependencies (TLS in exocrawl
uses the ubiquitous `curl` binary). Every API is plain text and
self-describing (`GET /` prints the full spec).

## Build

Zero dependencies: a C11 compiler and POSIX.

```sh
make            # produces build/exomind
make exosched exoflow exodoc exoqms exocrawl exocontext exokit
make test test-exosched test-exoflow test-exodoc test-exoqms test-exocrawl
```

## Install

From source:

```sh
make                    # all modules
make test test-exosched test-exoflow test-exodoc test-exoqms test-exocrawl test-exocontext test-exokit
make install PREFIX=~/.local   # default /usr/local
```

Packaged (Early Alpha `v0.4.0-alpha.1`, built by `packaging/release.sh`):

- **pacman/Arch**: download `exomind-0.4.0alpha1-1-x86_64.pkg.tar.zst` from
  the GitHub release and `pacman -U` it (or `makepkg -i` in `packaging/`).
- **apt/Debian-Ubuntu**: download `exomind_0.4.0~alpha1-1_amd64.deb` and
  `sudo apt install ./exomind_0.4.0~alpha1-1_amd64.deb` (or build with
  `dpkg-buildpackage`).
- or the source tarball + SHA256SUMS from the release, verified with
  `sha256sum -c`.

## Usage: console or MCP server

Every module runs two ways:

```sh
# 1) from the console: $ exo<module> <options>
exomind --port 7654 --data exomind.dat --backup ~/exomind-backups --mandate "read memory first"
exosched --exomind http://127.0.0.1:7654
exokit verify

# 2) as an MCP server (for AI agents): $ <module>-server <options>
exomind-server            # stdio MCP: initialize / tools/list / tools/call
exocrawl-server --proxy http://proxy:8080

# whole-stack help, titled by module name, on ANY module:
exomind --help modules
exomind --help exosched   # one module's guide

# keys (auth) file management, console subcommands:
exomind keys add alpha:ro:scope=logs/* --keys ~/.config/exo/keys
exomind keys list --keys ~/.config/exo/keys
exomind keys remove alpha --keys ~/.config/exo/keys
```

Shared options: `--host <ip>`, `--port <n>`, `--keys <file>`, `--token`,
`--log-level error|warn|info|debug`, `--rate-limit <n>/s`, `--help
[modules]`, `--version`. On the bound address each daemon serves its API
at both `/` (base usage) and `/exo<module>` (e.g. `GET
127.0.0.1:7654/exoexomind/ping`). Proxy applies where it makes sense
(exocrawl).

## Memory model (exomind)

Two stores instead of one pile:

- **Main memory file** (`--data`, default `exomind.dat`) — general and
  important facts that are NOT project specific.
- **Project memory file** — lives in the agent's project root (auto-
  detected upward from the CWD via a `.git`/`.exo` marker, or
  `--project-root`), at `<root>/.exo/project.dat`, even when exomind
  runs from elsewhere. Keys with the `p:` prefix and any request with
  `proj=1` operate on it; `/search` and `/recall` cover both stores.

**Backups** (`--backup <dir>`, `POST /backup`): timestamped copies of the
main memory file, newest 24 kept — redundancy against a failing host.

**Associations** — memories are never silently deleted:
`POST /outdate?key=k&reason=...` keeps the value, records
`history:<k>` and marks `outdated:<k>` (view with `/outdated`, clear
with `/revive`); `POST /link?from=a&to=b&rel=...` cross-references
memories (`/assoc` lists both directions). When an old error recurs you
can see the fix, why it was tried, and what superseded it.
`GET /recall?q=` bundles search + outdated + history + associations.

**Mandate** (`--mandate "..."` or `--mandate-file`, `POST /mandate`) —
the memory module is not optional: agents must read `/mandate` and
acknowledge with `agent:<id>:ready`. The QMS `memory-awareness` check
fails any configured agent that has not acknowledged.

## API (exomind)

Plain text, lowercase answers (`ok`, `missing`, `error: <reason>`), one
record per line, tab-separated. An agent learns the whole API from
`GET /` — the software describes itself.

| method | path | purpose |
|--------|------|---------|
| GET | `/` | help / self-describing spec |
| GET | `/ping` | liveness: `pong` |
| POST | `/set?key=k` | store a value (raw body + `?key=`, form, or JSON; `ttl=` optional) |
| GET | `/get?key=k` | read raw value; 404 body `missing` |
| POST | `/append?key=k` | append body to a value, newline-separated |
| DELETE | `/del?key=k` | delete |
| GET | `/list` | keys (`prefix=`, `limit=`, `offset=`, `sort=desc`) |
| GET | `/search?q=t` | ranked substring search over keys and values |
| POST | `/note` | store body as a timestamped note, answers `ok <key>` |
| GET | `/notes` | notes newest-first (`q=`, `limit=`, `offset=`) |
| POST | `/batch` | JSON array of ops; one result line per op |
| POST | `/embed?key=k` | store a vector embedding for a key |
| GET | `/embed?key=k` | read a stored embedding |
| DELETE | `/embed?key=k` | remove an embedding |
| POST | `/sim` | rank keys by embedding similarity to the body text |
| GET | `/snapshot`, POST `/restore` | full dump / atomic restore |
| GET | `/stats` | counters and health |
| POST | `/backup` | write a timestamped backup copy |
| GET | `/project` | project store location |
| POST | `/outdate?key=k&reason=...` | mark a memory outdated (kept in history) |
| GET | `/outdated?key=k` | outdated marker + history of a key |
| POST | `/revive?key=k` | clear an outdated marker |
| POST | `/link?from=a&to=b&rel=...` | associate two memories |
| GET | `/assoc?key=k` | associations of a key (both directions) |
| GET | `/recall?q=` | search + outdated + history + associations |
| GET | `/mandate` | the mandatory briefing (ack: `agent:<id>:ready`) |

Any listing endpoint accepts `json=1` for machine-readable JSON.

```sh
# remember a fact forever
curl -X POST 'localhost:7654/set?key=project:alpha:deadline' -d '2026-09-01'

# remember it only for an hour
curl -X POST 'localhost:7654/set?key=tmp:build:status&ttl=3600' -d 'building'

# accumulate a work log
curl -X POST 'localhost:7654/append?key=log:session:42' -d 'step 3 done'

# many operations, one round trip
curl -X POST localhost:7654/batch -d '[["set","a","1"],["get","a"],["del","a"]]'

# leave a note to your future self
curl -X POST localhost:7654/note -d 'remember to revisit the parser edge case'

# find everything that mentions "parser"
curl 'localhost:7654/search?q=parser'
```

## Internals

- **Storage** — append-only log (Bitcask-style) with an in-memory hash
  index. Records carry CRC32 checksums over key+value. Write = one
  append; reads are a single `pread`.
- **Crash safety** — every acknowledged interactive write is fsynced. A
  torn tail record from a crash is detected on load and the log
  truncated back to the last good offset.
- **Prefix index** — a sorted key vector turns `/list?prefix=` into an
  O(log n + k) range walk (queries verify liveness against the hash, so
  the index can safely be stale); `/search` stays linear.
- **Compaction** — when the log exceeds 64 MB with >33% dead bytes, live
  records are rewritten atomically via rename.
- **Snapshot/restore** — `GET /snapshot` emits a length-prefixed plain
  dump; `POST /restore` rebuilds through temp file + fsync + rename.
- **Auth scopes** — `--tokens` file tokens can be read-only and/or
  prefix-scoped; every endpoint enforces the scope.
- **TTLs** — lazy expiry on read/query; expired keys never leak back
  during compaction.
- **Concurrency** — thread per connection, one mutex over the store.
- **No dependencies** — no libs, no package manager, single
  static-friendly binary.

## Tests

`make test` covers every endpoint, persistence across restarts,
tombstone survival, TTL expiry, bearer + scoped auth, SIGKILL crash
recovery, concurrent writers, snapshot round-trips, and vector recall.
The QMS gate runs `./build/exomind --version` (in-budget smoke).

## Limitations

- No clustering or replication built in (single-writer log).
- Substring search (`/search`) is linear in the number of keys
  (10k keys ≈ 30 ms); only prefix listing is indexed.
- The GUI-free, machine-first design assumes an agent or CLI operator.

## The development loop

The stack is developed the way it is used — by agents, in parallel, with
the stack itself holding the state:

1. **Memory** (exomind) is the session state: `agent:*` keys record
   progress, `metric:*` keys record measurements, `debt:*` keys track
   TODO items, `issue:*` keys track problems, `norm:*` keys hold
   standards.
2. **Scheduling** (exosched) sets agent deadlines; fired reminders land
   as notes in the memory.
3. **Orchestration** (exoflow) structures work as dependency graphs with
   claims, deadlines, and claim timeouts — a stuck worker releases its
   step deterministically.
4. **Quality** (exoqms) audits everything — component tests, doc
   compliance, code safety (any language), debt, hygiene, secrets,
   asset logic, docs coverage, and agent health: the freeze detector
   flags an agent whose reminder fired with no response and no
   deliverable.
5. **Research** (exocrawl) provides external knowledge: private
   metasearch and clean-text extraction, feeding standards and findings
   back into the memory.
6. **The kit** (exokit) makes every software its own SDK: the software
   carries a behavioral contract (function inventory + examples ledger +
   runner shims), and any development — especially language translation —
   regenerates from the contract instead of porting code. The QMS gates
   on it (`kit-fidelity`).

## Knowledge corpus

Freely-available international norms harvested into memory (`norm:*`
keys, registry `norm:index`): RFC 2119, RFC 8259, PEP 8, WCAG 2.2, NN/g
heuristics. Harvest with `bash exocrawl/contrib/fetch-norms.sh`.

## Quality gate

```sh
make test-exodoc     # exodoc suite + live documentation audit (0 fail)
make test-exoqms     # QMS suite: code-safety, debt, hygiene, secrets
make audit-stack     # full exoqms audit program against the live stack
```

Documentation debt is CI-checked: `exodoc audit --live` compares every
component README against its live `GET /` spec and fails the build on
any drift.

## Process & records

- **TODO.md** — the roadmap (mirrored as `todo:*` keys in exomind)
- **ISSUES.md** — the issue log (mirrored as `issue:*` keys)
- Every merged change lands with tests, the doc gate green, and the QMS
  audit at 100%.

## License

GPL-3.0-only — see [LICENSE](LICENSE).
