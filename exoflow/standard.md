# exoflow standard v0.4.0-alpha.1 — the orchestrator standard for the AI-native stack

Normative document. This standard defines the requirements for the `exoflow`
daemon — a dependency-graph task orchestrator for agent swarms — covering
its API surface, durability story, claim semantics, loop scheduling, and
test expectations. Every requirement below is checkable by `exodoc audit`
(doc checks c1–c7, c8) or by the QMS checks.

## 1. Scope

This standard applies to every deployment of the `exoflow` component of
the exomind stack. `exoflow` orchestrates agent work as DAGs of steps;
workers claim runnable steps with `GET /next` and report completion
with `POST /step`. All durable state lives in `exomind`.

## 2. Identification

- **c1 purpose** — this document opens with a top-level heading naming
  `exoflow` followed by a non-empty first paragraph describing its
  purpose (dependency-graph task orchestrator for AI-agent swarms).
- **c8 version** — the document contains a version token matching `v?X.Y.Z`
  (the first such token: `v0.4.0-alpha.1`).

## 3. API surface

| method | path                     | purpose                           |
|--------|--------------------------|-----------------------------------|
| GET    | `/`                      | self-describing spec              |
| GET    | `/ping`                  | liveness: `pong`                  |
| POST   | `/flow`                  | create a flow (TSV body)          |
| GET    | `/flow?id=`              | one flow, TSV state               |
| GET    | `/flows`                 | list flows                        |
| GET    | `/loops`                 | loop iterations list              |
| GET    | `/next?flow=&worker=`    | claim the next ready step         |
| POST   | `/step?flow=&id=`        | done / failed / unclaim           |
| POST   | `/flow?id=&action=cancel`| cancel non-terminal steps         |
| POST   | `/flow?id=&action=stop-loop`| halt future loop iterations    |
| DELETE | `/flow?id=`              | remove a flow and its keys        |

Every endpoint is self-describing at `GET /`. Errors are `error: <reason>`
with appropriate HTTP 4xx/5xx status codes.

## 4. Build and run

Build: `make exoflow` from the repo root produces `exoflow/build/exoflow`.
Run: `./build/exoflow --serve --port 7656 --exomind http://127.0.0.1:7654 --exosched http://127.0.0.1:7655`.
Console mode: `./build/exoflow /flows --exomind http://127.0.0.1:7654`.

## 5. Flow model

A flow is a DAG of steps. Each step has: an id, a description, and
comma-separated dependency ids. Steps become runnable when all their
dependencies reach a terminal state (done or failed). A step with no
dependencies is immediately runnable.

Body format for `POST /flow`:
```
flow_name
step_id<TAB>description<TAB>dep1,dep2
step_id<TAB>description<TAB>dep3<TAB>deadline_epoch<TAB>timeout_s
loop<TAB>every <n><s|m|h><TAB>[max <n>] [until <epoch>]
```

The optional 4th column on a step line is a `deadline_epoch` (Unix time);
the 5th column is `timeout_s` (claim timeout in seconds).

## 6. Claim and step semantics

`GET /next?flow=<id>&worker=<name>` atomically claims one runnable step
for the named worker. The claim is exclusive — exactly one worker gets
the step. The answer is `ok <stepid>` or `none`.

`POST /step?flow=<id>&id=<stepid>` with body `done [note]`, `failed [note]`,
or `unclaim` transitions the step. Notes are written to the exomind feed
as audit records.

## 7. Loop scheduling

A flow can be made a loop by adding a last line:
`loop<TAB>every <n><s|m|h><TAB>[max <n>] [until <epoch>]`. Loop
iterations are spawned **lazily**: on every read (`/flows`, `/flow`,
`/loops`, `/next`) and startup reload, exoflow checks whether the newest
iteration is terminal and `next_run` has arrived; if so it spawns the
next iteration with `iter <n+1>`, same steps all pending.

`POST /flow?id=<f>&action=stop-loop` halts future iterations. `DELETE
/flow?id=<f>` on any record halts the loop and removes that record.

## 8. Durability and state

All flow state lives in exomind under `exoflow:*` keys. On startup the
daemon reloads flows from exomind; if exomind is down, reload is retried
every second for 10s. Every state change (claim, step transition, cancel)
is written as an exomind note, providing an append-only audit ledger.

## 9. Tests

`make test-exoflow` runs the full suite including the integration test
(`test-integration.sh`) which spawns a private exomind (7674), exosched
(7675), and exoflow (7676). Coverage includes claim exclusivity,
dependency ordering, deadline enforcement, loop lifecycle, persistence
across SIGKILL restarts, bearer auth, and console operations.

## 10. Limitations

- `exoflow` is a pull orchestrator: workers must call `GET /next`; there
  is no push of new work to idle workers.
- A step claimed by a dead worker is released only via `unclaim` or the
  deadline sweep marking it `overdue`.
- Deadline and loop scheduling are best-effort lazy: the authoritative
  sweep runs on reads and startup reload, not at exact deadline instants.

## 11. Robustness

The daemon SHALL never crash on malformed input: oversized bodies are
rejected (413), unknown paths return 404, wrong methods return 405, and
malformed loop specs are rejected with `error: bad loop spec`. Bearer auth
(`--token`) is enforced on all endpoints.
