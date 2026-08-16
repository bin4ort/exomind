# exomind v0.3.0 — the AI-native software stack

A software stack designed for, built by, and used by AI agents — with
humans welcome as reviewers and operators. Six components, zero
dependencies, plain-text machine-first APIs, and a development loop that
uses the stack to build the stack: every component is developed while the
others run, audited by the quality system, and dogfooded in the process.

**License: GPL-3.0-only** (see [LICENSE](LICENSE)).

## The stack

| port | component | role | README |
|------|-----------|------|--------|
| 7654 | **exomind** | durable long-term memory: key/value, notes, ranked search, TTLs, snapshots, scoped tokens, vector recall | this file |
| 7655 | **exosched** | the alarm clock: scheduled reminders (`in 90s` / `at` / `every`), WebSocket push, state persisted in exomind | [exosched/](exosched/README.md) |
| 7656 | **exoflow** | swarm orchestrator: dependency-graph flows, claims, deadlines, claim timeouts, self-looping flows | [exoflow/](exoflow/README.md) |
| — | **exodoc** | documentation auditor: ISO 9001 §7.5-flavored standard, live API conformance | [exodoc/](exodoc/README.md) |
| 7657 | **exoqms** | universal Quality Management System: objectives, NCs, audit programs, trends, field modules for any language | [exoqms/](exoqms/README.md) |
| 7658 | **exocrawl** | AI-native research: independent private metasearch, token-efficient HTML→text extraction, concurrent scraping | [exocrawl/](exocrawl/README.md) |

All components: C11, POSIX, zero compile dependencies (TLS in exocrawl
uses the ubiquitous `curl` binary). Every API is plain text and
self-describing (`GET /` prints the full spec).

## Build

Zero dependencies: a C11 compiler and POSIX.

```sh
make            # produces build/exomind
make exosched exoflow exodoc exoqms exocrawl
make test test-exosched test-exoflow test-exodoc test-exoqms test-exocrawl
```

## Run

```sh
./build/exomind --port 7654 --data exomind.dat &              # memory
./exosched/build/exosched --port 7655 --exomind http://127.0.0.1:7654 &
./exoflow/build/exoflow --port 7656 --exomind http://127.0.0.1:7654 --exosched http://127.0.0.1:7655 &
./exoqms/build/exoqms --port 7657 --exomind http://127.0.0.1:7654 --exosched http://127.0.0.1:7655 \
  --exodoc ./exodoc/build/exodoc --ui ./exoqms/ui/build/exoqms-ui \
  --code ./exoqms/code/build/exoqms-code --svg ./exoqms/svg/build/exoqms-svg --repo "$PWD" &
./exocrawl/build/exocrawl --port 7658 --cache exomind &
```

Every daemon answers `GET /` with its complete specification; `--token
<secret>` enables Bearer auth. `EXOMIND_TOKEN` works as `--token`.

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
