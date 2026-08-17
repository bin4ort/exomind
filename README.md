# exomind v0.4.0-alpha.1 — the AI-native software stack

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

### Packaging

The stack ships as one package containing all modules: console binaries
(`exomind`, `exosched`, ...), MCP server symlinks (`exomind-server`, ...),
batch tools (`exodoc`, `exokit`). `bash packaging/release.sh` produces
everything (it needs `git`, `make`, and, when present, `makepkg` and
`dpkg-deb`):

```
dist/exomind-<version>.tar.gz      # source tarball for GitHub
dist/SHA256SUMS                    # checksums
dist/exomind-0.4.0alpha1.tar.gz    # Arch source (pkgver-named)
packaging/exomind-0.4.0alpha1.tar.gz
packaging/exomind.SRCINFO          # pacman/AUR metadata (makepkg-generated)
dist/exomind_0.4.0~alpha.1-1_amd64.deb
```

If you run `makepkg` directly, the source tarball must be named
`exomind-$pkgver.tar.gz` (the PKGBUILD globs the extracted directory, so
the internal prefix does not matter). Verify integrity before installing:
`sha256sum -c dist/SHA256SUMS`.

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

## exosched — the alarm clock (port 7655)

A scheduled-reminders + WebSocket-push daemon (C11, zero dependencies:
libc + pthread only). Durable state lives entirely inside exomind under
keys `exosched:timer:<id>` with a TTL slightly past fire time — no state
on disk, exomind is the only source of truth. Timers survive restarts,
fired timers expire on their own, and every fire grows exomind's
searchable note feed.

Build: `make exosched` produces `exosched/build/exosched`; test with
`make test-exosched` (54 checks, ~2.5 min).

### Usage

```sh
./build/exosched --port 7655 --exomind http://127.0.0.1:7654 [--token secret]
```

`--exomind` defaults to `http://127.0.0.1:7654`, `--port` to 7655.
Set `--token` (or env `EXOSCHED_TOKEN`) to require
`Authorization: Bearer <token>` on every request, including the
WebSocket upgrade.

### Endpoints

| method | path | purpose |
|--------|------|---------|
| GET | `/` | full spec (self-describing) |
| GET | `/ping` | liveness: `pong` |
| POST | `/remind` | schedule a reminder (see below) |
| GET | `/timers` | active timers (`json=1` for JSON) |
| DELETE | `/timer?id=<id>` | cancel a timer: `ok` or `missing` |
| GET | `/ws` | WebSocket push channel (RFC 6455) |

Errors are `error: <reason>` with HTTP 4xx/5xx.

### Scheduling

`POST /remind` takes a plain-text body:

```
in 90s "water the plants"
in 5m "stand up and stretch"
in 2h "push the branch"
in 3d "renew the certificate"
at 1786740704 "fire at this unix epoch"
```

Units: `s m h d`. The message may be quoted (`\"` and `\\` escapes) or
unquoted to the end of the body. The answer is
`ok <id> <when-epoch>` with an id of the form `<epoch>:<8-hex>`.

### Durability (dogfooding exomind)

- Creating a timer writes exomind key `exosched:timer:<id>` with a TTL
  of fire-time + 300s, then answers.
- On startup exosched lists `exosched:timer:*` (one `/batch` of gets),
  reschedules timers still in the future, and logs overdue ones as
  notes (`missed timer <id>: <msg> (was at <epoch>)`) before dropping
  them. If exomind is briefly down at startup, reload is retried every
  second for 10s.
- On fire, exosched pushes the event over WebSocket, writes a note
  `fired timer <id>: <msg> at <epoch>` and deletes the key. Cancel
  deletes the key immediately.
- The TTL is the safety net: even if exosched is down at fire time the
  key expires on its own.

### WebSocket push

`GET /ws` performs the RFC 6455 handshake (SHA-1 + base64 accept key
implemented by hand). The server then pushes one text frame per fired
timer to every connected client:

```
timer <id> <epoch> <message>
```

Clients send nothing; close frames are answered and the socket closed,
dead clients are purged on the next broadcast, pings get pongs.

### Receipts

Add `receipt=1` to any reminder body to request a delivery receipt:

    in 10m "weekly backup" receipt=1

When the timer fires, exomind receives `receipt:<id>` = `fired:<epoch>:<msg>`
(24 h TTL) in addition to the note. Agents can therefore prove a reminder
was actually fired (checking the key exists) instead of trusting a note
that might have been dropped during an exomind outage.

### Design

- One pthread timer loop (condvar, CLOCK_MONOTONIC deadlines, 100ms
  granularity, woken on add/cancel).
- Wall clock only for `at` parsing, `remaining_s` output and exomind
  notes.
- One thread per HTTP/WS connection, like exomind.
- No state on disk: exomind is the only source of truth.

### Limitations

- exosched is a *timer* daemon, not a durable message broker: fired timers
  are pushed over WebSocket and logged to the exomind note feed, but there
  is no replay queue — an agent that was disconnected at fire time must
  catch up via `GET /notes`.
- `at`-style reminders are rejected in the past; there is no timezone
  support (all epochs are Unix time).
- A timer whose fire is retried during an exomind outage is kept and
  retried every 5s — reliable, but a long outage can pile up pending fires.

### Tests

`bash test/test.sh` runs its own private exomind (port 7660, data in
/tmp/exosched_test) and exosched (port 7661); it never touches a shared
exomind. Needs `curl`, `python3` (the WebSocket client) and `ss`.

Coverage includes the 0.2.0 recurring-timer surface: `every` cadence
(measured from note epochs), persistence across SIGKILL restarts, `until`
semantics (stops after the last fire, past `until` rejected, past `at`
rejected), DELETE of a recurring timer, the 6-column `/timers` TSV and
`json=1` `repeat_s`/`until` fields, reload catch-up of overdue recurring
timers, 0.1.0 one-shot wire values (`fire\tmsg`) loading and firing, and
the reload/cancel race: a timer cancelled while a degraded-startup
background reload is in flight is never resurrected by the stale snapshot.

## exoflow — the swarm orchestrator (port 7656)

`exoflow` v0.2.0 is a dependency-graph task orchestrator for AI-agent swarms. A flow
is a DAG of steps; an arbitrary number of agents pull work from it with
`GET /next`, execute, and report back with `POST /step`. exoflow guarantees
that every step is claimed by exactly one worker and only becomes runnable
once all of its dependencies are done. Durable state lives in exomind
(external long-term memory); step deadlines are enforced through exosched
scheduled reminders; every claim, completion and deadline is audited as an
exomind note. Since 0.2.0 a flow can be a **loop**: when its last
iteration reaches a terminal state, exoflow lazily spawns the next one
(`iter <n+1>`), repeating with a fixed interval up to `max` / `until`
limits.

