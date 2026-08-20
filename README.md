# exomind v0.4.0-alpha.1 — the AI-native software stack

A software stack designed for, built by, and used by AI agents — with
humans welcome as reviewers and operators. Eight components, zero
dependencies, plain-text machine-first APIs, and a development loop that
uses the stack to build the stack: every component is developed while the
others run, audited by the quality system, and dogfooded in the process.

The interface is **console operations**, not flags or daemons by default:

```sh
exomind /set?key=project:alpha:deadline --body '2026-09-01'
exosched /remind --body 'in 90s "water the plants"'
exoflow /next?flow=<id>&worker=w1
exoqms /report
exocrawl /search?q=bitcask&n=5
```

Each `exo<module> <path-and-query>` runs ONE API operation in-process and
exits. No module binds a port unless it is explicitly told to serve.

**License: GPL-3.0-only** (see [LICENSE](LICENSE)).
**Status: Early Alpha** — the first packaged release is `v0.4.0-alpha.1`
(pacman + apt packages, see [Releasing](packaging/release.sh)).

## The stack

| component | role | server-mode port | README |
|-----------|------|------------------|--------|
| **exomind** | durable long-term memory: key/value, notes, ranked search, TTLs, snapshots, scoped tokens, vector recall | 7654 | this file |
| **exosched** | the alarm clock: scheduled reminders (`in 90s` / `at` / `every`), WebSocket push, state persisted in exomind | 7655 | [exosched/](exosched/README.md) |
| **exoflow** | swarm orchestrator: dependency-graph flows, claims, deadlines, claim timeouts, self-looping flows | 7656 | [exoflow/](exoflow/README.md) |
| **exoqms** | universal Quality Management System: objectives, NCs, audit programs, trends, field modules for any language | 7657 | [exoqms/](exoqms/README.md) |
| **exocrawl** | AI-native research: independent private metasearch, token-efficient HTML→text extraction, concurrent scraping | 7658 | this file |
| **exocontext** | context continuity: bounded recency-ranked digest of an agent's state and notes | 7659 | this file |
| **exodoc** | documentation auditor: ISO 9001 §7.5-flavored standard, live API conformance — **batch** (no port) | — | this file |
| **exokit** | behavioral development kit: every software carries its own contract + ledger + runner shims; translate by regenerating from the contract — **batch** (no port) | — | this file |

Four modules are **batch**: they run one operation and exit — `exodoc`,
`exokit`, and the two QMS field analyzers plus their sibling `exoqms-ui`
(three batch helpers under `exoqms/`: `exoqms-code`, `exoqms-ui`,
`exoqms-svg`, see the exoqms section).

All components: C11, POSIX, zero compile dependencies (TLS in exocrawl
uses the ubiquitous `curl` binary). Every API is plain text and
self-describing (`GET /` prints the full spec). The "server-mode port"
column is the port a module binds **when told to serve** (`--serve` or an
explicit `--port`); nothing binds by default. Ports are metadata
(`docs/stack.tsv`), not a discovery mechanism: services reach each other
by **name** — fork/exec of sibling binaries — never by probing HTTP
ports.

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

`make install` lays down each module as `exo<name>` (console binary) plus
an `<name>-server` symlink. The `exomind-server` symlink is the stack-wide
MCP server (see below); sibling `-server` symlinks exist for naming
parity.

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

Every module is a console binary first and a daemon only on demand:

```sh
# 1) from the console: one operation, one process, no port bound
exo<module> /op?key=value... [--body <text>]

exomind /set?key=greeting --body 'hello world'      # store a value
exosched /remind --body 'in 5m "stand up"'          # schedule a reminder
exoflow /flows                                      # list flows
exoqms /report                                      # quality picture
exocrawl /search?q=bitcask                          # metasearch
exocontext /context?agent=b2                        # agent digest
exodoc /audit?live=1                                # doc gate
exokit /verify?kit=proj/kit                         # run the ledger

# 2) as a server, when you actually want one (opt-in):
exomind --serve --port 7654 --data exomind.dat --mandate "read memory first"
exosched --serve --exomind http://127.0.0.1:7654

# 3) as an MCP server (for AI agents), stdio JSON-RPC:
exomind --mcp                   # single module: exomind's own tools
exomind-server                  # the whole stack as one MCP server
```

### The console contract

The first argument is a **path-and-query**, exactly like a request line
without the method:

```
exo<module> /operation?key=value&flag=1 [--body <text>]
```

- A path starting with `/` runs that one API operation in-process, prints
  the response body on stdout, and exits. No socket is ever opened.
- The HTTP verb is implied by the operation (`/set`, `/remind`, `/step`,
  `/scrape`, `/nc`, `/objectives`, `/audit` are writes; `/get`, `/list`,
  `/timers`, `/flows`, `/report` are reads; `/del`, `/timer`, `/flow`
  accept DELETE).
- **Bodies**: `--body <text>` supplies the payload; otherwise it is read
  from stdin — but only for non-GET operations AND only when stdin is not
  a terminal (a TTY would block forever waiting for EOF):

```sh
exomind /set?key=greeting --body 'hello world'
echo 'hello world' | exomind /set?key=greeting     # same thing
exosched /remind --body 'in 90s "water the plants"'
exoflow /step?flow=F&id=s1 --body 'done cli'
printf 'url1\nurl2\n' | exocrawl /scrape
```

- **The guide**: running `exo<module>` with NO arguments prints the
  module's full spec (the same text the daemon serves at `GET /`) and
  exits 0. The software describes itself; `GET /` on a serving module
  does the same.
- **Exit codes** (the stack-wide contract):
  - `0` — the operation succeeded
  - `1` — the operation failed (an API error, HTTP >= 400)
  - `2` — usage error or unknown operation
- Every module also accepts the `/exo<module>` prefix on paths
  (`/exoexomind/ping`, `/exoexosched/timers`), matching the server-mode
  mount points.

### Server mode is opt-in

No module binds a port unless you ask for it:

- `--serve` runs the HTTP daemon.
- An explicit `--port <n>` implies server mode too.
- Otherwise the binary is a console op or a guide printer. exomind listens
  on 7654 by default **in server mode only**; the other modules' defaults
  are the ports in the stack table above.

```sh
exomind --serve                       # HTTP on 127.0.0.1:7654
exosched --serve --port 7675          # HTTP on 127.0.0.1:7675
exoflow --serve --exomind http://127.0.0.1:7654 \
        --exosched http://127.0.0.1:7655
```

While serving, each daemon answers at both `/` and `/exo<module>` on the
bound address, plain text, one record per line, lowercase `ok` /
`error: <reason>` — the same contract as the console ops, so anything you
can do over the socket you can do as a one-shot op.

### Whole-stack help

