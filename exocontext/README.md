# exocontext v0.4.0-alpha.1 — context continuity for AI agents

A tiny daemon that compresses an agent's durable state into a bounded,
recency-ranked digest: everything under `agent:<id>:*` plus the notes
mentioning the agent, capped at a character budget. An agent that
restarts (or opens a fresh context window) reconstructs its working
state from a single `GET /context?agent=<id>` — no more re-reading a
hundred keys by hand.

**License: GPL-3.0-only** (see the repository root).

Part of the [exomind stack](../README.md) — the main README is the full
stack reference (this file has the complete exocontext documentation).

## Build

```sh
make            # produces exocontext/build/exocontext
make test       # 24 hermetic tests (spins its own exomind)
```

Zero dependencies: C11, POSIX, threads. The only backend is exomind.

## Run

```sh
./exocontext/build/exocontext --port 7659 \
    --exomind http://127.0.0.1:7654 &
```

`--token <secret>` enables Bearer auth. `GET /` prints the full spec.

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
#    ## notes (newest first)
#    note:1786899721384:0000fdad  agent:b2 milestone: ...
#    ## state (agent:<id>:* keys)
#    agent:b2:plan  1) fix doc gate 2) commit 3) exocontext done
```

`budget` is capped at 256 KB; values inside the digest are truncated to
400 chars each. `json=1` is accepted and ignored by design (token
efficiency: the plain form is the canonical one).

## Internals

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

## Tests

`make test` covers digest composition (notes + state keys, other agents
excluded), budget capping, POST bodies, error paths, and bearer auth.
The QMS gate runs `./build/exocontext --version` (in-budget smoke).

## Limitations

- Recency for keys is approximated by `sort=desc` on the key name
  (timestamps are not in `/list`), so `agent:<id>:z*` keys may sort
  before newer `agent:<id>:a*` keys; notes are genuinely newest-first.
- The digest is a flat dump, not a summary: compression is by cap and
  truncation, not by meaning.
- One agent namespace per request; cross-agent digests are out of scope.