```
exomind (state)   <-+-  exoflow   <-+-- GET /next?flow=F&worker=W   (workers)
exosched (alarms) <-+      |       +-- POST /step?flow=F&id=s done
                      (audit notes into exomind)
```

### Quickstart

Run the trio (each daemon answers plain-text on its port) — shared
instances already run on 7654/7655 in the exomind swarm, so use your own
ports for testing, e.g. 7674/7675/7676:

```
build/exomind --port 7654 --data /tmp/xm.dat &
exosched/build/exosched --port 7655 --exomind http://127.0.0.1:7654 &
exoflow/build/exoflow --port 7676 --exomind http://127.0.0.1:7654 \
    --exosched http://127.0.0.1:7655 &
```

Check it is alive — `GET /` is self-describing: `curl -s localhost:7676/`.

### A full diamond flow, by hand

Create a 5-step diamond where `s4` joins `s2` and `s3`, with a deadline on
`s4` (`now + 120s`):

```
curl -s -X POST localhost:7676/flow --data-binary "ship
s1<TAB>build<TAB>
s2<TAB>test<TAB>s1
s3<TAB>lint<TAB>s1
s4<TAB>package<TAB>s2,s3<TAB>$(date +%s --date '+120s')
s5<TAB>publish<TAB>s4"
# -> ok <flow-id> 5
```

Then drive it with the contrib worker (see below):

```
exoflow/contrib/worker.sh -u http://127.0.0.1:7676 -f <flow-id> -w w1
```

...or claim and finish steps by hand:

```
curl -s "localhost:7676/next?flow=<flow-id>&worker=cli"   # ok s1  (auto-claims)
curl -s -X POST "localhost:7676/step?flow=<flow-id>&id=s1" -d "done cli"
curl -s "localhost:7676/flow?id=<flow-id>"                # TSV state
curl -s localhost:7676/flows                              # all flows
```

### The worker loop (`exoflow/contrib/worker.sh`)

A simulated agent that runs the orchestration loop against any compliant
exoflow: repeatedly `GET /next?flow=F&worker=ME` (which auto-claims one
runnable step), "executes" it (a short sleep; if the step description starts
with `fail:` the step is failed on purpose), then reports back with
`POST /step`. `none` means no runnable work remains and the worker exits 0.

```
worker.sh -u URL -f FLOW -w NAME [-m MAX] [-s SLEEP] [-r PARK] [-e EXOMIND] [-q]
  -u URL      exoflow base URL (required)
  -f FLOW     flow id (required)
  -w NAME     worker name, included in audit notes (required)
  -m MAX      safety cap on claimed steps (default 64)
  -s SLEEP    simulated work seconds per step (default 0.5)
  -r PARK     on `none`, re-poll up to PARK times (default 1 = exit 0
              immediately, per the loop spec). Parking keeps a worker
              available so multi-worker runs genuinely contend for steps
  -e EXOMIND  optional exomind URL; every claim/step is also appended as a
              timestamped note "FLOW|STEP|ACT|WORKER" (| separators —
              exomind escapes control characters, so tabs would not round
              trip) so that ordering can be verified from note timestamps
  -q          quiet mode

exit: 0 = no work left, 1 = protocol/driver error, 2 = step failed
```

Every action is logged on stdout as `worker <name>: claimed s2` /
`worker <name>: done s2` / `worker <name>: no work left`. Run two of them in
parallel against one flow to see claim exclusivity in action — each step is
claimed by exactly one worker (park the workers with `-r` so they contend at
every level of the graph):

```
exoflow/contrib/worker.sh -u http://127.0.0.1:7676 -f <flow-id> -w w1 -r 10 > w1.log &
exoflow/contrib/worker.sh -u http://127.0.0.1:7676 -f <flow-id> -w w2 -r 10 > w2.log &
wait
```

### Architecture

- **State lives in exomind keys.** Every flow and step (status, deps,
  claims, deadlines) is durably stored under `exoflow:*` keys, so the
  daemon survives restarts with no local data file: SIGKILL the exoflow
  process, start it again on the same ports/backend and the flow state is
  intact.
- **Claim timeouts.** A step may carry a 5th column `timeout_s`: the
  claim via `/next` sets its deadline to `now + timeout`, and the lazy
  sweep marks the step `overdue` if the worker does not finish in time;
  `unclaim` resets the clock. A stuck worker therefore releases the step
  deterministically, and the freeze detector (`agent-health` in exoqms)
  catches silent workers on top.
- **Deadlines via exosched.** A step with a `deadline_epoch` registers a
  scheduled reminder with exosched; when it fires (or on the next read, via
  the lazy deadline sweep) exoflow marks the step `overdue` and writes an
  audit note. `GET /flow?id=` reflects the overdue state.
- **Audit via notes.** Claims, step completions/failures, cancellations and
  deadline misses are written as timestamped exomind notes, which gives you
  an append-only ledger you can query with `GET /notes?q=<flow-id>`.
- **Loops are lazy.** A loop spawns its next iteration on the next read
  (`/flows`, `/flow?id=`, `/loops`, `/next`) or startup reload after the
  newest iteration is terminal and `next_run` has arrived — no background
  threads, no clock dependencies, survives SIGKILL. The exosched reminder
  `exoflow:loop:<id>` is feed candy; the lazy check is authoritative.

### Loops

Make any flow a loop by adding an optional LAST line to the POST /flow
body:

```
loop<TAB>every <n><s|m|h><TAB>[max <n>] [until <epoch>]
```

`every 2s` / `every 1m` / `every 3h` set the interval; `max` caps the
number of COUNTED iterations (default unlimited); `until` stops new runs
at/after the given epoch. A line starting with `loop<TAB>every` that does
not parse is rejected with `error: bad loop spec`; any other last line is
a plain step, so old bodies keep working byte for byte.

Scheduling is **lazy**, exactly like deadlines: no background thread, no
timer-driven spawn. On every `GET /flows`, `GET /flow?id=`, `GET /loops`,
`GET /next` and every startup reload, exoflow checks whether the NEWEST
iteration of a loop is terminal (all done / all failed / cancelled) and
its `next_run` has arrived; if so it spawns the next iteration:

- same steps, all pending;
- flow name `iter <n+1>`;
- parent link `parent=<first flow id>` (the first iteration has none);
- `next_run` advanced by the interval on every record of the loop;
- audit note `flow loop <id> -> iter <n+1> at <epoch>`;
- best-effort exosched reminder `exoflow:loop:<id>` at the new next_run
  (feed candy only, like deadline reminders — the lazy check is
  authoritative).

Reaching `max` writes `flow loop <id> finished (max reached)` and zeroes
`next_run`; passing `until` writes `finished (until reached)`. Because
the check is lazy, a loop that became terminal while the daemon was down
resumes from persisted state at the next read — SIGKILL mid-loop is safe.

