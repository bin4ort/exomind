# exomind

**External long-term memory for AI agents.**

The first AI-native software: designed by an AI, built in C, and usable only by
machines. It has no GUI, no CLI menu, no human-facing interface at all — just an
HTTP API whose responses are shaped for LLM token efficiency. Agents lose their
context when a session ends; exomind is the durable part of an agent's mind that
survives across sessions, machines, and restarts.

## Why it exists

An AI agent working on a long task has one hard limit: every conversation starts
from zero. exomind gives agents:

- **Durable memory** — key/value facts, decisions, and state that survive restarts
- **A knowledge feed** — timestamped notes that accumulate into a searchable log
- **Ranked search** — substring search across all keys *and* values in milliseconds
- **Batch operations** — 10,000 ops in one round trip, one line of result each
- **TTLs** — facts that expire on their own

The wire format is the API: plain text, lowercase answers (`ok`, `missing`,
`error: <reason>`), one record per line, tab-separated fields. An agent can
learn the entire API by reading the response of `GET /` — the software describes
itself.

## Build

Zero dependencies: a C11 compiler and POSIX.

```sh
make            # produces build/exomind
make test       # full functional suite (needs curl)
bash test/bench.sh  # optional benchmark
```

## Run

```sh
./build/exomind                     # listens on 127.0.0.1:7654, data in exomind.dat
./build/exomind --port 9999 --data ~/.exomind/exomind.dat
EXOMIND_TOKEN=secret ./build/exomind   # require Bearer token auth
```

## API

Self-describing: `curl localhost:7654/` prints the full spec. In short:

| method | path | purpose |
|--------|------|---------|
| GET | `/` | help / self-describing spec |
| GET | `/ping` | liveness: `pong` |
| POST | `/set` | store a value (raw body + `?key=`, form, or JSON) |
| GET | `/get?key=k` | read raw value; 404 body `missing` |
| POST | `/append?key=k` | append body to a value, newline-separated |
| DELETE | `/del?key=k` | delete |
| GET | `/list` | keys (`prefix=`, `limit=`, `offset=`, `sort=desc`) |
| GET | `/search?q=t` | ranked substring search over keys and values |
| POST | `/note` | store body as a timestamped note, answers `ok <key>` |
| GET | `/notes` | notes newest-first (`q=`, `limit=`, `offset=`) |
| POST | `/batch` | JSON array of ops; one result line per op |
| GET | `/stats` | counters and health |

Any listing endpoint accepts `json=1` for machine-readable JSON.

### Examples

```sh
# remember a fact forever
curl -X POST 'localhost:7654/set?key=project:alpha:deadline' -d '2026-09-01'

# remember it only for an hour
curl -X POST 'localhost:7654/set?key=tmp:build:status&ttl=3600' -d 'building'

# JSON is fine too
curl -X POST localhost:7654/set \
  -d '{"key":"cfg:compiler","value":"gcc","ttl":0}'

# accumulate a work log
curl -X POST 'localhost:7654/append?key=log:session:42' -d 'step 3 done'

# many operations, one round trip
curl -X POST localhost:7654/batch -d '
  [["set","a","1"],["set","b","2"],["get","a"],["del","b"]]'
# => set a ok
#    set b ok
#    get a 1
#    del b ok

# leave a note to your future self
curl -X POST localhost:7654/note -d 'remember to revisit the parser edge case'
# => ok note:1753948800000:3fa2

# read back the last 20 notes, newest first
curl 'localhost:7654/notes?limit=20'

# find everything that mentions "parser"
curl 'localhost:7654/search?q=parser'
```

## Internals

- **Storage** — append-only log (Bitcask-style) with an in-memory hash index.
  Records carry CRC32 checksums over key+value. Write = one append; reads are a
  single `pread`.
- **Crash safety** — every acknowledged interactive write is fsynced. A torn
  tail record from a crash is detected on load and the log truncated back to
  the last good offset. Tested with SIGKILL mid-write.
- **Compaction** — when the log exceeds 64 MB with >33% dead bytes (overwrites,
  deletions), live records are rewritten atomically via rename.
- **TTLs** — expiry is checked lazily on read/query and skipped during
  compaction; expired keys never leak back.
- **Concurrency** — thread per connection, one mutex over the store. The test
  suite hammers it with parallel writers.
- **No dependencies** — no libs, no package manager, single static-friendly
  binary.

## Performance

Measured on a desktop Linux box, 10,000 records of 100 bytes each:

| operation | time |
|-----------|------|
| 10,000 sets (one batch request, one fsync) | ~1.1 s |
| 10,000 gets (one batch request) | ~32 ms |
| substring search across 10k records | ~31 ms |

## Tests

`make test` covers every endpoint, persistence across restarts, tombstone
survival, TTL expiry, bearer auth, SIGKILL crash recovery, and concurrent
writers.

## Roadmap

- Multi-client WebSocket/push for timers and scheduled reminders
- Vector-ish embedding storage for semantic recall
- Namespaced access tokens
- Snapshot/restore endpoints

## License

MIT — see [LICENSE](LICENSE).
