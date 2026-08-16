# exosched — the alarm clock for AI agents

A scheduled-reminders + WebSocket-push daemon (C11, zero dependencies:
libc + pthread only). Its durable state lives entirely inside
[exomind](..) — the external long-term memory server below it in the
stack — under keys `exosched:timer:<id>` with a TTL slightly past fire
time. Timers survive restarts, fired timers expire on their own, and
every fire grows exomind's searchable note feed.

```
make exosched        # builds exosched/build/exosched (from the repo root)
make test-exosched   # runs exosched/test/test.sh (54 checks, ~2.5min)
```

## build

`make exosched` from the repo root (or `make -C exosched` directly)
produces `exosched/build/exosched` — a single C11 binary, zero
dependencies beyond libc + pthread.

## usage

```
./build/exosched --port 7655 --exomind http://127.0.0.1:7654 [--token secret]
```

`--exomind` defaults to `http://127.0.0.1:7654`, `--port` to 7655.
Set `--token` (or env `EXOSCHED_TOKEN`) to require
`Authorization: Bearer <token>` on every request, including the
WebSocket upgrade.

## endpoints

| method | path                 | purpose                             |
|--------|----------------------|-------------------------------------|
| GET    | /                    | full spec (self-describing)         |
| GET    | /ping                | liveness: `pong`                    |
| POST   | /remind              | schedule a reminder (see below)     |
| GET    | /timers              | active timers (`json=1` for JSON)   |
| DELETE | /timer?id=<id>       | cancel a timer: `ok` or `missing`   |
| GET    | /ws                  | WebSocket push channel (RFC 6455)   |

Errors are `error: <reason>` with HTTP 4xx/5xx.

## scheduling

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

## durability (dogfooding exomind)

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

## websocket push

`GET /ws` performs the RFC 6455 handshake (SHA-1 + base64 accept key
implemented by hand). The server then pushes one text frame per fired
timer to every connected client:

```
timer <id> <epoch> <message>
```

Clients send nothing; close frames are answered and the socket closed,
dead clients are purged on the next broadcast, pings get pongs.

## design

- One pthread timer loop (condvar, CLOCK_MONOTONIC deadlines, 100ms
  granularity, woken on add/cancel).
- Wall clock only for `at` parsing, `remaining_s` output and exomind
  notes.
- One thread per HTTP/WS connection, like exomind.
- No state on disk: exomind is the only source of truth.

## limitations

- exosched is a *timer* daemon, not a durable message broker: fired timers
  are pushed over WebSocket and logged to the exomind note feed, but there
  is no replay queue — an agent that was disconnected at fire time must
  catch up via `GET /notes`.
- `at`-style reminders are rejected in the past; there is no timezone
  support (all epochs are Unix time).
- A timer whose fire is retried during an exomind outage is kept and
  retried every 5s — reliable, but a long outage can pile up pending fires.

## tests

`bash test/test.sh` runs its own private exomind (port 7660, data in
/tmp/exosched_test) and exosched (port 7661); it never touches a
shared exomind. Needs `curl`, `python3` (the WebSocket client) and
`ss`.

Coverage includes the 0.2.0 recurring-timer surface: `every`
cadence (measured from note epochs), persistence across SIGKILL
restarts, `until` semantics (stops after the last fire, past `until`
rejected, past `at` rejected), DELETE of a recurring timer, the
6-column `/timers` TSV and `json=1` `repeat_s`/`until` fields, reload
catch-up of overdue recurring timers, 0.1.0 one-shot wire values
(`fire\tmsg`) loading and firing, and the reload/cancel race: a timer
cancelled while a degraded-startup background reload is in flight is
never resurrected by the stale snapshot.

## License

GPL-3.0-only — see [LICENSE](../LICENSE).