- `POST /flow?id=<f>&action=stop-loop` halts future iterations of the
  loop (note written, existing records kept).
- `DELETE /flow?id=<f>` on any record of a loop also halts the loop and
  removes that record.
- Cancelling one iteration (`action=cancel`) does NOT count toward `max`:
  the loop continues and a replacement iteration is spawned without
  consuming budget.
- `GET /loops` lists every iteration:
  `loop<TAB><id><TAB>iter <n><TAB>next <epoch><TAB>interval <s>`
  (`json=1` for JSON).

Persistence: format version 2 — the header is `exoflow<TAB>2<TAB>name`
and looping records carry one trailing line
`loop<TAB><interval s><TAB><max><TAB><until><TAB><iter><TAB><budget><TAB><next_run><TAB><parent><TAB><stopped>`.
Version-1 records load as non-looping flows (both formats are read).

### API reference

| method | path                     | body / params            | reply |
|--------|--------------------------|--------------------------|-------|
| GET    | `/`                      | —                        | self-describing text |
| GET    | `/ping`                  | —                        | `pong` |
| POST   | `/flow`                  | line 1 = flow name; then `id<TAB>desc<TAB>deps` lines, deps comma-separated (empty allowed); optional 4th field `deadline_epoch`, optional 5th field `timeout_s`; optional LAST line `loop<TAB>every <n><s|m|h><TAB>[max <n>] [until <epoch>]` | `ok <flow-id> <nsteps>` |
| GET    | `/flow?id=`              | —                        | TSV state (one line per step; `loop` line for loops) |
| GET    | `/flows`                 | —                        | flow list |
| GET    | `/loops`                 | —                        | loop iterations list |
| GET    | `/next?flow=&worker=`    | —                        | `ok <stepid>` (auto-claims) or `none` |
| POST   | `/step?flow=&id=`        | `done [note]` / `failed [note]` / `unclaim` | `ok` |
| POST   | `/flow?id=&action=cancel`| —                        | `ok` |
| POST   | `/flow?id=&action=stop-loop` | —                    | `ok` |
| DELETE | `/flow?id=`              | —                        | `ok` |

Auth: start exoflow with `--token <secret>`; every endpoint then requires
`Authorization: Bearer <secret>` and answers `401` without it.

### Limitations

- exoflow is a *pull* orchestrator: workers must call `GET /next`; there is
  no push of new work to idle workers.
- A step claimed by a worker that dies is released only via an explicit
  `unclaim` (or the deadline sweep marking it `overdue`); there is no
  lease timeout / requeue-on-heartbeat.
- Deadline enforcement is best-effort lazy: the authoritative sweep runs on
  reads (`/flow`, `/next`) and startup reload, so an overdue step is
  reflected at the next read, not at the exact deadline instant.
- Loop scheduling is lazy too: the next iteration spawns at the first read
  after the newest iteration is terminal and `next_run` has arrived (the
  exosched reminder `exoflow:loop:<id>` only surfaces the due moment in
  the note feed). There is no timer-driven push.

### Integration tests

`exoflow/test/test-integration.sh` spawns a private stack — exomind on
**7674**, exosched on **7675**, exoflow on **7676**, temp data under
`/tmp/b2-exoflow-int` — and proves the loop end-to-end:

1. creates the 5-step diamond (`s1 → s2,s3 → s4 → s5`) with a deadline on
   `s4`, runs **two `contrib/worker.sh` workers in parallel**, and asserts
   every step is done, no step was claimed by both workers, and `s4` was
   claimed only after both `s2` and `s3` were done (compared via note
   timestamps in the private exomind);
2. creates a flow whose step deadline is `now+2s`, waits, and asserts
   `/flow` marks it overdue and an audit note exists;
3. SIGKILLs exoflow mid-flow, restarts it on the same ports/backend, and
   asserts the state survived and the flow still completes;
4. restarts exoflow with `--token` and asserts 401 without / success with
   the token;
5. kills all three daemons, removes temp data, prints
   `=== results: N passed, 0 failed ===`.

Run it (builds exoflow first if needed; if `exoflow/` is missing from your
clone it fetches and merges `feat/exoflow` from origin):

```
make test-exoflow     # B1 unit tests + this integration suite
# or, standalone:
timeout 300 bash exoflow/test/test-integration.sh
```

Environment hooks: `EXOFLOW_BIN` (binary path) and `EXOFLOW_ARGS` (extra
daemon flags, e.g. `--token` in production-style runs) override the
defaults. The suite never touches shared swarm instances (7654/7655) and
only uses its own ports.

## exodoc — the documentation auditor (batch, no port)

`exodoc` v0.1.0 is a batch command-line auditor (C11, zero dependencies:
libc only) that checks component documentation against the ISO 9001
§7.5-flavored standard in `exodoc/standard.md` — the stack's quality
gate. It reads the stack manifest `docs/stack.tsv` (one component per line:
`name<TAB>dir<TAB>port<TAB>...`), then for each component verifies that its
`README.md` satisfies the standard's clauses: identification (purpose
heading, version token), required sections (build, run, API, state, tests,
honesty), and — with `--live` — that the documented API and version agree
with the running daemon's self-describing `GET /` spec. It never crashes on
malformed input: documents and manifests are capped, control bytes are
stripped, and unreachable daemons are reported as `SKIP` (never fatal).

### Usage

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

Exit status is 0 for a completed run regardless of failures; the gate is
the report line `=== audit: N pass, M fail (score X%) ===` — integration
wiring greps that line for `0 fail`.

### Checks (implemented from `exodoc/standard.md`)

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

### Design

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

### Tests

The suite runs a live audit against its own fake daemons (PASS/FAIL/SKIP
math), JSON validity, `--out` file writing, down-daemon SKIP behavior,
version mismatch detection via binary and via spec, API mismatch detection,
and garbage-doc robustness (NUL bytes, oversized lines) — `exodoc` must
never crash on them. Needs `curl` and `python3` (the fake daemons);
`bash exodoc/test/test.sh` runs standalone in its own temp dir.

### Limitations

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

### Roadmap

- Iteration 5: the Quality Management component consumes `--json` output
  and turns doc-debt history into trend reports.

## exoqms — the Quality Management System (port 7657)

`exoqms` v0.2.0 is a QMS daemon (C11, zero dependencies: libc + pthread
only) that turns the ISO 9000 family into running code for the exomind
stack. It holds quality objectives, runs ISO 19011 audit programs
against the live stack, records non-conformities (NCs) with a
full corrective-action lifecycle, and publishes every milestone into
exomind's note feed. Its durable state lives entirely inside exomind
under `exoqms:*` keys, so the QMS itself is auditable and survives
restarts like every other layer.

The audit program runs the seven checks defined in
`exoqms/standard.md`, invoking `exodoc` (the documentation auditor),
`exoqms-ui` (the UI quality auditor), `exoqms-code` (the code-safety
analyzer) and `exoqms-svg` (the asset-logic analyzer) as child
processes under a hard 5-second timeout each.

