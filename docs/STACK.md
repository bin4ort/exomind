# The exomind Stack

**One repo, four daemons, one batch auditor — the AI-native software stack.**

This document is the single human-facing reference for the whole stack: what
it is, why it exists, how the pieces fit together, and how every piece is
audited against the ISO 9001 §7.5-flavored documentation standard in
[`exodoc/standard.md`](../exodoc/standard.md). Component-level docs live in
each directory's README; this page is the map.

| component | version | port  | role |
|-----------|---------|-------|------|
| [exomind](../README.md) | 0.3.0 | 7654 | durable long-term memory |
| [exosched](../exosched/README.md) | 0.2.0 | 7655 | scheduled reminders + push |
| [exoflow](../exoflow/README.md) | 0.1.0 | 7656 | dependency-graph orchestrator |
| [exoqms](../exoqms/README.md) | 0.1.0 | 7657 | quality management system |
| [exodoc](../exodoc/README.md) | 0.1.0 | — (batch) | documentation auditor |

## Why this stack exists: the AI-native philosophy

The stack is built for **agents, not humans**:

- **No GUI, no CLI menus, no dashboards.** Every component is a daemon that
  speaks plain text over HTTP. There is nothing to click; there is everything
  to script.
- **Machine-first plain-text APIs.** Responses are shaped for LLM token
  efficiency: lowercase answers (`ok`, `missing`, `error: <reason>`), one
  record per line, tab-separated fields. An agent can parse the whole stack
  with a couple of `curl` calls and `split`.
- **Self-describing specs.** `GET /` on every daemon returns its own full
  spec. There is no separate API documentation to keep in sync — the binary
  *is* the documentation. `exodoc` audits that the human READMEs match the
  binaries.
- **Zero dependencies.** C11 + libc + pthread only. No package managers, no
  node_modules, no containers required to run. State is append-only logs and
  memory.
- **Durability through layering.** Every daemon above the bottom one persists
  its state *inside* the daemon below it (exosched timers and exoflow flows
  live as exomind keys), so restarting any layer loses nothing.
- **Documentation is a quality gate.** `exodoc` audits every README against
  the standard and CI (`make test-exodoc`) fails on doc debt, so the docs are
  never allowed to rot.

## The components

### exomind — durable long-term memory (7654)

The foundation of the stack and its first component. A key/value store with
TTLs, timestamped notes, substring search, and a local deterministic vector
layer (`/sim`) — no model, no network. Every other daemon persists its state
here, and every daemon's events land in exomind's note feed, which grows into
a searchable paper trail for the whole swarm.

- Purpose: durable external memory that survives agent sessions.
- Port: **7654** (`127.0.0.1`).
- Durability: append-only log with CRC32, fsync-before-ack writes,
  crash-safe truncation recovery, automatic compaction; snapshot/restore
  round-trips the whole store as plain text.