```sh
exomind --help modules    # every module's guide, titled by module name
exomind --help exosched   # one module's guide
```

### Shared options

`--body <text>`, `--serve`, `--port <n>`, `--token <t>` (or env
`EXO<MODULE>_TOKEN`), `--keys <file>`, `--rate-limit <n>/s` (429 when
exhausted), `--log-level error|warn|info|debug`, `--help [modules]`,
`--version`, `--update` (fetch + rebuild + reinstall, see the
self-update section). `--host <addr>` (default 127.0.0.1) exists on the
modules that support a bind address. Tokens require
`Authorization: Bearer <token>` on every request.

### keys (auth) file management

exomind manages the shared key file:

```sh
exomind keys add alpha:ro:scope=logs/* --keys ~/.config/exo/keys
exomind keys list --keys ~/.config/exo/keys
exomind keys remove alpha --keys ~/.config/exo/keys
```

Key-file lines are `name` or `name:ro` or `name:ro:scope=prefix/*` — the
same format `--tokens <file>` loads.

### Service discovery is by name, not by port

Daemons and tools never probe each other's ports. When exomind-server
needs exosched, it looks up the **`exosched` binary on PATH** and runs it
as a one-shot console op. When exoqms runs its audit checks it shells out
to `exodoc`, `exoqms-ui`, `exoqms-code`, `exoqms-svg` by name. The stack
manifest `docs/stack.tsv` carries ports as metadata (packaging, doc gate)
— nothing binds them unless a server is started explicitly.

### MCP: the Model Context Protocol server

- `exo<module> --mcp` serves **that module's** tools as an MCP server over
  stdio (`initialize` / `tools/list` / `tools/call`, protocol version
  `2024-11-05`).
- `exomind-server` — exomind invoked with an argv[0] ending in `-server`
  (the installed symlink does this; `exec -a exomind-server ./build/exomind`
  also works) — is the **stack-wide MCP router**: it registers ONE tool
  per sibling module plus exomind's own thirteen, and routes each
  `tools/call` for a sibling through the sibling's console contract
  (`<binary> <path> [--body <body>]`, fork/exec, stdout captured). The
  sibling daemons do NOT need to be running: a routed call runs the
  operation in the sibling process.
- Sibling tools take `path` (the console op, e.g. `"/timers"`) and an
  optional `body`. Exomind's own tools take their natural arguments
  (`key`, `value`, `q`, ...).

Own tools (in-process): `ping`, `set`, `get`, `append`, `del`, `list`,
`search`, `note`, `notes`, `batch`, `stats`, `snapshot`, `sim`.

Routed tools: `exosched`, `exoflow`, `exoqms`, `exocrawl`,
`exocontext`, `exodoc`, `exokit` — one per sibling module.

```sh
# a single-module MCP server (exomind's tools only)
exomind --mcp

# the stack-wide MCP router
exomind-server
```

### Self-update (`--update`)

`exomind-server --update` (works on plain `exomind` too) fetches the
remote, pulls the newest commits, rebuilds and reinstalls the whole
stack into the prefix the running binary lives in — **no sudo needed
for user-local installs** (`~/.local/bin` → `~/.local`). The update is
a human-only, terminal action and is deliberately invisible to agents:

```sh
exomind-server --update

# exomind: updated e35b77d -> 4463ea1 (2 commits, v0.4.0-alpha.1)
# exomind: binaries installed into /home/you/.local/bin
# exomind: restart 'exomind-server' to apply (hint: systemctl --user
#           restart exo-exomind)
```

Behavior, in order: fetch (30s timeout) → compare `HEAD` vs
`origin/<branch>` → if equal: `up to date`, exit 0 → pull
(`--ff-only`; dirty working trees abort with a hint) → `make` (build
errors abort) → `make install` into the derived prefix (non-writable
prefixes abort with `EXO_UPDATE_PREFIX` hint). It never restarts the
daemon itself; the hint does a `systemctl --user restart` .

**Startup version notice (humans only):** every time exomind-server
(or `exomind --serve`) starts, it quietly checks the remote and —
only when updates exist — prints a notice to **stderr**:

```
exomind-server v0.4.0-alpha.1: update available (git e35b77d -> 4463ea1, 2 commits behind origin/main)
  run 'exomind-server --update' to fetch, rebuild and reinstall into /home/you/.local/bin
  (EXO_UPDATE_CHECK=0 silences this notice)
```

The check is bounded (`timeout 8`), fails silent on any error, and
never touches **stdout**: agent channels (console-op replies, MCP
JSON-RPC, HTTP bodies) stay byte-clean, so agents never see the notice
and cannot be nudged into updating mid-work. Notes:

- The check needs a source tree + git remote. The build default is the
  directory the binary was compiled in
  (`EXO_REPO_DIR_DEFAULT`); a distro binary without it stays silent.
