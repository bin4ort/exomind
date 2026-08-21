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
make               # produces build/exomind
make -j all-stack  # builds every module (11 binaries) in parallel
make exosched exoflow exodoc exoqms exocrawl exocontext exokit
make test test-exosched test-exoflow test-exodoc test-exoqms test-exocrawl
```

Header dependencies are tracked (`-MMD`), so a shared-header change
rebuilds exactly the affected modules. See
[docs/build-efficiency.md](docs/build-efficiency.md) for the measured
build profile.

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
- **Replication** — `GET /repl?from=N` streams raw log records from byte
  offset N (CRC-verified); a follower (`--repl <url>`) tails a primary,
  resumes from the last applied offset after a restart, and applies each
  record cast-once. `/stats` reports the follower's role and tail gap.
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
                       # snapshot round-trips, vector recall, self-update
make test-exosched test-exoflow test-exodoc test-exoqms test-exocrawl \
     test-exocontext test-exokit \
     test-exoqms-code test-exoqms-ui test-exoqms-svg
```

The suites spawn their own private daemons and temp data and never touch
shared instances. Current totals (all green in the gate pass): exomind
371 PASS, exosched 82, exoflow 139, exocrawl 68, exocontext 51, exodoc
36, exokit 50, exoqms 160, exoqms-code 58, exoqms-ui 35, exoqms-svg 61.
The QMS gate's component-tests check runs each manifest test command
(`docs/stack.tsv` 5th column) under the 5 s audit budget:
`./build/exomind --version`, `./build/exosched --version`,
`./build/exoflow --version`, `./build/exoqms --version`,
`./build/exocrawl --version` and `./build/exocontext --version` smokes
for the daemons, `make test` for the two batch suites that fit the
budget (exodoc 36, exokit 50); exocrawl's full suite (~17 s of
mock-server wall time) exceeds the budget, so its manifest command is
the `--version` smoke and the suite runs via `make test-exocrawl`.

## Limitations

- Single-writer by design: the follower (`--repl`) is a hot read-only
  tail (cast-once replay), not a quorum.
- Substring search (`/search`) is linear in the number of keys
  (10k keys ≈ 30 ms); only prefix listing is indexed.
- The GUI-free, machine-first design assumes an agent or CLI operator.

## exosched — the alarm clock

A scheduled-reminders + WebSocket-push daemon (C11, zero dependencies).
Durable state lives entirely inside exomind under `exosched:timer:<id>`
keys with a TTL past fire time — no state on disk. Server mode binds
7655. Full contract: [exosched/README.md](exosched/README.md).

```sh
make exosched            # exosched/build/exosched
exosched /remind --body 'in 90s "water the plants"'   # console op
exosched /delivery?detail=1                           # delivery receipts
make test-exosched       # 82 hermetic tests
```

## exoflow — the swarm orchestrator

Dependency-graph task orchestrator for agent swarms: a flow is a DAG of
steps; workers claim with `GET /next` and report with `POST /step`.
Guarantees one claim per step; loop flows spawn iterations lazily.
State lives in exomind, deadlines are enforced through exosched. Server
mode binds 7656. Full contract: [exoflow/README.md](exoflow/README.md).

```sh
make exoflow             # exoflow/build/exoflow
make test-exoflow        # 139 unit + integration tests
```

## exodoc — the documentation auditor (batch)

Batch command-line auditor that checks component documentation against
the stack's standard: required sections, version token, and live API
conformance (`--live`), with the `=== audit: N pass, M fail (score S%) ===`
gate line. No daemon, no port. Full contract:
[exodoc/README.md](exodoc/README.md).

```sh
make exodoc              # exodoc/build/exodoc
exodoc audit --live --stack docs/stack.tsv --base .   # doc-debt gate
make test-exodoc         # 36 hermetic tests + the live gate
```

## exoqms — the Quality Management System

ISO 9000-flavored QMS daemon: objectives, non-conformities with a
corrective-action lifecycle, ISO 19011 audit programs against the live
stack, trends, and the universal `.exoqms.json` project config. State
lives in exomind under `exoqms:*` keys. Server mode binds 7657. Full
contract: [exoqms/README.md](exoqms/README.md) and its field modules
[ui](exoqms/ui/README.md), [code](exoqms/code/README.md) and
[svg](exoqms/svg/README.md).

```sh
make exoqms              # exoqms/build/exoqms + the field modules
exoqms /audit --body "stack	component-tests,doc-compliance	a,b,b1,b2,b3"
make test-exoqms         # 160 daemon + 35 ui + 58 code + 61 svg tests
```

## exocrawl — the research daemon

AI-native web research: independent private metasearch (five engines),
token-efficient HTML→text extraction, concurrent scraping, opt-in
robots.txt politeness, and measurable extraction quality vs goldfiles.
Server mode binds 7658. Full contract:
[exocrawl/README.md](exocrawl/README.md).

```sh
make exocrawl            # exocrawl/build/exocrawl
make test-exocrawl       # 68 hermetic tests
```

## exocontext — context continuity

Tiny daemon that compresses an agent's durable state into a bounded,
recency-ranked digest: everything under `agent:<id>:*` plus the notes
mentioning the agent, capped at a character budget, with auto-compression
of long sessions. Stateless; the digest is computed from exomind on
every request. Server mode binds 7659. Full contract:
[exocontext/README.md](exocontext/README.md).

```sh
make exocontext          # exocontext/build/exocontext
make test-exocontext     # 51 hermetic tests
```

## exokit — the behavioral development kit (batch)

Every software carries its own development kit: a `kit/` directory that
is the software's SDK-for-itself — a behavioral contract (inventory +
examples + runner shims) instead of the code. Batch tool (init /
extract / verify / diff / audit), no daemon, no port. Full contract:
[exokit/README.md](exokit/README.md).

```sh
make exokit              # exokit/build/exokit
make test-exokit         # 50 hermetic tests + the exoflow integration suite
```
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
