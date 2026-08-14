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
- **Semantic recall** — `/sim` finds the nearest stored vectors to any text, embedded locally (no model, no network)
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
./build/exomind --tokens tokens.txt    # extra scoped tokens (see below)
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
| GET | `/embed?key=k` | read a stored vector (`dim 256 i:v ...`) |
| POST | `/embed?key=k` | embed raw body, store as `vec:<k>` |
| DELETE | `/embed?key=k` | delete a vector |
| POST | `/sim?k=10` | nearest vectors to the body, one `key<TAB>sim` per line |
| POST | `/note` | store body as a timestamped note, answers `ok <key>` |
| GET | `/notes` | notes newest-first (`q=`, `limit=`, `offset=`) |
| POST | `/batch` | JSON array of ops; one result line per op |
| GET | `/stats` | counters and health |
| GET | `/snapshot` | lossless plain-text dump of all live records |
| POST | `/restore` | atomically replace the whole store from a snapshot |

Any listing endpoint accepts `json=1` for machine-readable JSON (the snapshot
is a pure dump format and ignores it).

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

### Snapshot & restore

`GET /snapshot` dumps every live record (no tombstones, no expired keys) as a
length-prefixed plain-text dump, safe for values containing tabs, newlines, or
binary. `POST /restore` with that body atomically replaces the entire store
(temp file + fsync + rename) and answers `ok <n_records>`; a malformed body
answers `error: bad snapshot` and leaves the store untouched.

```sh
# full backup
curl localhost:7654/snapshot > backup.txt
# restore it (overwrites everything)
curl -X POST localhost:7654/restore --data-binary @backup.txt
# => ok 137
```

Format — one record per line, raw length-prefixed key/value bytes:

```
exomind-snapshot-v1
<klen>\t<vlen>\t<key><value>
```

TTLs and write timestamps are not preserved.

### Vectors (exovec)

`POST /embed?key=k` hashes the raw body into a fixed 256-dimension count
vector and stores it durably under the ordinary key `vec:<k>` — vectors are
ordinary keys, so TTLs (`ttl=60`), scoping, snapshots, restore and crash
recovery all apply to them as-is. Answer: `ok <k> 256`.

Embedding is local and deterministic — no model, no network, no deps: the
text is lowercased and split into words on any non-alphanumeric byte; every
character 3-gram of a word is FNV-1a-hashed mod 256 into the count vector,
and words shorter than 3 characters are hashed whole. Counts are clamped to
255; cosine similarity divides by both norms, so the raw counts need no
normalization. The same text always produces the same vector.

`GET /embed?key=k` answers `dim 256` followed by each nonzero dimension as
`index:count` pairs, ascending by index: `dim 256 12:3 45:1`. A value under
`vec:k` that is not a well-formed vector answers `error: bad vector`.

`POST /sim` embeds the body and answers the top-k nearest stored vectors
(default 10, `k=` up to 1000), one per line, best first:
`key<TAB>similarity` — cosine in [0,1] at 6 decimals, `vec:` stripped from
the output key. Vectors sharing no dimension, and an empty query, match
nothing. `json=1` switches to `{"results":[{"key":"k","sim":0.934512},...]}`.
An in-memory index (rebuilt at load, kept in sync with writes) makes the
scan cover only vectors: ~15 ms over 10,000 of them.

`DELETE /embed?key=k` removes the vector. `DELETE /del?key=k` also drops the
vector for `k` when the token may write it (the reply reflects only the main
key). `/batch` supports `embed` ops: `["embed","k","text"]` or
`{"embed":"k","value":"text"}` → `embed k ok 256`.

### Scoped access tokens

With `--tokens <file>` (or alongside `--token`/`EXOMIND_TOKEN`), each line of
the file defines an extra token; `#` comments and blank lines are ignored:

```
agent2                   # full access
reader:ro                # read-only (no writes, no restore)
logs:scope=logs/*        # only keys under the logs/ prefix
logro:ro:scope=logs/*    # read-only + prefix-scoped
```

Prefix scopes are enforced on `/get /set /append /del /list /search /notes
/batch /snapshot /embed /sim`; violations answer `error: denied` (403).
Vectors live under `vec:` keys, so a scope like `logs:scope=logs/*` covers
vectors only as `vlogs:scope=vec:logs/*` (`scope=vec:*` for all vectors).
Read-only tokens are blocked on `/set /append /del /note /restore`,
POST/DELETE `/embed`, and write elements of `/batch` (per-element
`error: denied`-style `<op> <key> denied` lines), but may GET `/embed` and
POST `/sim`. `/restore` additionally requires a full-access token. Without
`--token`/`--tokens` auth stays off and everything is allowed.

## Internals

- **Storage** — append-only log (Bitcask-style) with an in-memory hash index.
  Records carry CRC32 checksums over key+value. Write = one append; reads are a
  single `pread`.
- **Crash safety** — every acknowledged interactive write is fsynced. A torn
  tail record from a crash is detected on load and the log truncated back to
  the last good offset. Tested with SIGKILL mid-write.
- **Compaction** — when the log exceeds 64 MB with >33% dead bytes (overwrites,
  deletions), live records are rewritten atomically via rename.
- **Snapshot/restore** — `GET /snapshot` emits a length-prefixed plain-text
  dump; `POST /restore` rebuilds the store through temp file + fsync + rename,
  so a torn restore never damages the live log (the old index stays valid
  until the rename succeeds).
- **Auth scopes** — extra tokens from a `--tokens` file can be read-only and/or
  prefix-scoped; every endpoint enforces the scope before touching the store.
- **Vector index (exovec)** — every `vec:` key with a well-formed vector is
  mirrored in an in-memory index (rebuilt at load from the log, updated
  incrementally on write/delete, dropped on tombstone and expiry), so `/sim`
  scans only vectors, never the log. Sparse storage: a vector with n nonzero
  dims costs 7 + 2n bytes under its `vec:` key.
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
| 10,000 embeds (one batch request, one fsync) | ~0.6 s |
| 10,000 gets (one batch request) | ~32 ms |
| substring search across 10k records | ~31 ms |
| /sim nearest-vectors over 10k vectors | ~15 ms |

## Tests

`make test` covers every endpoint, persistence across restarts, tombstone
survival, TTL expiry, bearer auth, SIGKILL crash recovery, concurrent writers,
snapshot round-trips (including binary values), restore atomicity and
malformed-input safety, scoped/read-only token enforcement, and the vector
layer (embed round-trips, /sim ranking, TTLs on vectors, /del cascade,
snapshot/restore of vectors, SIGKILL mid-embed-batch).

## Roadmap

Delivered: exovec (vector-ish embedding storage for semantic recall) landed
in 0.3.0. The 0.2.0 roadmap is complete.

## The stack so far

- **exomind** — durable external memory (this repo). Passive: agents pull.
- **exosched** — `exosched/` in this repo. The alarm clock for AI agents:
  scheduled reminders (`POST /remind`) that fire and push to every connected
  WebSocket client (`GET /ws`), with all timer state persisted inside exomind
  (`exosched:timer:*` keys, TTL'd) and fired/missed events logged as notes.
  Build with `make exosched`, test with `make test-exosched`.

## License

MIT — see [LICENSE](LICENSE).