### ISO mapping

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

### Quickstart

Build: `make exoqms` builds `exoqms/build/exoqms` AND
`exoqms/ui/build/exoqms-ui` (or `make qms-modules` for just code + svg).

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

### Usage

```
./build/exoqms [--host <addr>] [--port <n>] [--exomind <url>]
               [--exosched <url>] [--exodoc <path>] [--ui <path>]
               [--code <path>] [--svg <path>] [--repo <dir>]
               [--agents <a,b,c>] [--notes24h <n>] [--token <t>]
```

Defaults: port 7657, exomind `http://127.0.0.1:7654`, exosched
`http://127.0.0.1:7655`, exodoc on `PATH`, repo `.`, agents
`a,b,b1,b2,b3`, notes24h 5. Set `--token` (or env `EXOQMS_TOKEN`) to
require `Authorization: Bearer <token>` on every request. `--ui`
points at the exoqms-ui binary and enables the `ui-audit` check;
`--code` enables `code-safety`; `--svg` enables `asset-logic`;
without a module's binary that check reports `skip`.

### Endpoints

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
| GET | /report | consolidated quality picture |
| GET | /trends | metric trend + verdict |

Add `json=1` to listing endpoints for JSON. Errors are
`error: <reason>` with HTTP 4xx/5xx; request bodies are tab-separated
fields.

### Objectives (ISO 9001 §6.2)

`POST /objectives` body: `title<TAB>metric_key<TAB>target`, with an
optional fourth field `period` (default `iter`). `met` means
`value >= target` for numeric targets, equality for string targets; a
missing metric key yields `no-data`. Answer: `ok <id>`.

### Non-conformities and corrective action (ISO 9001 §8.7, §10.2)

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

### Audit programs (ISO 19011)

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

### The universal project config (iter7, v0.2.0)

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

### The field modules

Three sibling quality-audit engines live under `exoqms/`, each a
zero-dependency C11 batch binary with its own fixtures and test suite:

- [`exoqms/ui`](exoqms/ui/README.md) — the UI quality auditor (7 defect
  classes: emoji icons, overlapping controls, misaligned siblings,
  corner mismatches, missing backgrounds, unstyled sdk-default
  controls, WCAG AA contrast). Permanent fixtures `good.html` (0
  findings) and `bad.html` (12 intentional findings).
- [`exoqms/code`](exoqms/code/README.md) — the code-safety analyzer:
  error-handling defects in C source (unchecked returns of critical
  libc calls, missing error paths, null-deref paths). This is the
  deployment loop's fix step applied to code: the audit program runs
  it against the stack's own source, findings become NCs, and the
  fixes are verified by re-auditing.
- [`exoqms/svg`](exoqms/svg/README.md) — the asset-logic analyzer: generated
  SVG shape rules (tree rule-set: stem, crown, proportions, symmetry,
  degeneracy). Fixtures: `tree-good.svg` (clean) and six deliberately
  broken trees.

The daemon invokes them through the `ui-audit`, `code-safety` and
`asset-logic` checks. Build all of them with `make exoqms` from the
repo root (or `make qms-modules` for just code + svg).

#### exoqms-ui — the UI quality auditor

A zero-dependency C11 static analyzer (ISO honesty: it is an
*approximate* static analysis, not a browser — see below). It reads an
HTML file (or a directory of `.html` files plus their linked `.css`
files) and detects seven classes of UI defects, so the QMS can enforce
visual quality without a human looking at a screen.

```
exoqms-ui <target> [--json] [--no-emoji] [--emoji-allowlist <chars>]
exoqms-ui --help | --version
```

`<target>` is an HTML file, or a directory — a directory audit walks it
recursively for `.html` files and pulls in each page's `<link
rel="stylesheet">` files (relative paths resolved, remote URLs skipped
with a note) and inline `<style>` blocks.

Plain output is one finding per line, then an exact summary line:

```
major emoji-icon html > body > header.topbar > button.cart-btn emoji 🛒 in visible UI text where an icon belongs (use an SVG <use> or <img> icon instead)
=== findings: 12 (10 major) ===
```

`--json` prints a JSON array of findings (one object per line) and no
summary line. Exit codes: `0` no findings, `1` findings, `2` usage/IO
error.

The seven checks:

| id | severity | flags | how |
|----|----------|-------|-----|
| `emoji-icon` | major | emoji characters in the visible text of interactive elements (button/a/label/summary/option) or icon-ish contexts (class/id containing `icon`, `ico`, `btn`) | UTF-8 scan of own text nodes against the emoji codepoint ranges (U+2600–27BF, U+2B00–2BFF, U+1F000–1FAFF, FE0F/20E3); `--no-emoji` disables, `--emoji-allowlist` admits characters |
| `overlap` | major | intersecting bounding boxes of interactive elements that are not nested (button/a/input/select/textarea/label/summary/option) | simplified layout boxes; intersection must exceed 2px in both axes; reports both selectors and the intersection size; geometry checks never fire on elements whose size cannot be determined |
| `misalign` | minor | siblings that should align but don't: same tag + shared class + same width and height, but different left edges (stacked layout) or different top edges (flex-row) | compares computed x (block flow) or y (flex row) of the sibling pair; tolerance 2px |
| `corner-mismatch` | minor | two adjacent siblings sharing an edge where one corner is rounded and the matching corner on the other element is square (rounded corner does not connect to a straight line) | border-radius per corner (shorthand 1-4 values, `%` resolved against min(w,h)); vertical and horizontal adjacency within 1px and 4px of shared edge |
| `background` | major | interactive elements with no background (no affordance) on the page background; background equal to the page background; hardcoded background colors that are neither the theme palette nor the page background | effective background via ancestor walk (background does NOT inherit; alpha composited over the page bg); page bg = body/html bg or white; bordered elements (any side ≥ 1px) are exempt as ghost buttons; palette = resolved `--color*` custom properties; elements using `var()` are exempt from the palette check |
| `sdk-default` | major | interactive elements (button/a/input/select/textarea) with zero CSS rules targeting them anywhere in the stylesheet | selector matching during cascade: element, `.class`, `#id`, `*`, descendant, `>`; pseudo-classes stripped; inline `style=` does not count as a rule |
| `contrast` | major | text vs effective background below WCAG AA: 4.5:1, or 3:1 for large text (font-size ≥ 24px) | sRGB relative luminance (WCAG formula) from hex/rgb/rgba/hsl/named colors; skipped when opacity < 1, background images are present, or any color is unresolvable |