- API: see [exomind README](../README.md#api) or `curl localhost:7654/`.

| method | path | purpose |
|--------|------|---------|
| GET | `/` | self-describing spec |
| GET | `/ping` | liveness: `pong` |
| POST | `/set` | store `key` → `value` (raw, form, or JSON body) |
| GET | `/get?key=k` | read raw value (404 body: `missing`) |
| POST | `/append?key=k` | append body to value, newline-separated |
| DELETE | `/del?key=k` | delete key |
| GET | `/list` | keys (`prefix=`, `limit=`, `offset=`, `sort=`) |
| GET | `/search?q=t` | ranked substring search over keys + values |
| GET | `/embed?key=k` | read stored vector (`dim 256 i:v ...`) |
| POST | `/embed?key=k` | embed raw body, store as `vec:<k>` |
| DELETE | `/embed?key=k` | delete vector |
| POST | `/sim?k=10` | nearest vectors to body, one per line |
| POST | `/note` | store body as timestamped note |
| GET | `/notes` | notes, newest first (`q=`, `limit=`, `offset=`) |
| POST | `/batch` | JSON array of ops; one result line each |
| GET | `/stats` | counters and health |
| GET | `/snapshot` | lossless dump of all live records |
| POST | `/restore` | replace entire store from a snapshot |

### exosched — the alarm clock for AI agents (7655)

A scheduled-reminders + WebSocket push daemon. Timers are plain text
(`in 90s "water the plants"`, `every 10m "check the pipeline"`, `at <epoch>`
...), stored durably in exomind under `exosched:timer:*` keys with a TTL
slightly past fire time, so timers survive restarts and fired timers expire
on their own. Every fire is written to exomind's note feed and pushed as a
WebSocket frame to every connected agent.

- Purpose: time for agents — the swarm's alarm clock.
- Port: **7655** (`127.0.0.1`).
- Durability: state lives in exomind; on startup timers are reloaded from
  exomind (future timers rescheduled, overdue ones logged as `missed timer`
  notes); if exomind is down at fire time the fire is retried until it lands.
- API: see [exosched README](../exosched/README.md) or `curl localhost:7655/`.

| method | path | purpose |
|--------|------|---------|
| GET | `/` | self-describing spec |
| GET | `/ping` | liveness: `pong` |
| POST | `/remind` | schedule a reminder (body below) |
| GET | `/timers` | active timers (`json=1` for JSON) |
| DELETE | `/timer?id=<id>` | cancel a timer: `ok` or `missing` |
| GET | `/ws` | WebSocket push channel (RFC 6455) |

### exoflow — the orchestrator for agent swarms (7656)

A dependency-graph task orchestrator. A flow is a DAG of steps; agents pull
runnable work with `GET /next` (which atomically claims one step) and report
back with `POST /step`. Claims are exclusive, a step only becomes runnable
once all its deps are done, and step deadlines become exosched reminders.
All state lives in exomind under `exoflow:flow:*` keys; every state change is
an audited note.

- Purpose: coordinate many agents working on one task.
- Port: **7656** (`127.0.0.1`).
- Durability: flows live in exomind and are reloaded on startup (retried in
  background if exomind is down); every state change is persisted before it
  is acknowledged.
- API: see [exoflow README](../exoflow/README.md) or `curl localhost:7656/`.

| method | path | purpose |
|--------|------|---------|
| GET | `/` | self-describing spec |
| GET | `/ping` | liveness: `pong` |
| POST | `/flow` | create a flow (body below) |
| GET | `/flow?id=<f>` | one flow, TSV or `json=1` |
| GET | `/flows` | list flows (`status=`, `limit=`, `offset=`) |
| GET | `/next?flow=<f>&worker=<w>` | claim the next ready step |
| POST | `/step?flow=<f>&id=<s>` | `done` / `failed` / `unclaim` |
| POST | `/flow?id=<f>&action=cancel` | cancel non-terminal steps |
| DELETE | `/flow?id=<f>` | remove a flow and its keys |

### exodoc — the documentation auditor (batch)

A batch CLI that audits component documentation against the ISO 9001
§7.5-flavored standard in [`exodoc/standard.md`](../exodoc/standard.md). It
reads the stack manifest `docs/stack.tsv`, then for each listed component
checks that its README satisfies the standard clauses (identity, purpose,
API surface, durability story, ...) — optionally against the live daemon
(`--live`), which pulls the ground truth spec from `GET /` on the component's
port. It is the deployment loop's fix step: run it, fix what it flags, run
again, until `=== audit: 0 fail`.

- Purpose: keep documentation compliant, machine-checked.
- No port: batch tool, run on demand.
- Build: `make exodoc`; test: `make test-exodoc`.

### exoqms — the Quality Management System (7657)

The QMS daemon (C11, zero dependencies) that turns the ISO 9000 family
into running code for the stack. It holds quality objectives
(ISO 9001 §6.2), monitors the `metric:iterN:tests_passing` trend as the
sustained-success indicator (ISO 9004), runs ISO 19011 audit programs
against the live stack — invoking `exodoc`, `exoqms-ui`, `exoqms-code`
and `exoqms-svg` as child processes under a hard 5s timeout each — and
records non-conformities with a full corrective-action lifecycle
(ISO 9001 §8.7/§10.2). Its durable state lives inside exomind under
`exoqms:*` keys, so the QMS itself is restarted without losing a
record, and every audit score and NC transition lands in the note feed.

- Purpose: machine-auditable quality management for the whole stack.
- Port: **7657** (`127.0.0.1`; development instances run on own ports,
  e.g. 7692, still backed by the shared exomind on 7654).
- Durability: all state is `exoqms:obj:*`, `exoqms:nc:*`,
  `exoqms:audit:*`, `exoqms:config:*` keys in exomind, reloaded on
  startup; records written while exomind is down are rejected, never
  lost silently.
- API: see [exoqms README](../exoqms/README.md) or
  `curl localhost:7657/`.

| method | path | purpose |
|--------|------|---------|
| GET | `/` | self-describing spec |
| GET | `/ping` | liveness: `pong` |
| POST | `/objectives` | add quality objective (ISO 9001 §6.2) |
| GET | `/objectives` | list objectives |
| POST | `/nc` | raise a non-conformity (ISO 9001 §8.7) |
| GET | `/nc?id=<id>` | NC detail |
| GET | `/nc?status=<st>` | list NCs, filtered |
| POST | `/nc?id=<id>&action=<a>` | NC lifecycle transition (§10.2) |
| POST | `/audit` | run an ISO 19011 audit program |
| GET | `/audit?id=<id>` | audit report with findings |
| GET | `/audits` | audit program list |
| GET | `/report` | consolidated quality picture |
| GET | `/trends` | metric trend + verdict |

ISO mapping (one line per principle→feature): **ISO 9000** vocabulary →
the machine-executable checks in `exoqms/standard.md` §5; **ISO 9001
§5.2** policy → the quality policy statement in `standard.md` §2;
**ISO 9001 §6.2** objectives → `POST /objectives`; **ISO 9001 §7.5**
documented information → the `doc-compliance` check (exodoc);
**ISO 9001 §8.7** non-conforming outputs → `POST /nc` with
major/minor severity; **ISO 9001 §9.1** measurement → the `metrics`
check; **ISO 9001 §10.2** corrective action → the NC lifecycle
`open → analysis → corrective → verify → closed` with mandatory
evidence; **ISO 9004** sustained success → the trend verdict and
stagnation flag; **ISO 19011** audit programs → `POST /audit` with
named programs, criteria and durable records.

The audit program has **seven checks** (see `exoqms/standard.md` §5):
`component-tests`, `doc-compliance`, `dogfood`, `ui-audit`, `metrics`,
plus the iteration-6 field checks `code-safety` and `asset-logic`.

**Field modules.** The QMS's batch analysis engines live as sibling
components under `exoqms/`, each a zero-dependency C11 binary with
permanent fixtures and its own test suite:

| module | binary | check | what it audits | pass rule |
|--------|--------|-------|----------------|-----------|
| [`exoqms/ui`](../exoqms/ui/README.md) | `exoqms-ui` | `ui-audit` | HTML/CSS UI defect classes (7 checks, no browser) | 0 findings |
| [`exoqms/code`](../exoqms/code/README.md) | `exoqms-code` | `code-safety` | error-handling defects in C source: unchecked returns of critical libc calls, missing error paths, null-deref paths | 0 **major** findings (minor non-fatal) |
| [`exoqms/svg`](../exoqms/svg/README.md) | `exoqms-svg` | `asset-logic` | generated SVG shape logic (tree rule-set) | 0 **major** findings (minor non-fatal) |

**Code-safety real-run (iteration 6).** The deployment loop's fix step
was applied to the code-safety field: the analyzer audited the stack's
own C source (exomind, exosched, exodoc, exoqms, exoqms-ui; exoflow
excluded while its loop feature was in flight) — **2 verified findings
before (1 major, 1 minor)** → fixes landed in
`exoqms/ui/src/util.c` (`file_read` short-read error path),
`exomind/src/store.c` (`store_close` fdatasync check) and
`exodoc/src/audit.c` (`read_sanitized` ferror check) → **0 findings
after** (major count 1 → 0). The 3 NCs
(`1786807693:288919d2` major, `1786807693:7dd8dd6b`,
`1786807693:a8c44471` minor) were lifecycle-closed
(open→analysis→corrective→verify→closed) on the live QMS with the fix
commit as evidence.

Honest gaps, documented not silenced: (1) the exoqms-code module
branch (`feat/qms-code`) had not landed when this integration was
verified — the code-safety check ran against a contract-conformant
stopgap checker (same CLI/JSON contract, `exoqms/standard.md` §5.3 c6);
the daemon wiring is module-agnostic, so landing the module later needs
no daemon change. (2) exoflow excluded from the real run (B3's loop
feature in flight). (3) an asset-logic audit with no `?target=` scans
the whole repo and therefore flags the SVG module's own *deliberate
negative fixtures* (`exoqms/svg/fixtures/`) — expected; the full
audit-program gate passes with an explicit target (`make audit-stack`).

## Stack flows

```
                        agents (any number)
                       |        |        |
          GET /next    |        |        |    POST /step done|failed
                       v        v        v
                 +-----------------------------+
                 |           exoflow           |  orchestrator (7656)
                 |   flows = DAGs of steps     |
                 +------+----------+-----------+
                        |          |
            exoflow:flow:* keys   deadlines registered as reminders
                        |          |
                        v          v
        +-------------------+   +-------------------+
        |      exomind      |   |     exosched      |
        | memory + notes +  |   | alarms + WS push  |  (7655)
        | vectors (7654)    |   +-------------------+
        |                   |         |
        |   exosched:timer:* <-- state lives here   |
        |   exoflow:flow:*  |         |
        |   exoqms:* (QMS)  |         |
        +--------+----------+   every fire:
                 |              note into exomind + WS frame
                 v
        every event appended as a timestamped NOTE
        -> /notes = the swarm's paper trail

        the QMS loop (exoqms, 7657 — runs against the same exomind):
             POST /audit -> exodoc --live + exoqms-ui + dogfood/metrics
                 -> findings -> POST /nc -> corrective action -> close
                 -> every audit + transition = a NOTE (the audit trail)
```

Reading it as a data-flow diagram: **memory** (exomind) is the single
source of truth at the bottom; **scheduler** (exosched) stores its
timers there and feeds its fires back as notes; **orchestrator**
(exoflow) stores its flows there, borrows the scheduler for deadlines,
and audits every transition into the note feed; **auditor** (exodoc)
reads the manifest and the live specs to verify the docs that describe
the whole thing; **QMS** (exoqms) reads the same memory to audit the
stack, files NCs and closes them with evidence — its records live in
exomind like everyone else's, so the quality system is part of the
paper trail it audits.

## Running the stack

```sh
make            # builds build/exomind
make exosched   # builds exosched/build/exosched
make exoflow    # builds exoflow/build/exoflow
make exoqms     # builds exoqms/build/exoqms + exoqms/ui/build/exoqms-ui
make exodoc     # builds exodoc/build/exodoc

build/exomind --port 7654 --data exomind.dat &
exosched/build/exosched --port 7655 --exomind http://127.0.0.1:7654 &
exoflow/build/exoflow --port 7656 --exomind http://127.0.0.1:7654 \
    --exosched http://127.0.0.1:7655 &
exoqms/build/exoqms --port 7657 --exomind http://127.0.0.1:7654 \
    --exodoc exodoc/build/exodoc --ui exoqms/ui/build/exoqms-ui \
    --repo . --agents a,b,b1,b2,b3,e &
```

## The quality gate

```sh
make test-exodoc        # exodoc's unit suite, then the live audit
timeout 30 ./exodoc/build/exodoc audit --live --stack docs/stack.tsv
# expect: === audit: 43 pass, 0 fail (score 100%) ===
make audit-stack        # exoqms audit program against the live stack
# expect: ok <audit-id> 100%
```

`make test-exodoc` runs exodoc's suite, then the live audit against the
shared instances on 7654/7655/7656 and fails the build unless the report
says `0 fail`. The gate is strict where it can be: version and endpoint
conformance are checked against the running daemons (falling back to the
local binaries as ground truth). When the daemons are unreachable and no
local binary exists, those checks degrade to `SKIP` — the gate then holds
on the doc checks alone (purpose, sections, version token, honesty), so CI
can run without a live stack, and a `FAIL` in that degraded state triggers
an offline cross-check audit before the build is failed.
