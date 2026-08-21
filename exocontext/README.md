# exocontext v0.4.0-alpha.1 — context continuity for AI agents

A tiny daemon that compresses an agent's durable state into a bounded,
recency-ranked digest: everything under `agent:<id>:*` plus the notes
mentioning the agent, capped at a character budget. An agent that
restarts (or opens a fresh context window) reconstructs its working
state from a single `GET /context?agent=<id>` — no more re-reading a
hundred keys by hand.

Part of the [exomind stack](../README.md) — the main README is the full
stack reference (this file has the complete exocontext documentation).

## Build

```sh
make            # produces exocontext/build/exocontext
make test       # 51 hermetic tests (spins its own exomind)
```

Zero dependencies: C11, POSIX, threads. The only backend is exomind.

## Run

```sh
./exocontext/build/exocontext --serve --port 7659 \
    --exomind http://127.0.0.1:7654 &
```

`--serve` is the only way to start the HTTP server (together with
`--port`); without it the binary never binds a port. `--token <secret>`
enables Bearer auth. `GET /` prints the full spec.

## Console operations

The digest also runs as a one-shot, in-process console operation — same
routing, no socket, no daemon. `--body <text>` supplies the POST-style
body (or stdin, when stdin is not a terminal).

```sh
exocontext /context?agent=b2&budget=2000 --exomind http://127.0.0.1:7654
exocontext /context --exomind http://127.0.0.1:7654 --body 'agent=b2&budget=2000'
echo 'agent=b2' | exocontext /context --exomind http://127.0.0.1:7654
```

Exit codes: `0` success, `1` operation failed (e.g. `error: missing
agent`, or a missing `--exomind` backend), `2` unknown operation. No
arguments at all prints the guide (the same text `GET /` serves) and
exits 0.

## API

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
#    ## summary (compressed history)      <- only after auto-compression
#    # summary 5 entries (compressed at 1786899721)
#    decided: ship v1
#    state: docs green
#    (+5 entries compressed)
#    ## notes (newest first)
#    note:1786899721384:0000fdad  agent:b2 milestone: ...
#    ## state (agent:<id>:* keys)
#    agent:b2:plan  1) fix doc gate 2) commit 3) exocontext done
```

`budget` is capped at 256 KB; values inside the digest are truncated to
400 chars each. `json=1` is accepted and ignored by design (token
efficiency: the plain form is the canonical one).

## Auto-compression of long sessions

Sessions are the `agent:<id>:*` keys in exomind (each key = one log
entry; the digest is the session log view). When a session's stored log
grows past a byte budget, the oldest entries are condensed instead of
kept verbatim:

- **Budget** — `EXO_CTX_BUDGET` bytes per session (default 16384,
  measured as `strlen(key)+strlen(value)+2` over the live keys). A
  `GET /context` over the budget folds the *oldest* entries (keys list
  asc = timestamp-in-name order) until the live tail fits; the newest
  entry is never folded.
- **Summary** — folded entries are condensed into `agent:<id>:summary`
  (a companion key inside the session namespace): a line-based digest
  with the `# summary <n> entries (compressed at <epoch>)` header, the
  `decided:` / `state:` lines of the folded entries carried forward
  (other lines dropped), and a `(+N entries compressed)` footer. Repeat
  compression merges into the same key: the count accumulates, state
  lines dedupe (capped at 64 lines).
- **Evidence** — `ctx:summary:<id>` (an epoch timestamp) records the
  last compression outside the session namespace, so compression is
  observable without polluting the digest.
- **Re-expansion** — on resume the digest emits the summary as a
  `## summary (compressed history)` section right before the live tail,
  so a resuming agent sees summarized decisions/state then the recent
  entries. The `/context` op contract is unchanged (no new query
  params; re-expansion is automatic and backward compatible).
- **Failure semantics** — compression is best-effort and write-after:
  the summary is persisted before any key is deleted; backend failures
  are logged and the live keys are left in place (the op still
  succeeds).

## Internals

- **Composition** — one exomind round trip for notes (`/notes?q=agent:…`,
  already newest-first), one for keys (`/list?prefix=agent:<id>:`), one
  batch read for values (`/batch`-style multi-get). A seen-set dedupes
  entries across the two sections.
- **Budget** — lines are emitted until the budget is consumed; the
  crossing line is kept, then emission stops. Values are cut at 400
  chars so a single runaway key cannot eat the whole budget.
- **Server** — thread-per-connection, single read loop honoring
  Content-Length, bearer auth checked before routing. Console mode
  dispatches the same routing in-process, backend included.
- **No state** — the digest is computed from exomind on every request;
  exocontext is stateless and can be restarted freely.

## Tests

`make test` covers digest composition (notes + state keys, other agents
excluded), budget capping, POST bodies, error paths, bearer auth, and
auto-compression (fold, re-expansion on resume, re-compression merge,
small sessions untouched). The QMS gate runs `./build/exocontext --version` (in-budget smoke).

## Limitations

- Recency for keys is approximated by `sort=desc` on the key name
  (timestamps are not in `/list`), so `agent:<id>:z*` keys may sort
  before newer `agent:<id>:a*` keys; notes are genuinely newest-first.
- Compression folds `agent:<id>:*` keys only; the exomind notes stream
  mentioning the agent is never folded (it is shared and oldest-first
  trimming there would be destructive). Summary content is still
  syntactic, not semantic: only `decided:`/`state:` lines are carried.
- The digest is a flat dump, not a summary: compression is by cap and
  truncation, not by meaning.
- One agent namespace per request; cross-agent digests are out of scope.

## License

GPL-3.0-only — see [LICENSE](../LICENSE).