Supported input subset: **HTML** — tags, attributes
(class/id/style/src/href/...), text nodes, nesting, self-closing tags
(`/>` and void elements), comments, doctype, common entity decoding.
Not a full HTML5 spec parser: no implied end-tags, unclosed tags stay
open until the document ends, error recovery is "pop until matching
tag". **CSS** — selectors `element`, `.class`, `#id`, `*`, descendant
and `>` combinators, comma lists; pseudo-classes are stripped (`:hover`
rules still count for `sdk-default`); custom properties on `:root`
resolved via `var()` (with fallbacks); declarations for color,
background(-color/-image), border-radius, width/height, min/max,
margin/padding (all shorthands), position/offsets, display, float,
flex basics (flex-direction/justify-content/align-items/align-self/
flex-wrap/flex-grow/shrink/basis), box-sizing, font-size, line-height,
opacity, visibility, border widths/colors. `@media/@import/@keyframes/
@font-face` blocks and `[attr]`/`+`/`~` selectors are skipped and
counted (a note is printed to stderr).

Layout model limitations — a static approximation of the CSS box model
on a fixed 1024×768 viewport:

- Static block flow only; floats are approximated as block placement;
  `float` wrapping of subsequent text is not modeled.
- Flex is simplified: row/column direction, top-aligned items,
  `justify-content`/`align-items` ignored (except direction); auto
  widths of flex items are estimated from text.
- Inline layout is a one-line flow with wrapping; `inline-block` boxes
  are placed on the line like inline elements.
- No text wrapping/measurement: intrinsic widths are estimated as
  0.5em per ASCII glyph (0.8em for non-ASCII); line-height defaults to
  `normal` (1.4).
- No transforms, no overflow clipping, no z-index, no `position:
  sticky`, no tables (block-approximated), no `calc()`, no `vw/vh`,
  no background images (elements with images are skipped for
  color-dependent checks).
- No margin collapsing between siblings.
- When a dimension cannot be determined at all, the box is marked
  unknown and geometry checks emit **nothing** for that element. When a
  dimension is only estimated (text-sized, percentage, content-derived
  height), the finding reason says "(approx geometry)".
- Default UA styles approximated: body margin 8px, font-size 16px;
  inline vs block display defaults follow common tags (a/span/label
  inline, button/input/img inline-block, everything else block).

False negatives are preferred over false positives: if the model cannot
decide, it stays silent.

Fixtures (permanent QA artifact): `fixtures/bad.html` + `bad.css` —
deliberately defective, contains all seven defect classes: emoji icons
(🛒 ⚙), overlapping buttons (relative `top: -30px`), misaligned
siblings (`margin-left: 30px`), rounded card above square card,
white-on-white CTA, transparent raw button, an off-palette hardcoded
`#ff00ff`, an unstyled `<button>`, low-contrast text. Expected:
**12 findings (10 major)**, every check fires. `fixtures/good.html` +
`good.css` — clean page: SVG `<use>` icons, aligned flex rows and card
grid, consistent corner radii, every interactive element styled from
the `:root` palette, WCAG AA contrast everywhere. Expected: **0
findings**.

Test suite (`make -C exoqms/ui test`, 28 checks): audits both fixtures
with pinned finding counts and the exact summary line, validates
`--json` with python3, exercises `--no-emoji` and `--emoji-allowlist`,
feeds garbage (NUL bytes, unclosed tags, a 10MB single line) and
asserts no crash, checks exit codes 0/1/2, `--version`/`--help`, and
directory mode with per-file headers.

`exoqms-ui` is the batch auditor of the QMS loop: the daemon shells out
to it and the pipeline collects its findings. It writes no state of its
own; findings are plain text or JSON, one per line.

#### exoqms-code — the code-safety analyzer

Multi-language QMS code-safety module (v0.2.0): C/C++ analysis, shell
and python line-based adapters, and a generic text-rule engine — audits
any project in any language. The premise (production experience): when
error paths are missing, a simple var-rename or a dropped `fopen`
result silently corrupts output and costs hours to find. With proper
"if not / else" error branches, the error points at the exact function,
and debugging covers only what leads up to it. This analyzer finds the
places where that structure is missing.

```
exoqms-code <file-or-dir>... [--json] [--ignore <glob>] [--version]
```

Findings, one per line:

```
<severity> <check-id> <file:line:col> <reason>
=== findings: N (M major) ===
```

Exit 0 = clean, 1 = findings, 2 = usage error. `--json` emits a JSON
array `{check,severity,file,line,col,reason}` for the QMS daemon.

Checks:

| id | severity | what it flags |
|----|----------|---------------|
| `missing-error-path` | major | value of an error-returning call (fopen, read, strdup, in-file pointer-returning fn) used later without an intervening if-not/else branch — the var-rename-class bug |
| `unchecked-deref-alloc` | major | malloc/calloc/realloc result dereferenced before any NULL check |
| `unchecked-return` | minor | statement `errfn(...);` with the result dropped (`(void)` disclaims) |
| `uninitialized-use` | minor | local read before assignment (in an expression or condition) |
| `swallowed-error` | minor | `if (errfn() != 0) { }` — failure checked with an empty branch |
| `empty-error-branch` | minor | `if (x != 0) { }` — error-style condition with an empty branch |

Error-function model:

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

Parser scope and honest limitations — lexical analysis, no full parse.
The checker stays silent when a pattern is undeterminable (false
positives are the enemy):

- No control-flow analysis: `p = f(); if (!p) ...; use(p)` is recognized, but
  a use in a sibling branch of an `if/else` is reported conservatively (the
  `util.c:363` style finding is a known single false positive).
- No cross-file analysis: typedefs from included headers are not collected
  (header typedefs in pointer contexts are treated as types by heuristic).
- No macro expansion; preprocessor lines are skipped.
- `(void)` casts, `free()`, `sizeof x`, `&x` output params, and
  `x && use(x)` / `x ? ... : ...` truthiness guards are all respected.

Test suite (`make -C exoqms/code test`, 24 checks): `fixtures/bad.c`
fires every check id with pinned counts, `fixtures/good.c` is clean,
JSON validity, `--ignore`, directory mode, NUL-byte / 10 MB inputs
never crash.

Real-run results (iteration 6, the stack's own C code): after
calibration, over all 7 components (~20k lines C) — **majors: 99 -> 1**
(the documented branch-blindness false positive); minors: 312 -> 101,
all `unchecked-return` on `close()`/`fclose()`/`setsockopt()`/
`clock_gettime()` in error paths and cleanup (deliberate drops; the
error is already in flight — triage items, not defects); one real
defect found and fixed in the module's own tokenizer (fread short-read
silently truncated; now returns an error); the module's own source is
self-audit clean.

Notes: `@nonnull` annotations are also a documentation contract —
`exodoc`-style readers see at a glance which functions cannot return
NULL. Severity rule for the QMS daemon's `code-safety` check: **pass
iff 0 major**; minor findings are reported but non-fatal (see
`exoqms/standard.md` §5.3 c6).

