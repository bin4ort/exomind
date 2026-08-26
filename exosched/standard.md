# exosched standard v0.4.0-alpha.1 — the scheduling standard for the AI-native stack

Normative document. This standard defines the requirements for the `exosched`
daemon — scheduled reminders with WebSocket push — covering its API surface,
durability story, error handling, and test expectations. Every requirement
below is checkable by `exodoc audit` (doc checks c1–c7, c8) or by the QMS
`component-tests` check.

## 1. Scope

This standard applies to every deployment of the `exosched` component of
the exomind stack. `exosched` is a scheduled-reminders + WebSocket-push
daemon backed by `exomind` (the external long-term memory). It provides
time for agents — the swarm's alarm clock.

## 2. Identification

- **c1 purpose** — this document opens with a top-level heading naming
  `exosched` followed by a non-empty first paragraph describing its
  purpose (scheduled reminders + WebSocket push, state in exomind).
- **c8 version** — the document contains a version token matching `v?X.Y.Z`
  (the first such token is the documented version: `v0.4.0-alpha.1`).

## 3. API surface

| method | path                 | purpose                             |
|--------|----------------------|-------------------------------------|
| GET    | /                    | full spec (self-describing)         |
| GET    | /ping                | liveness: `pong`                    |
| POST   | /remind              | schedule a reminder (body below)    |
| GET    | /timers              | active timers (`json=1` for JSON)   |
| DELETE | /timer?id=<id>       | cancel a timer: `ok` or `missing`   |
| GET    | /ws                  | WebSocket push channel (RFC 6455)   |
| GET    | /delivery            | delivery receipts stats             |

Every endpoint is self-describing at `GET /`. Errors are `error: <reason>`
with appropriate HTTP 4xx/5xx status codes.

## 4. Build and run

Build: `make exosched` from the repo root produces `exosched/build/exosched`.
Run: `./build/exosched --serve --port 7655 --exomind http://127.0.0.1:7654`.
Console mode: `./build/exosched /remind --body 'in 5m "msg"'`.

## 5. Durability and state

`exosched` keeps no local state on disk. All timer data lives in exomind
under `exosched:timer:<id>` keys with a TTL past fire time (fire-time +
300s). On startup the daemon lists these keys, reschedules future timers,
and logs overdue ones as `missed timer` notes. If exomind is down at
startup, reload is retried every second for 10s.

## 6. Scheduling model

- **Relative**: `in <n><s|m|h|d> "message"` — fires after the given duration.
- **Absolute**: `at <epoch> "message"` — fires at the given Unix timestamp.
- **Recurring**: `every <n><unit> "message"` — fires repeatedly; optional
  `until <epoch>` suffix stops it.
- Every fire writes a note to the exomind note feed, pushes a WebSocket
  frame to connected clients, and records delivery receipts
  (`delivery:<id>:<epoch>` keys).

## 7. WebSocket push

`GET /ws` performs an RFC 6455 handshake and pushes one text frame per
fired timer: `timer <id> <epoch> <message>`. Clients may ACK with
`ack <id> <epoch>`; the server waits a 500ms window and records the
outcome. Close frames and pings are handled; dead clients are purged on
the next broadcast.

## 8. Delivery receipts

Per-fire receipts are stored as `delivery:<id>:<epoch>` keys with a 24h
TTL: `acked <n>/<total> at <ts>`. Cumulative stats live in the
`exosched:delivery` key with counters `fired`, `acked`, `unacked`,
`last_acked_ts`, `last_unacked_ts`. `GET /delivery?detail=1` appends
per-timer receipt lines.

## 9. Tests

`make test-exosched` runs the full suite (82 checks, ~3min) which spins
a private exomind on port 7660 and a private exosched on port 7661; it
never touches a shared exomind. Coverage includes recurring-timer semantics
(`every`, `until`, `at`), persistence across SIGKILL restarts, DELETE
cancellation, `json=1` fields, delivery receipts (ACKed/unacked counters,
`detail=1`), and console-mode operations.

## 10. Limitations

- `exosched` is a timer daemon, not a durable message broker: there is no
  replay queue — agents disconnected at fire time must catch up via
  `GET /notes` in exomind.
- No timezone support — all epochs are Unix time.
- A timer whose fire is retried during an exomind outage is kept and
  retried every 5s — reliable, but a long outage can pile up pending fires.

## 11. Robustness

The daemon SHALL never crash on malformed input: oversized bodies are
rejected (413), unknown paths return 404, wrong methods return 405, and
malformed scheduling expressions are rejected with `error: bad format`.
Bearer auth (`--token` or `EXOSCHED_TOKEN`) is enforced on all endpoints
including the WebSocket upgrade.