- `EXO_UPDATE_CHECK=0` (or `off`/`no`) disables the notice.
- `EXO_UPDATE_DIR=/path/to/exomind` overrides the source tree.
- `EXO_UPDATE_PREFIX=~/.local` overrides the install prefix (default:
  derived from the binary's own location).
- `EXO_UPDATE_BRANCH=main` overrides the tracked branch (default: the
  working tree's current branch).

### MCP over stdio, concretely

```sh
$ printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
    | exomind-server
{"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05","capabilities":{"tools":{}},"serverInfo":{"name":"exomind-server","version":"0.4.0-alpha.1"}}}
```

`tools/list` returns the 20 tools (13 own + 7 siblings). A call to an
own tool:

```sh
$ printf '%s\n' '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"set","arguments":{"key":"greeting","value":"hello"}}}' \
    | exomind-server
{"jsonrpc":"2.0","id":2,"result":{"content":[{"type":"text","text":"ok"}],"isError":false}}
```

A call routed to a sibling — no exosched daemon needed, the console op
runs in a forked process:

```sh
$ printf '%s\n' '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"exosched","arguments":{"path":"/remind","body":"in 5m \"stand up\""}}}' \
    | exomind-server
{"jsonrpc":"2.0","id":3,"result":{"content":[{"type":"text","text":"ok 1786899721384:0000fdad 1786899724384"}],"isError":false}}
```

Failures come back as `isError: true` with the sibling's stderr/exit
status mapped onto the operation result; the router never binds a port
and never keeps state.

## Memory model (exomind)

Two stores instead of one pile:

- **Main memory file** (`--data`, default `exomind.dat`) — general and
  important facts that are NOT project specific.
- **Project memory file** — lives in the agent's project root (auto-
  detected upward from the CWD via a `.git`/`.exo` marker, or
  `--project-root`), at `<root>/.exo/project.dat`, even when exomind
  runs from elsewhere. Keys with the `p:` prefix and any request with
  `proj=1` operate on it; `/search` and `/recall` cover both stores.

**Backups** (`--backup <dir>`, or `/backup` as a console op): timestamped
copies of the main memory file, newest 24 kept — redundancy against a
failing host.

**Associations** — memories are never silently deleted: `/outdate` keeps
the value, records `history:<k>` and marks `outdated:<k>` (view with
`/outdated`, clear with `/revive`); `/link` cross-references memories
(`/assoc` lists both directions). When an old error recurs you can see
the fix, why it was tried, and what superseded it. `/recall?q=` bundles
search + outdated + history + associations.

**Mandate** (`--mandate "..."` or `--mandate-file`, or `/mandate` as a
console op) — the memory module is not optional: agents must read
`/mandate` and acknowledge with `agent:<id>:ready`. The QMS
`memory-awareness` check fails any configured agent that has not
acknowledged.

```sh
exomind /set?key=project:alpha:deadline --body '2026-09-01'
exomind /outdate?key=iter10:plan&reason=replaced --body 'iter11 plan superseded it'
exomind /link?from=iter11:plan&to=iter10:plan&rel=replaces
exomind /recall?q=parser
```

## API (exomind)

Plain text, lowercase answers (`ok`, `missing`, `error: <reason>`), one
record per line, tab-separated. An agent learns the whole API from
`GET /` — the software describes itself.

| method | path | purpose |
|--------|------|---------|
| GET | `/` | help / self-describing spec |
| GET | `/ping` | liveness: `pong` |
| GET | `/repl?from=N` | raw log records from byte offset N (replication tail) |
| POST | `/set?key=k` | store a value (raw body + `?key=`, form, or JSON; `ttl=` optional) |
| GET | `/get?key=k` | read raw value; 404 body `missing` |
| POST | `/append?key=k` | append body to a value, newline-separated |
| DELETE | `/del?key=k` | delete |
| GET | `/list` | keys (`prefix=`, `limit=`, `offset=`, `sort=desc`) |
| GET | `/search?q=t` | ranked substring search over keys and values |
| GET | `/embed?key=k` | read a stored vector (`dim 256 i:v ...`) |
| POST | `/embed?key=k` | embed raw body, store as `vec:<k>` (TTL works) |
| DELETE | `/embed?key=k` | remove a vector |
| POST | `/sim?k=10` | rank keys by vector similarity to the body text |
| POST | `/note` | store body as a timestamped note, answers `ok <key>` |
| GET | `/notes` | notes newest-first (`q=`, `limit=`, `offset=`) |
| POST | `/batch` | JSON array of ops; one result line per op |
| GET | `/stats` | counters and health |
| GET | `/snapshot`, POST `/restore` | full dump / atomic restore |
| POST | `/backup` | write a timestamped backup copy |
| GET | `/project` | project store location |
| POST | `/outdate?key=k&reason=...` | mark a memory outdated (kept in history) |
| GET | `/outdated?key=k` | outdated marker + history of a key |
| POST | `/revive?key=k` | clear an outdated marker |
| POST | `/link?from=a&to=b&rel=...` | associate two memories |
| GET | `/assoc?key=k` | associations of a key (both directions) |
| GET | `/recall?q=` | search + outdated + history + associations |
| GET | `/mandate` | the mandatory briefing (ack: `agent:<id>:ready`) |

Any listing endpoint accepts `json=1` for machine-readable JSON. Query
values (reasons, values with spaces) must be URL-encoded (`%20` for
spaces) or the request is rejected.

The same API, three ways:

```sh
# console ops (one-shot, no daemon):
exomind /set?key=project:alpha:deadline --body '2026-09-01'
exomind /set?key=tmp:build:status&ttl=3600 --body building
exomind /append?key=log:session:42 --body 'step 3 done'
exomind /batch --body '[{"set":"a","value":"1"},{"get":"a"},{"del":"a"}]'
exomind /note --body 'remember to revisit the parser edge case'
exomind /search?q=parser
exomind /notes?q=deadline

# over HTTP to a serving daemon (identical semantics):
curl -X POST 'localhost:7654/set?key=project:alpha:deadline' -d '2026-09-01'
curl 'localhost:7654/search?q=parser'

# as MCP tools:
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"set","arguments":{"key":"k","value":"v"}}}
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
- **Vectors (exovec)** — `POST /embed` hashes the body into a fixed
  256-dimension count vector (lowercased, word character-trigrams,
  FNV-1a mod 256, clamped counts), stored as an ordinary `vec:<k>` key —
  local and deterministic, no model, no network. `/sim` answers the top-k
  by cosine similarity. An in-memory index makes the scan cover only
  vectors.
- **Auth scopes** — `--tokens` file tokens can be read-only and/or
  prefix-scoped; every endpoint enforces the scope.
- **TTLs** — lazy expiry on read/query; expired keys never leak back
  during compaction.
- **Concurrency** — thread per connection, one mutex over the store.
- **No dependencies** — no libs, no package manager, single
  static-friendly binary.

## Tests

Every module carries its own hermetic suite at `<module>/test/test.sh`
(`bash test/test.sh` from the module directory), runnable from the repo
root as `make test-<module>`:

```sh
make test              # exomind: every endpoint, persistence across
                       # restarts, tombstones, TTL expiry, bearer + scoped
                       # auth, SIGKILL crash recovery, concurrent writers,
                       # snapshot round-trips, vector recall
make test-exosched test-exoflow test-exodoc test-exoqms test-exocrawl \
     test-exocontext test-exokit
```

The suites spawn their own private daemons and temp data and never touch
shared instances. The QMS gate runs `./build/exomind --version` as the
in-budget smoke command (5s audit budget; the full suites are too slow
for it and run via `make test-<module>`).

## Limitations

- No clustering or replication built in (single-writer log).
- Substring search (`/search`) is linear in the number of keys
  (10k keys ≈ 30 ms); only prefix listing is indexed.
- The GUI-free, machine-first design assumes an agent or CLI operator.

## exosched — the alarm clock

A scheduled-reminders + WebSocket-push daemon (C11, zero dependencies:
libc + pthread only). Durable state lives entirely inside exomind under
keys `exosched:timer:<id>` with a TTL slightly past fire time — no state
on disk, exomind is the only source of truth. Timers survive restarts,
fired timers expire on their own, and every fire grows exomind's
searchable note feed.

Build: `make exosched` produces `exosched/build/exosched`; test with
`make test-exosched`.

### Console operations

```sh
exosched /remind --body 'in 90s "water the plants"'   # POST; body = schedule
exosched /timers                                      # active timers
exosched /timer?id=<id>                               # cancel: `ok` / `missing`
exosched /ping                                        # pong
exosched                                              # the full spec
```

| method | path | purpose |
|--------|------|---------|
| GET | `/` | full spec (self-describing) |
| GET | `/ping` | liveness: `pong` |
| POST | `/remind` | schedule a reminder (see below) |
| GET | `/timers` | active timers (`json=1` for JSON) |
| DELETE | `/timer?id=<id>` | cancel a timer: `ok` or `missing` |
| GET | `/ws` | WebSocket push channel (RFC 6455; server mode only) |

Errors are `error: <reason>`.

`--exomind` defaults to `http://127.0.0.1:7654`; server mode binds 7655
by default (`--serve` or `--port`). Set `--token` (or env
`EXOSCHED_TOKEN`) to require `Authorization: Bearer <token>` on every
request, including the WebSocket upgrade.

### Scheduling

`POST /remind` takes a plain-text body:

```
in 90s "water the plants"
in 5m "stand up and stretch"
in 2h "push the branch"
in 3d "renew the certificate"
at 1786740704 "fire at this unix epoch"
every 10m "check the pipeline"
every 30s "nudge" until 1786740704
```

Units: `s m h d`. `every <n><unit> "msg"` schedules a RECURRING timer;
the optional `until <epoch>` suffix (quoted messages only) stops it after
the fire at or before that epoch. `at` in the past is rejected. The
message may be quoted (`\"` and `\\` escapes) or unquoted to the end of
the body. The answer is `ok <id> <when-epoch>` with an id of the form
`<epoch>:<8-hex>`.

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
  catch up via `exomind /notes`.
- `at`-style reminders are rejected in the past; there is no timezone
  support (all epochs are Unix time).
- A timer whose fire is retried during an exomind outage is kept and
  retried every 5s — reliable, but a long outage can pile up pending fires.

### Tests

`bash exosched/test/test.sh` runs its own private exomind and exosched on
private ports (temp data under /tmp); it never touches a shared exomind.
Needs `curl`, `python3` (the WebSocket client) and `ss`. Coverage
includes one-shot and recurring timers, `every` cadence (measured from
note epochs), persistence across SIGKILL restarts, `until` semantics,
cancel of a recurring timer, the 6-column `/timers` TSV and `json=1`
fields, reload catch-up of overdue timers, and the reload/cancel race: a
timer cancelled while a degraded-startup background reload is in flight
is never resurrected by the stale snapshot.

## exoflow — the swarm orchestrator

A dependency-graph task orchestrator for AI-agent swarms. A flow is a
DAG of steps; an arbitrary number of agents pull work from it with
`/next`, execute, and report back with `/step`. exoflow guarantees that
every step is claimed by exactly one worker and only becomes runnable
once all of its dependencies are done. Durable state lives in exomind
(external long-term memory); step deadlines are enforced through exosched
scheduled reminders; every claim, completion and deadline is audited as
an exomind note. Since 0.2.0 a flow can be a **loop**: when its last
iteration reaches a terminal state, exoflow lazily spawns the next one
(`iter <n+1>`), repeating with a fixed interval up to `max` / `until`
limits.

```
exomind (state)   <-+-  exoflow   <-+-- /next?flow=F&worker=W   (workers)
exosched (alarms) <-+      |       +-- /step?flow=F&id=s done
                      (audit notes into exomind)
```

### Console operations

```sh
# create a flow (POST; body on --body or stdin)
exoflow /flow --body "ship
s1<TAB>build<TAB>
s2<TAB>test<TAB>s1
s3<TAB>lint<TAB>s1
s4<TAB>package<TAB>s2,s3<TAB>$(date +%s --date '+120s')
s5<TAB>publish<TAB>s4"
# -> ok <flow-id> 5

# drive it
exoflow /next?flow=<flow-id>&worker=cli     # ok s1  (auto-claims)
exoflow /step?flow=<flow-id>&id=s1 --body 'done cli'
exoflow /flow?id=<flow-id>                  # TSV state
exoflow /flows                              # all flows
exoflow /loops                              # loop iterations
exoflow /flow?id=<flow-id>&action=cancel    # cancel non-terminal steps
exoflow /flow?id=<flow-id>&action=stop-loop # halt future loop iterations
exoflow /ping
```

| method | path | purpose |
|--------|------|---------|
| GET | `/` | full spec |
| GET | `/ping` | liveness: `pong` |
| POST | `/flow` | create a flow (body: name, then `id<TAB>desc<TAB>deps` lines) |
| GET | `/flow?id=<f>` | one flow, TSV or `json=1` |
| DELETE | `/flow?id=<f>` | remove a flow and its keys |
| POST | `/flow?id=<f>&action=cancel` | cancel non-terminal steps |
| POST | `/flow?id=<f>&action=stop-loop` | halt future iterations of a loop |
| GET | `/flows` | flow list (`status=`, `limit=`, `offset=`) |
| GET | `/loops` | loop iterations list |
| GET | `/next?flow=<f>&worker=<w>` | claim the next ready step: `ok <stepid>` or `none` |
| POST | `/step?flow=<f>&id=<s>` | body `done [note]` / `failed [note]` / `unclaim` |

Console ops run in-process but the flow state lives in exomind, so
`/flow`, `/next` and `/step` need a serving exomind; `/ping` and the
guide work without one. To run the contrib worker (which drives a
serving daemon over HTTP), start a server:

```sh
build/exomind --serve --port 7654 --data /tmp/xm.dat &
exosched/build/exosched --serve --port 7655 --exomind http://127.0.0.1:7654 &
exoflow/build/exoflow --serve --port 7676 --exomind http://127.0.0.1:7654 \
    --exosched http://127.0.0.1:7655 &
```

`--exomind` defaults to `http://127.0.0.1:7654`, `--exosched` to
`http://127.0.0.1:7655`, server port to 7656. `--token` (or env
`EXOFLOW_TOKEN`) requires `Authorization: Bearer <secret>` on every
request.

### The worker loop (`exoflow/contrib/worker.sh`)

A simulated agent that runs the orchestration loop against any compliant
serving exoflow: repeatedly `GET /next?flow=F&worker=ME` (which
auto-claims one runnable step), "executes" it (a short sleep; if the step
description starts with `fail:` the step is failed on purpose), then
reports back with `POST /step`. `none` means no runnable work remains and
the worker exits 0.

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
`worker <name>: done s2` / `worker <name>: no work left`. Run two of them
in parallel against one flow to see claim exclusivity in action — each
step is claimed by exactly one worker (park the workers with `-r` so they
contend at every level of the graph):

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
  scheduled reminder with exosched; when it fires (or on the next read,
  via the lazy deadline sweep) exoflow marks the step `overdue` and
  writes an audit note. `GET /flow?id=` reflects the overdue state.
  Registration is best-effort: a down exosched never breaks flow
  creation, and the lazy sweep is authoritative.
- **Audit via notes.** Claims, step completions/failures, cancellations
  and deadline misses are written as timestamped exomind notes, which
  gives you an append-only ledger you can query with `exomind /notes?q=<flow-id>`.
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
timer-driven spawn. On every `/flows`, `/flow?id=`, `/loops`, `/next`
and every startup reload, exoflow checks whether the NEWEST iteration of
a loop is terminal (all done / all failed / cancelled) and its `next_run`
has arrived; if so it spawns the next iteration:

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

| method | path | body / params | reply |
|--------|------|---------------|-------|
| GET | `/` | — | self-describing text |
| GET | `/ping` | — | `pong` |
| POST | `/flow` | line 1 = flow name; then `id<TAB>desc<TAB>deps` lines, deps comma-separated (empty allowed); optional 4th field `deadline_epoch`, optional 5th field `timeout_s`; optional LAST line `loop<TAB>every <n><s|m|h><TAB>[max <n>] [until <epoch>]` | `ok <flow-id> <nsteps>` |
| GET | `/flow?id=` | — | TSV state (one line per step; `loop` line for loops) |
| GET | `/flows` | — | flow list |
| GET | `/loops` | — | loop iterations list |
| GET | `/next?flow=&worker=` | — | `ok <stepid>` (auto-claims) or `none` |
| POST | `/step?flow=&id=` | `done [note]` / `failed [note]` / `unclaim` | `ok` |
| POST | `/flow?id=&action=cancel` | — | `ok` |
| POST | `/flow?id=&action=stop-loop` | — | `ok` |
| DELETE | `/flow?id=` | — | `ok` |

Step ids and worker names are restricted to `[A-Za-z0-9._-]` (they travel
inside URLs, TSV columns and the deps column). Rejects: duplicate ids,
unknown deps, cyclic deps.

### Limitations

- exoflow is a *pull* orchestrator: workers must call `GET /next`; there
  is no push of new work to idle workers.
- A step claimed by a worker that dies is released only via an explicit
  `unclaim` (or the deadline sweep marking it `overdue`); there is no
  lease timeout / requeue-on-heartbeat.
- Deadline enforcement is best-effort lazy: the authoritative sweep runs
  on reads (`/flow`, `/next`) and startup reload, so an overdue step is
  reflected at the next read, not at the exact deadline instant.
- Loop scheduling is lazy too: the next iteration spawns at the first
  read after the newest iteration is terminal and `next_run` has arrived
  (the exosched reminder `exoflow:loop:<id>` only surfaces the due moment
  in the note feed). There is no timer-driven push.

### Integration tests

`exoflow/test/test-integration.sh` spawns a private stack — exomind,
exosched, exoflow on private ports, temp data under /tmp — and proves
the loop end-to-end:

1. creates the 5-step diamond (`s1 → s2,s3 → s4 → s5`) with a deadline
   on `s4`, runs **two `contrib/worker.sh` workers in parallel**, and
   asserts every step is done, no step was claimed by both workers, and
   `s4` was claimed only after both `s2` and `s3` were done (compared via
   note timestamps in the private exomind);
2. creates a flow whose step deadline is `now+2s`, waits, and asserts
   `/flow` marks it overdue and an audit note exists;
3. SIGKILLs exoflow mid-flow, restarts it on the same ports/backend, and
   asserts the state survived and the flow still completes;
4. restarts exoflow with `--token` and asserts 401 without / success with
   the token;
5. kills all three daemons, removes temp data, prints
   `=== results: N passed, 0 failed ===`.

Run it: `make test-exoflow` (unit tests + this integration suite), or
standalone `timeout 300 bash exoflow/test/test-integration.sh`.
Environment hooks: `EXOFLOW_BIN` and `EXOFLOW_ARGS` override defaults.
The suite never touches shared swarm instances and only uses its own
ports.

## exodoc — the documentation auditor (batch)

`exodoc` v0.1.0 is a batch command-line auditor (C11, zero dependencies:
libc only) that checks component documentation against the ISO 9001
§7.5-flavored standard in `exodoc/standard.md` — the stack's quality
gate. It reads the stack manifest `docs/stack.tsv` (one component per
line: `name<TAB>dir<TAB>port<TAB>...`; empty port = batch tool), then
for each component verifies that its `README.md` satisfies the standard's
clauses: identification (purpose heading, version token), required
sections (build, run, API, state, tests, honesty), and — with `live=1` —
that the documented API and version agree with the running daemon's
self-describing `GET /` spec. It never crashes on malformed input:
documents and manifests are capped, control bytes are stripped, and
unreachable daemons are reported as `SKIP` (never fatal).

### Usage

```
exodoc /audit?live=1&stack=docs/stack.tsv&base=.&exomind=http://127.0.0.1:7654&out=/tmp/audit.txt&json=1
exodoc /audit?live=1                          # defaults: stack, base, no exomind
exodoc audit --live --stack docs/stack.tsv --base .   # legacy subcommand form
```

The `/audit` op takes the same parameters as the subcommand's flags:
`stack` (manifest path, default `docs/stack.tsv`), `base` (base dir for
component dirs, default `.`), `exomind` (persist `exodoc:audit:*` scores
+ a summary note to exomind), `out` (also write the report to a file),
`live` (crawl daemons; verify version + API conformance against `GET /`),
`json` (machine-readable report). Flag parameters accept off values
`0/false/no/off`.

Exit status is 0 for a completed run regardless of failures; the gate is
the report line `=== audit: N pass, M fail (score X%) ===` — the build
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
| c8 version | §2/§4 | version token present; matches daemon with `live=1` |
| c9 api-conformance | §5 | documented endpoints == live `GET /` endpoints |

### Design

- One pass over the manifest, then one pass over each component's README
  (headings indexed first, sections ranged to the next same-or-higher
  heading, endpoints normalized and deduplicated, version token scanned
  per the §4 rules).
- Live ground truth comes from `GET /` on the component's server-mode
  port (and the local `build/<name> --version` binary when present) —
  self-description is authoritative.
- Every check is independently `PASS` / `FAIL` / `SKIP`; `SKIP` never
  counts against a component's score, so a missing daemon degrades the
  report instead of breaking it.
- Stateful dogfooding: with `exomind=`, each component's score is stored
  as `exodoc:audit:<ts>:<component>` and a summary line lands in the
  note feed, so doc-debt history is itself queryable.

### Tests

The suite runs a live audit against its own fake daemons (PASS/FAIL/SKIP
math), JSON validity, `--out` file writing, down-daemon SKIP behavior,
version mismatch detection via binary and via spec, API mismatch
detection, and garbage-doc robustness (NUL bytes, oversized lines) —
`exodoc` must never crash on them. Needs `curl` and `python3` (the fake
daemons); `bash exodoc/test/test.sh` runs standalone in its own temp dir.

### Limitations

- `live=1` verifies version and endpoint sets only; it does not exercise
  every endpoint's behavior — behavioral conformance is the job of each
  component's own test suite.
- The manifest format, section synonyms and endpoint normalization are
  fixed by `standard.md`; a doc that uses exotic heading wording outside
  the synonym table will be flagged even if a human finds it adequate.
- A component's README version token is the FIRST `X.Y.Z` in the
  document; docs must therefore state the version before any incidental
  numeric collocation.

## exoqms — the Quality Management System

`exoqms` v0.2.0 is a QMS daemon (C11, zero dependencies: libc + pthread
only) that turns the ISO 9000 family into running code for the exomind
stack. It holds quality objectives, runs ISO 19011 audit programs
against the live stack, records non-conformities (NCs) with a full
corrective-action lifecycle, and publishes every milestone into exomind's
note feed. Its durable state lives entirely inside exomind under
`exoqms:*` keys, so the QMS itself is auditable and survives restarts
like every other layer.

The audit program runs the checks defined in `exoqms/standard.md`,
invoking `exodoc` (the documentation auditor), `exoqms-ui` (the UI
quality auditor), `exoqms-code` (the code-safety analyzer) and
`exoqms-svg` (the asset-logic analyzer) as child processes under a hard
5-second timeout each.

### ISO mapping

| ISO document | principle | exoqms feature |
|--------------|-----------|----------------|
| ISO 9000:2015 | concepts and vocabulary: quality is conformance to stated requirements | the checks in `standard.md` section 5 define the stated requirements, machine-executable |
| ISO 9001:2015 §5.2 | quality policy — commitment to quality | the quality policy statement in `standard.md` section 2 |
| ISO 9001:2015 §6.2 | quality objectives | `/objectives` — measurable objectives with metric keys and targets |
| ISO 9001:2015 §7.5 | documented information | `doc-compliance` check runs `exodoc audit --live` on every audit |
| ISO 9001:2015 §8.7 | control of non-conforming outputs | `/nc` — NCs with severity (`major`/`minor`) and source |
| ISO 9001:2015 §9.1 | monitoring, measurement, analysis and evaluation | `metrics` check — the `metric:iterN:tests_passing` trend |
| ISO 9001:2015 §10.2 | corrective action | the NC lifecycle `open → analysis → corrective → verify → closed` with mandatory evidence |
| ISO 9004:2018 | sustained success over the long term | the trend verdict (`up`/`flat`/`down`) and the stagnation flag in `/trends` and `/report` |
| ISO 19011:2018 | audit programs: planning, criteria, evidence, records | `/audit` — named audit programs, check criteria, per-check findings with evidence, durable records |

### Console operations

```sh
exoqms /objectives --body "tests-green	metric:iter1:tests_passing	100"
exoqms /nc --body "broken docs	major	README drifted from the binary"
exoqms /audit --body "audit-stack	component-tests,doc-compliance,dogfood,ui-audit,metrics,code-safety,asset-logic	a,b,b1,b2,b3,e"
exoqms /audit?criteria=metrics              # criteria in the query instead of a body
exoqms /audit?id=<audit-id>                 # findings
exoqms /objectives                          # list
exoqms /nc?status=open                      # open NCs
exoqms /issues                              # detection registry
exoqms /report                              # consolidated quality picture
exoqms /trends                              # metric trend + verdict
exoqms /ping
```

| method | path | purpose |
|--------|------|---------|
| GET | `/` | full spec (self-describing) |
| GET | `/ping` | liveness: `pong` |
| POST | `/objectives` | add objective (body below) |
| GET | `/objectives` | list objectives |
| POST | `/nc` | raise non-conformity (body below) |
| GET | `/nc?id=<id>` | NC detail |
| GET | `/nc?status=<st>` | list NCs, filtered |
| POST | `/nc?id=<id>&action=<a>` | NC lifecycle transition |
| POST | `/audit` | run an audit program (body below) |
| GET | `/audit?id=<id>` | audit report with findings |
| GET | `/audits` | audit program list |
| GET | `/issues` | detection registry (`issue:<check>` records) |
| GET | `/report` | consolidated quality picture |
| GET | `/trends` | metric trend + verdict |

A body selects POST on `/objectives`, `/nc`, `/audit`; a body-less op
falls back to GET (list / detail). Console ops need a serving exomind
(the QMS state lives there). Server mode binds 7657 by default and takes
`--exomind` (default `http://127.0.0.1:7654`), `--exosched` (default
`http://127.0.0.1:7655`), `--exodoc` (default `exodoc` on PATH),
`--ui`, `--code`, `--svg` (paths to the field-module binaries; without
them the corresponding check reports `skip`), `--rules` (rule files for
the universal checks), `--repo` (default `.`), `--agents` (default
`a,b,b1,b2,b3`), `--notes24h` (default 5), `--token` (or env
`EXOQMS_TOKEN`). Add `json=1` to listings; request bodies are
tab-separated fields.

### Objectives (ISO 9001 §6.2)

`/objectives` body: `title<TAB>metric_key<TAB>target`, with an optional
fourth field `period` (default `iter`). `met` means `value >= target`
for numeric targets, equality for string targets; a missing metric key
yields `no-data`. Answer: `ok <id>`.

### Non-conformities and corrective action (ISO 9001 §8.7, §10.2)

`/nc` body: `title<TAB>severity<TAB>description`, severity is `major`
(breach of a normative clause or regression) or `minor`. Answer:
`ok <id>`; the NC starts `open`.

```
open --analyse--> analysis --correct--> corrective --verify--> verify --close--> closed
```

`/nc?id=<id>&action=analyse|correct|verify|close` advances the NC; every
transition is written to the exomind note feed as an audit-trail entry
(`nc <id> transition: <from> -> <to> (<body>)`). Invalid transitions are
rejected naming the expected status. `close` is the escape hatch: it is
allowed from ANY status when the body carries
`corrective_action<TAB>evidence` (a third field is appended as a note, a
fourth sets `closed_by`, default `api`); from `verify` a note alone is
enough. Closed NCs drop out of the open count in `/report`.

### Audit programs (ISO 19011)

`/audit` body: `name<TAB>criteria` — criteria is a comma-separated list
of check ids (empty = all), with an optional third field `agents` for
the dogfood check. Query `?target=<path>` feeds the `ui-audit` check
(and overrides the scan target of `code-safety` and `asset-logic`).
Answer: `ok <audit-id> <score>%`; `/audit?id=` prints the record plus
one findings line per check: `check<TAB>result<TAB>evidence`. The score
is `100 * pass / (pass + fail)` rounded, `skip` not counted.

| check | passes when |
|-------|-------------|
| component-tests | every manifest test command (5th column of `docs/stack.tsv`) exits 0 within the 5s budget |
| doc-compliance | `exodoc audit --live` reports 0 fail |
| dogfood | every listed agent has `agent:<id>:status`, and ≥ `notes24h` notes exist in the last 24h |
| ui-audit | `exoqms-ui` finds 0 findings on the `?target=` page |
| metrics | the `metric:iterN:tests_passing` trend is not `down` |
| code-safety | `exoqms-code` reports 0 **major** findings on the stack's own C source (default target: the manifest source dirs; minor findings non-fatal) |
| asset-logic | `exoqms-svg --shape auto` reports 0 **major** findings on the stack's own SVG assets (default target: the repo root; minor findings non-fatal) |
| debt | `debt-*` findings ≤ `thresholds.debt` (default 10) |
| hygiene | 0 `hygiene-*` findings |
| secrets | 0 `secrets-*` findings (matched lines masked to `***`) |
| agent-health | every configured agent has responded since its last scheduled reminder (freeze detection) |
| docs-coverage | every manifest component ships the required docs files |
| kit-fidelity | the stack's `kit/` audits pass (exokit) |
| memory-awareness | every configured agent acknowledged the exomind mandate |
| issue-tracking | failed checks leave `issue:<check>` detection records |

Each check runs under a hard 5s timeout; children that overrun are
SIGKILLed and the check fails with `timed out`.

### The universal project config

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
`rules` toggles the universal checks, `thresholds.debt` is the maximum
number of `debt-*` findings that still passes, `test` runs whole-project
test commands (5s budget each, from the repo root) when there is no
stack manifest, `docs` names the required files for `doc-compliance` in
that mode, and `ignore` globs are handed to the analyzer scans.

The universal checks share one `exoqms-code --rules` scan per audit
(memoized), partitioned by check-id prefix. The rule files live in the
`--rules` directory, else `rules/` next to the `--code` binary, else
`<repo>/exoqms/code/rules`; without them the checks report `skip` so a
half-wired deployment degrades honestly.

### The field modules

Three sibling quality-audit engines live under `exoqms/`, each a
zero-dependency C11 batch binary with its own fixtures and test suite
(see each module's README for the full contract):

- [`exoqms/ui`](exoqms/ui/README.md) — the **UI quality auditor**: 7
  defect classes (emoji icons, overlapping controls, misaligned
  siblings, corner mismatches, missing backgrounds, unstyled
  sdk-default controls, WCAG AA contrast) over an HTML subset + a
  static layout model. `exoqms-ui <target> [--json] [--no-emoji]
  [--emoji-allowlist <chars>]`; exit 0 = clean, 1 = findings, 2 =
  usage/IO error. Fixtures `good.html` (0 findings) and `bad.html` (12
  findings) pin the counts.
- [`exoqms/code`](exoqms/code/README.md) — the **code-safety analyzer**:
  error-handling defects in C/C++ source plus shell/python line-based
  adapters and a text-rule engine for any language (missing-error-path,
  unchecked-deref-alloc, unchecked-return, uninitialized-use,
  swallowed-error, empty-error-branch). `exoqms-code <file-or-dir>...
  [--json] [--ignore <glob>]`; the daemon's `code-safety` check passes
  iff 0 major findings. The analyzer's own source is self-audit clean.
- [`exoqms/svg`](exoqms/svg/README.md) — the **asset-logic analyzer**:
  geometry checks on generated SVG (tree rule-set: stem taper, stem
  missing, crown roundness, proportions, symmetry, degeneracy,
  fragmentation, out-of-bounds). `exoqms-svg <target> [--shape
  tree|auto] [--json]`; `auto` skips files without a `data-shape="tree"`
  hint. Fixtures include `tree-good.svg` (0 findings) and deliberately
  broken trees with pinned counts.

The daemon invokes them through the `ui-audit`, `code-safety` and
`asset-logic` checks. Build all of them with `make exoqms` from the repo
root (or `make qms-modules` for just code + svg).

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
suites (`exoqms/test/test.sh` for the daemon, `exoqms/ui/test`,
`exoqms/code/test`, `exoqms/svg/test`). The daemon suite is too slow for
the 5s audit budget, so the stack manifest declares
`./build/exoqms --version` as the in-budget smoke command instead and
`make test-exoqms` remains the full-suite gate.

### Limitations

- The check budget is 5s per check (normative): only test commands that
  fit the budget may be declared in `docs/stack.tsv`; the full suites
  are run by `make test-exoqms`.
- The `ui-audit`, `code-safety` and `asset-logic` checks are static
  analyzers, not a browser / a compiler / a rendering engine: they catch
  defect classes, not semantics (see each module's README for the honest
  limitations).
- The `metrics` check needs at least two iterations of
  `metric:iterN:tests_passing`; with fewer it reports `skip`.
- Severity is assigned by the author of the NC; the daemon checks the
  vocabulary (`major`/`minor`), not the judgment.
- `/report` is a one-shot picture, not a historical chart; history lives
  in the note feed and the `exoqms:*` keys.

## exocrawl — the research daemon

**AI-native web research daemon — token-efficient, private, concurrent.**

The web is built for humans: HTML boilerplate, ads, cookie banners, and
search engines that optimize for sponsored content. exocrawl reduces the
web to what an AI needs — clean plain text, links, and images — with a
SearXNG-style private metasearch layer (no accounts, no cookies, no
tracking) and high-concurrency fetching with per-host pacing and identity
rotation.

### Console operations

```sh
exocrawl /search?q=bitcask&n=10                          # metasearch
exocrawl /fetch?url=https://example.com&max=8000         # HTML -> clean text
exocrawl /scrape --body "https://a.example/
https://b.example/	4000"                               # concurrent fetch-all
printf 'url1\nurl2\n' | exocrawl /scrape                 # body via stdin
exocrawl /stats                                          # counters
exocrawl /ping
exocrawl --extract page.html                             # legacy offline extraction
```

| method | path | purpose |
|--------|------|---------|
| GET | `/` | the spec |
| GET | `/search?q=...&n=10&engines=ddg,mojeek,marginalia,bing,wikipedia\|all&json=1` | independent metasearch → `rank<TAB>title<TAB>url<TAB>snippet` |
| GET | `/fetch?url=...&max=8000&links=1&images=1` | HTML → clean plain text (+ `## links` / `## images` sections) |
| POST | `/scrape` | one URL per line `[TAB max]`, concurrent fetch-all |
| GET | `/stats` | counters (fetches, errors, cache_hits, bytes) |
| GET | `/ping` | `pong` |

Server mode (`--serve` or `--port`, default 7658) serves the same
contract over HTTP; flags: `--token` / `--keys` (Bearer auth),
`--rate-limit`, `--log-level`, `--concurrency` (16), `--pace-ms` (200),
`--cache <exomind-url>` (durable cache: extracted text stored under
`exocrawl:cache:*` keys with a 24 h TTL, served on repeat fetches),
`--robots`, `--proxy http://...`. Exit codes follow the console
contract: 0 ok, 1 operation failed, 2 unknown operation.

### Why it exists

- **Token efficiency** — `/fetch` strips nav/ads/footers/cookie banners;
  headings become `# `, lists `- `, code stays verbatim. A 50 KB HTML
  page becomes a few hundred tokens.
- **Independent private search** — five engines fetched directly and
  parsed by our own adapters: DuckDuckGo HTML, Mojeek, Marginalia, Bing,
  and the Wikipedia opensearch API. No third-party aggregator (no
  SearXNG), no API keys, no accounts, no cookies; sponsored results
  filtered per engine.
- **Broad scraping** — `/scrape` fetches many URLs concurrently (worker
  pool) with per-host pacing (default 200 ms) and rotating identities;
  403/429 gets bounded retries with a different identity.
- **200% privacy** — stateless requests, no JS, no referrer, no
  persistent state unless you opt into the exomind cache.
- **TLS via curl** — the one runtime dependency: the ubiquitous `curl`
  binary provides HTTPS transport; everything else is native C.

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
- **Cache** — with `--cache <exomind-url>`, extracted text is stored
  under `exocrawl:cache:*` keys (24 h TTL) and served on repeat
  fetches.
- **Identity** — stateless requests with rotating user agents; no
  cookies, no referrer, no JS.

### Honest limitations

- No JavaScript execution — single-page apps yield their static content
  only.
- No browser-grade CSS/layout: extraction is content-order based.
- Engines can be rate-limited or captcha-gated (Bing especially); UA
  rotation and bounded retries mitigate this, and the engine list is
  configurable (edit the table in `src/search.c`).
- `robots.txt` is not consulted by default (research mode); `--robots`
  opts in, and rate limits and pacing are the politeness mechanism.

### Tests

`make test-exocrawl` — hermetic: local mock web serves fixtures for all
five engines plus a test page; asserts extraction (boilerplate removal,
headings, links, images, entities, pre verbatim), every engine's parser,
ad filtering, redirect decoding, /fetch caps, /scrape concurrency, 403
retry, stats counters, auth. ASAN-clean.

## exocontext — context continuity

A tiny daemon that compresses an agent's durable state into a bounded,
recency-ranked digest: everything under `agent:<id>:*` plus the notes
mentioning the agent, capped at a character budget. An agent that
restarts (or opens a fresh context window) reconstructs its working
state from a single `/context?agent=<id>` — no more re-reading a hundred
keys by hand.

Build: `make` produces `exocontext/build/exocontext`; `make test` runs
the hermetic suite (it spins its own exomind). Zero dependencies: C11,
POSIX, threads. The only backend is exomind — a serving exomind is
required for `/context` (the digest is computed live from it).

### Console operations

```sh
exocontext /context?agent=b2&budget=2000    # the digest
exocontext /ping                            # pong
exocontext                                  # the spec
# => # context for b2 (budget 2000 chars)
#    ## notes (newest first)
#    note:1786899721384:0000fdad  agent:b2 milestone: ...
#    ## state (agent:<id>:* keys)
#    agent:b2:plan  1) fix doc gate 2) commit 3) exocontext done
```

| method | path | purpose |
|--------|------|---------|
| GET | `/` | spec |
| GET | `/ping` | liveness: `pong` |
| GET | `/context?agent=<id>[&budget=<n>]` | the digest |
| POST | `/context` | same, body `agent=<id>&budget=<n>` |

`budget` is capped at 256 KB; values inside the digest are truncated to
400 chars each. `json=1` is accepted and ignored by design (token
efficiency: the plain form is the canonical one). Server mode binds 7659
by default; `--token <secret>` (or env) enables Bearer auth. `--exomind
<url>` is required (no default).

### Internals

- **Composition** — one exomind round trip for notes
  (`/notes?q=agent:…`, already newest-first), one for keys
  (`/list?prefix=agent:<id>:`), one batch read for values
  (`/batch`-style multi-get). A seen-set dedupes entries across the two
  sections.
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

## exokit — the behavioral development kit (batch)

Every software carries its own development kit: a `kit/` directory that
is the software's SDK-for-itself. Not a library, not a framework — a
tool + rules set that makes any development process (in any language, in
any combination) scaffold on a **behavioral contract** instead of on the
code.

The reason: standards (RFC 2119, PEP 8, …) limit implementation issues,
but not *logical* ones. A port (Rust+SvelteKit → C++ + vanilla JS/CSS)
can compile cleanly and still be a different program. exokit attacks
that at the root: the **contract is the truth, not the code**.

Build: `make` produces `exokit/build/exokit`; `make test` runs the
hermetic suite. Zero dependencies: C11, POSIX, no daemon, no server
(batch tool).

### Console operations

```sh
# scaffold a kit into <dir>/kit (contract + examples + runner shims)
exokit /init?dir=<dir>

# best-effort function inventory from C/C++ sources
exokit /extract?src=<src-dir>&out=kit/contract.tsv

# run the behavioral ledger through your implementation's runner
exokit /verify?kit=<kit-dir>[&runner=<cmd>&fn=<name>]

# compare two inventories (translation completeness)
exokit /diff?a=contract.a.tsv&b=contract.b.tsv&exact=1

# QMS-facing audit: completeness + ledger fidelity, JSON findings
exokit /audit?kit=<kit-dir>
```

The legacy subcommand forms (`exokit init <dir>`, `exokit extract
<src-dir> --out kit/contract.tsv`, `exokit verify`, `exokit diff a b
--exact`, `exokit audit`) still work and share the same exit codes:
0 pass, 1 findings/failure, 2 usage.

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
exokit /extract?src=src&out=kit/contract.tsv   # 1. inventory
#   2. write examples for every entry (incl. error cases)
#   3. implement a runner for YOUR language
exokit /verify?kit=kit                          # 4. ledger green?
exokit /audit?kit=kit                           # 5. gate
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
  `severity: major`, the same contract as the QMS field modules.
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

Documentation debt is CI-checked: `exodoc /audit?live=1` compares every
component README against its live `GET /` spec and the build fails on
any drift — the gate line is `=== audit: N pass, 0 fail ===`, currently
**70 pass / 0 fail** (score 100%) across the stack. Commits must keep
both the module test suites and this audit gate green.

## Process & records

- **TODO.md** — the roadmap (mirrored as `todo:*` keys in exomind)
- **ISSUES.md** — the issue log (mirrored as `issue:*` keys)
- Every merged change lands with tests, the doc gate green, and the QMS
  audit at 100%.

## License

GPL-3.0-only — see [LICENSE](LICENSE).