#### exoqms-svg — the asset-logic analyzer

A zero-dependency C11 static analyzer (ISO honesty: an *approximate*
geometry analysis, not a renderer — see below). It reads an SVG file
(or a directory of `.svg` files) and checks the **logic** of the shapes
with geometry, not taste: "an AI made tree SVG, unless it's as simple
as a tapered stem and a round crown, is almost certainly anything but
an actual tree." Generated graphics look plausible but are structurally
wrong; this module catches that. v0.1.0 ships the **tree** rule-set;
shape kinds are pluggable rule-sets behind the same `audit_run` entry
point.

```
exoqms-svg <target> [--shape tree|auto] [--json]
exoqms-svg --help | --version
```

`<target>` is one `.svg` file, or a directory — a directory audit walks
it recursively for `.svg` files. `--shape` selects the rule-set:
`tree` forces it, `auto` (the default) detects it from the root `<svg>`
`data-shape` attribute (e.g. `<svg data-shape="tree">`) or, failing
that, from a filename containing "tree" (case-insensitive). If `auto`
finds no hint the file is **skipped**: plain mode prints one line
`skip unknown-shape <path>: no tree hint (no data-shape="tree" on root
<svg>, filename has no "tree")`, it does not count as a finding and the
exit code stays 0. This is the documented behavior for `house.svg` in
the fixtures: with `--shape auto` it is skipped; with `--shape tree`
the tree rules run anyway and flag it (2 major + 1 minor, pinned in the
tests).

Plain output is one finding per line — `<severity> <check-id>
<reason>` — then an exact summary line:

```
minor symmetry crown leans 4830/12841 (left/right area about trunk axis x=156.0, balance ratio 0.38, want ≥ 0.6)
=== findings: 1 (0 major) ===
```

`--json` prints a JSON array of finding objects (`file`, `shape`,
`severity`, `check`, `reason`; one object per line) and no summary
line. Exit codes: `0` no findings, `1` findings, `2` usage/IO error.
Skipped files never produce findings in `--json` mode.

The tree rule-set (one line per check, with the math):

| id | severity | flags | how |
|----|----------|-------|-----|
| `stem-taper` | major | trunk is a parallel-sided box (ratio > 0.9) or a spike (< 0.15) | trunk = the elements whose bbox center lies below the crown split; stem width = the horizontal extent of the trunk segments crossing a horizontal line (cross-section), measured at 10% (top slice) and 90% (bottom slice) of the trunk bbox height; a tapered stem is narrower at top: `ratio = width_top / width_bottom` must lie in [0.15, 0.9] |
| `stem-missing` | major | nothing below the crown region at all | trunk element count == 0; reported even on degenerate input |
| `crown-roundness` | major | crown bbox aspect outside [0.75, 1.5]; or convexity < 0.8; or a box-shaped crown | crown = elements in the upper region (see region split); aspect = `crown_w / crown_h`; convexity = `union area / convex hull area`, hull by monotone chain (Andrew) over all sampled crown points, areas by shoelace, clamped to 1.0; union area = sum of the crown element areas (overlaps counted twice — see Limitations). A single crown element filling ≥ 85% of its own bbox is a box-shaped crown (a square has convexity 1.0, so convexity alone cannot flag it — documented extension) |
| `proportions` | minor | `trunk_h / crown_h` outside [0.15, 0.6], or `crown_w / total_h` outside [0.4, 1.6] | measured on the trunk/crown/total bounding boxes |
| `symmetry` | minor | crown left/right area balance ratio < 0.6 about the trunk axis | axis = trunk bbox center x; each crown element's area is split left/right by the fraction of its bbox on each side of the axis; `ratio = min(left,right) / max(left,right)`; skipped when the stem is missing |
| `empty-shape` | major | total painted area < 0.5% of the total bbox area (a single line, a stick, an empty `<svg>`) | degenerate input: the crown/trunk shape checks are then skipped (geometry is meaningless); `stem-missing` is still reported |
| `fragmented` | minor | more than 8 disconnected stroke groups | connected components (union-find) over path/polygon/polyline/line elements, two elements linked when any two of their points are within 2% of the bbox diagonal; computed on ≤ 4000 points (beyond that the check stays silent) |
| `out-of-bounds` | minor | element entirely outside the svg `viewBox` | element bbox vs `viewBox x y w h`; no viewBox → check skipped |

**Region split heuristic (documented):** crown = the top 65% of the
total bbox height; an element belongs to the crown when its bbox center
lies at or above the split line, otherwise it is trunk material. When
the resulting crown bbox extends below the top 70% of the total height
(the 65% line cut through the canopy), trunk elements that still sit
inside the crown's horizontal span and do not extend below the crown
bottom are reclassified as crown.

API (CLI surface):

| method | signature | purpose |
|--------|-----------|---------|
| batch | `exoqms-svg <file.svg>` | audit one file, tree rule-set via auto-detection |
| batch | `exoqms-svg <dir>` | audit every `.svg` under the directory, per-file headers + totals |
| batch | `--shape tree` | force the tree rule-set |
| batch | `--shape auto` | detect via `data-shape` attribute or filename (default) |
| batch | `--json` | JSON array output instead of plain text |
| batch | `--version` / `--help` | version / usage |

The audit engine is a library of one entry point, `audit_run()` (see
`src/svg.h`): shape kinds are pluggable rule-sets behind it, so a new
shape kind (e.g. `house`) is a new detection string + one rule function,
not a CLI change. The module is a batch auditor; it writes no state of
its own — findings are plain text or JSON, one finding per line, ready
for the QMS daemon and pipeline to consume like `exoqms-ui`'s.

Design: five files, no dependencies beyond libc + `-lm`: `main.c`
(CLI), `util.c` (memory/strings/file/dir walk), `svgparse.c` (the SVG
subset parser: XML-lite tokenizer + path-data sampler + affine
transforms), `geom.c` (shoelace, monotone-chain convex hull) and
`checks.c` (the tree rule-set + shape detection). Elements are sampled
to world-coord points/segments once at parse time; every check runs on
those points — no second parser, no state between audits. Sampling:
`C/S` → 8 points, `Q/T` → 4, `A` → 8 (center parameterization),
circles/ellipses → 16 perimeter points (their bbox corners are included
in the samples), `rect` → 4 corners, `polygon`/`polyline`/`line` →
their vertices. Paths are treated as filled: open subpaths are closed
implicitly for area and cross-section, matching SVG fill semantics.

Supported input subset (honest): **Elements** — `svg`, `g` (nested,
flattened), `path`, `circle`, `ellipse`, `rect`, `line`, `polygon`,
`polyline`. **Attributes** — `id`, `d`, `cx/cy/r`, `rx/ry`, `x/y`,
`x1/y1/x2/y2`, `width/height`, `points`, `transform`, `viewBox`,
`data-shape`. **Transforms** — `translate`, `scale` and `rotate` (with
optional center) are composed and applied; `matrix`, `skewX`, `skewY`
are **skipped with a note** on stderr. **Path commands** — `M/m L/l
H/h V/v C/c S/s Q/q T/t A/a Z/z` with implicit repetition; relative
coordinates supported; exponents (`1e2`) supported. **Not supported** —
`<use>`, `<defs>` contents (their subtrees are ignored entirely — they
are not rendered), `<text>`, gradients/markers/clips,
`style`/`fill`/`stroke` (colors are ignored; `fill="none"` is not
honored — all paths are treated as filled), rounded-rect `rx/ry`,
viewBox `preserveAspectRatio`. `garbage/no-d` input skips the element
with no finding; files are truncated at 16 MiB, element/point counts
are capped (20000 elements, 200000 points per element) so pathological
input never exhausts memory.

Layout model limitations (honest list):

- Union area = **sum of element areas**: overlapping crown elements are
  double-counted (convexity is clamped to 1.0, so this only errs toward
  "round enough"). The convexity check applies to the union only when
  the crown has ≥ 3 sampled points; with fewer, crown geometry checks
  stay silent.
- Convexity uses the sampled boundary (16-gon ≈ circle), so a disc
  measures ≈ 1.0; a 4-corner-only approximation would measure π/4 and
  falsely fail — documented choice.
- Stem width is a segment cross-section of the sampled boundary, so
  curves are measured via their samples (chords, slightly narrower than
  the true arc).
- Crown/trunk classification is by bbox center against the 65% split:
  foliage scatter far below the canopy midline can be counted as trunk
  material (see the region-split heuristic above).
- The taper check needs the trunk to be at least 1px tall and to have
  measurable width at both slices; otherwise it stays silent.
- Out-of-bounds flags only elements entirely outside the viewBox;
  partial overflows are common and not flagged.
- False negatives are preferred over false positives: when the model
  cannot decide, it stays silent.

Fixtures (permanent QA artifact):

| fixture | geometry | expected findings |
|---------|----------|-------------------|
| `tree-good.svg` | tapered stem path (12px top / 32px bottom, 95px tall) + round crown (r=85 circle + 2 symmetric foliage dots) | **0 findings** — the standard every generated tree must meet |
| `tree-stick.svg` | single vertical line | `empty-shape` major + `stem-missing` major |
| `tree-square-crown.svg` | square rect crown over a tapered stem | `crown-roundness` major only |
| `tree-box-stem.svg` | round crown over a 20px-wide parallel-sided trunk | `stem-taper` major only |
| `tree-too-lean.svg` | huge crown (r=90) on a 22px trunk stub | `proportions` minor only |
| `tree-asym.svg` | crown shifted right of the trunk axis (center x=190 vs axis x=156) | `symmetry` minor only |
| `house.svg` | rect body + triangle roof, no tree hint | `--shape auto`: skipped (exit 0); `--shape tree`: `stem-taper` + `crown-roundness` major, `proportions` minor |

Test suite (`make -C exoqms/svg test`, 54 checks): pins the finding
counts and severities of every fixture, the exact summary line and exit
codes, validates `--json` with python3 (fields and ids), exercises
`--shape auto` skipping (filename, `data-shape` attribute, unknown
hint), transforms (translate/scale keep 0 findings; translate+rotate
doesn't crash), the strange-geometry extras (out-of-bounds, fragmented,
empty-shape), feeds garbage (NUL bytes, garbage `d` data, unclosed
tags, a 10MB single line, an `r`-less circle) and asserts no crash,
checks `--version`/`--help`, unknown options, and directory mode with
per-file headers and the exact summary.

`exoqms-svg` is the asset-logic auditor of the QMS loop: like
`exoqms-ui` it is a batch tool the exoqms daemon shells out to; its
findings are one per line in the same `severity check-id reason`
vocabulary, so the pipeline can collect them identically. It writes no
state of its own.

### Durability (dogfooding exomind)

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

### Tests

`make test-exoqms` from the repo root runs the exoqms suite + module
suites. `make -C exoqms test` runs the daemon's suite
(`exoqms/test/test.sh`, ~60s — too slow for the 5s audit budget, so the
stack manifest declares `./build/exoqms --version` as the in-budget
smoke command instead and `make test-exoqms` remains the full-suite
gate); the module suites are `make -C exoqms/ui test` (28 checks),
`make -C exoqms/code test` and `make -C exoqms/svg test` (54 checks).

### Limitations

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

## exocrawl — the research daemon (port 7658)

**AI-native web research daemon — token-efficient, private, concurrent.**

The web is built for humans: HTML boilerplate, ads, cookie banners, and
search engines that optimize for sponsored content. exocrawl reduces the
web to what an AI needs — clean plain text, links, and images — with a
SearXNG-style private metasearch layer (no accounts, no cookies, no
tracking) and high-concurrency fetching with per-host pacing and identity
rotation.

### Why it exists

- **Token efficiency** — `/fetch` strips nav/ads/footers/cookie banners;
  headings become `# `, lists `- `, code stays verbatim. A 50 KB HTML page
  becomes a few hundred tokens.
- **Independent private search** — five engines fetched directly and parsed
  by our own adapters: DuckDuckGo HTML, Mojeek, Marginalia, Bing, and the
  Wikipedia opensearch API. No third-party aggregator (no SearXNG), no API
  keys, no accounts, no cookies; sponsored results filtered per engine.
- **Broad scraping** — `/scrape` fetches many URLs concurrently (worker
  pool) with per-host pacing (default 150 ms) and rotating identities;
  403/429 gets bounded retries with a different identity.
- **200% privacy** — stateless requests, no JS, no referrer, no persistent
  state unless you opt into the exomind cache (`--cache exomind`, keys
  `exocrawl:cache:*`, 24 h TTL).
- **TLS via curl** — the one runtime dependency: the ubiquitous `curl`
  binary provides HTTPS transport; everything else is native C.

### Build & run

```sh
make exocrawl          # exocrawl v0.1.0/build/exocrawl (zero compile deps)
make test-exocrawl     # hermetic suite (local mock web, 26 checks)
./exocrawl/build/exocrawl --port 7658 --cache exomind
```

Flags: `--port` (7658), `--token`, `--concurrency` (16), `--pace-ms`
(150), `--cache exomind`, `--proxy http://...`.

### API

Self-describing: `GET /` prints the full spec (the `/` endpoint itself
is the documentation; this file is the human view).

| method | path | purpose |
|--------|------|---------|
| GET | `/search?q=...&n=10&engines=ddg,mojeek,marginalia,bing,wikipedia\|all&json=1` | independent metasearch → `rank<TAB>title<TAB>url<TAB>snippet` |
| GET | `/fetch?url=...&max=8000&links=1&images=1` | HTML → clean plain text (+ `## links` / `## images` sections) |
| POST | `/scrape` | one URL per line `[TAB max]`, concurrent fetch-all |
| GET | `/stats` | counters (fetches, errors, cache_hits, bytes) |
| GET | `/ping` | `pong` |

All endpoints answer plain text, lowercase `ok`/`error:` style; `--token`
enforces Bearer auth.

### Extraction rules

- Skipped: `script/style/noscript/svg/iframe/form/nav/footer/aside/...` by
  tag; `nav`, `menu`, `sidebar`, `ads`, `advert`, `sponsor`, `cookie`,
  `banner`, `popup`, `modal`, `comment`, `share`, `social`, `related`,
  `recommend`, `subscribe`, `newsletter`, `promo` by class/id.
- HTML entities decoded (named + numeric, incl. UTF-8 output).
- Relative URLs resolved against the page URL.
- Limits: 200 links / 100 images per page; `max` caps the whole output.

### Internals

- **Transport** — the curl binary provides TLS; everything else is
  native C: request building, UA rotation, bounded retries, per-host
  pacing (a worker pool fans out `/scrape`), HTML→text extraction, and
  the per-engine parsers.
- **Cache** — with `--cache exomind`, extracted text is stored under
  `exocrawl:cache:*` keys (24 h TTL) and served on repeat fetches.
- **Identity** — stateless requests with rotating user agents; no
  cookies, no referrer, no JS.

### Honest limitations

- No JavaScript execution — single-page apps yield their static content only.
- No browser-grade CSS/layout: extraction is content-order based.
- Engines can be rate-limited or captcha-gated (Bing especially); UA
  rotation and bounded retries mitigate this, and the engine list is
  configurable (edit the table in `src/search.c`).
- `robots.txt` is not consulted by default (research mode); rate limits and
  pacing are the politeness mechanism.

### Tests

`make test-exocrawl` — hermetic: local mock web serves fixtures for all
five engines plus a test page; asserts extraction (boilerplate removal,
headings, links, images, entities, pre verbatim), every engine's parser,
ad filtering, redirect decoding, /fetch caps, /scrape concurrency, 403
retry, stats counters, auth. ASAN-clean.

## exocontext — context continuity (port 7659)

A tiny daemon that compresses an agent's durable state into a bounded,
recency-ranked digest: everything under `agent:<id>:*` plus the notes
mentioning the agent, capped at a character budget. An agent that
restarts (or opens a fresh context window) reconstructs its working
state from a single `GET /context?agent=<id>` — no more re-reading a
hundred keys by hand.

Build: `make` produces `exocontext/build/exocontext`; `make test` runs
24 hermetic tests (it spins its own exomind). Zero dependencies: C11,
POSIX, threads. The only backend is exomind.

### Run

```sh
./exocontext/build/exocontext --port 7659 \
    --exomind http://127.0.0.1:7654 &
```

`--token <secret>` enables Bearer auth. `GET /` prints the full spec.

### API

Plain text, lowercase answers. Every endpoint is self-describing.

| method | path | purpose |
|--------|------|---------|
| GET | `/` | spec |
| GET | `/ping` | liveness: `pong` |
| GET | `/context?agent=<id>[&budget=<n>]` | the digest |
| POST | `/context` | same, body `agent=<id>&budget=<n>` |

```sh
# reconstruct an agent's working state (notes + keys, newest first)
curl 'localhost:7659/context?agent=b2&budget=2000'
# => # context for b2 (budget 2000 chars)
#    ## notes (newest first)
#    note:1786899721384:0000fdad  agent:b2 milestone: ...
#    ## state (agent:<id>:* keys)
#    agent:b2:plan  1) fix doc gate 2) commit 3) exocontext done
```

`budget` is capped at 256 KB; values inside the digest are truncated to
400 chars each. `json=1` is accepted and ignored by design (token
efficiency: the plain form is the canonical one).

### Internals

- **Composition** — one exomind round trip for notes (`/notes?q=agent:…`,
  already newest-first), one for keys (`/list?prefix=agent:<id>:`), one
  batch read for values (`/batch`-style multi-get). A seen-set dedupes
  entries across the two sections.
- **Budget** — lines are emitted until the budget is consumed; the
  crossing line is kept, then emission stops. Values are cut at 400
  chars so a single runaway key cannot eat the whole budget.
- **Server** — thread-per-connection, single read loop honoring
  Content-Length, bearer auth checked before routing.
- **No state** — the digest is computed from exomind on every request;
  exocontext is stateless and can be restarted freely.

### Tests

`make test` covers digest composition (notes + state keys, other agents
excluded), budget capping, POST bodies, error paths, and bearer auth.

### Limitations

- Recency for keys is approximated by `sort=desc` on the key name
  (timestamps are not in `/list`), so `agent:<id>:z*` keys may sort
  before newer `agent:<id>:a*` keys; notes are genuinely newest-first.
- The digest is a flat dump, not a summary: compression is by cap and
  truncation, not by meaning.
- One agent namespace per request; cross-agent digests are out of scope.

## exokit — the behavioral development kit (batch, no port)

Every software carries its own development kit: a `kit/` directory that
is the software's SDK-for-itself. Not a library, not a framework — a
tool + rules set that makes any development process (in any language, in
any combination) scaffold on a **behavioral contract** instead of on the
code.

The reason: standards (RFC 2119, PEP 8, …) limit implementation issues,
but not *logical* ones. A port (Rust+SvelteKit → C++ + vanilla JS/CSS)
can compile cleanly and still be a different program. exokit attacks
that at the root: the **contract is the truth, not the code**.

Build: `make` produces `exokit/build/exokit`; `make test` runs 39
hermetic tests. Zero dependencies: C11, POSIX, no daemon, no server
(batch tool).

### Run

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

### API (the kit files)

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

### Internals

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

### Rules (the development kit part)

- R1 contract-first: no public function without an inventory entry.
- R2 every contract entry has ≥ 1 example, including an edge/error case.
- R3 translate by regenerating from the contract, never line-by-line.
- R4 deliver in inventory slices, each slice verified before the next.
- R5 both implementations must pass the same examples ledger.
- R6 the ledger is the only source of truth for behavior.

### Tests

`make test` covers scaffolding, extraction (multi-line, static, entry
points, kit self-exclusion), verification incl. intentional-drift
detection, `--fn`/`--runner` flags, audit completeness + fidelity
findings, and diff semantics (missing / extra / `--exact`).

### Limitations

- `extract` handles plain C/C++ shapes only; templates, macros and
  other languages need manual inventories (documented trade-off: the
  contract is a curated decision).
- `verify` spawns one process per example: large ledgers are slow
  (audits cap at 50 examples by default).
- No JSON arg encoding: args/expected are literal text without tabs or
  newlines (the shims' dispatch parses them).

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
