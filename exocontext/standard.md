# exocontext standard v0.4.0-alpha.1 — the context-continuity standard for the AI-native stack

Normative document. This standard defines the requirements for the
`exocontext` daemon — context continuity that compresses an agent's
durable state into a bounded, recency-ranked digest — covering its API
surface, compression model, test expectations, and limitations. Every
requirement below is checkable by `exodoc audit` (doc checks c1–c7, c8)
or by the QMS `component-tests` check.

## 1. Scope

This standard applies to every deployment of the `exocontext` component
of the exomind stack. `exocontext` provides context continuity: a bounded,
recency-ranked digest of an agent's state (`agent:<id>:*` keys in
exomind) plus relevant notes, enabling an agent that restarts to
reconstruct its working state from a single HTTP call.

## 2. Identification

- **c1 purpose** — this document opens with a top-level heading naming
  `exocontext` followed by a non-empty first paragraph describing its
  purpose (context continuity: bounded recency-ranked digest).
- **c8 version** — the document contains a version token matching `v?X.Y.Z`
  (the first such token: `v0.4.0-alpha.1`).

## 3. API surface

| method | path | purpose |
|--------|------|---------|
| GET | `/` | spec (self-describing) |
| GET | `/ping` | liveness: `pong` |
| GET | `/context?agent=<id>[&budget=<n>]` | the digest |
| POST | `/context` | same, body `agent=<id>&budget=<n>` |

Every endpoint is self-describing at `GET /`. Errors are `error: <reason>`
with appropriate HTTP 4xx/5xx status codes.

## 4. Build and run

Build: `make` from the repo root produces `exocontext/build/exocontext`.
Run: `./build/exocontext --serve --port 7659 --exomind http://127.0.0.1:7654`.
Console mode: `./build/exocontext /context?agent=b2 --exomind http://127.0.0.1:7654`.

## 5. Digest composition

The digest consists of two sections:
- **Notes**: everything under `agent:<id>:*` keys in exomind, newest first.
- **State**: all exomind notes mentioning the agent id, newest first.

Values inside the digest are truncated to 400 chars each. The total
output is capped at the `budget` (default 16384 bytes, capped at 256 KB).

## 6. Auto-compression

When a session's stored keys exceed the budget, the oldest entries are
compressed into a summary:
- **Budget**: `EXO_CTX_BUDGET` bytes per session (default 16384).
- **Summary**: folded entries become `agent:<id>:summary` with a
  `# summary <n> entries (compressed at <epoch>)` header, carried
  `decided:`/`state:` lines, and a `(+N entries compressed)` footer.
- **Re-expansion**: the summary is emitted as a `## summary (compressed
  history)` section in the digest before the live tail.
- **Evidence**: `ctx:summary:<id>` records the last compression epoch.
- **Failure semantics**: compression is best-effort and write-after;
  backend failures leave live keys in place.

## 7. Tests

`make test` covers digest composition (notes + state keys, other agents
excluded), budget capping, POST bodies, error paths (missing agent,
missing exomind), bearer auth, and auto-compression (fold, re-expansion
on resume, re-compression merge, small sessions untouched). The QMS gate
runs `./build/exocontext --version` (in-budget smoke).

## 8. Limitations

- Recency for keys is approximated by `sort=desc` on key name (timestamps
  are not in `/list`), so `agent:<id>:z*` keys may sort before newer
  `agent:<id>:a*` keys.
- Compression folds `agent:<id>:*` keys only; the exomind notes stream
  mentioning the agent is never folded.
- The digest is a flat dump, not a semantic summary.
- One agent namespace per request; cross-agent digests are out of scope.

## 9. Robustness

The daemon is stateless: the digest is computed from exomind on every
request. Malformed requests receive 400 responses, oversized bodies
413, unknown paths 404, and wrong methods 405. Bearer auth (`--token`)
is enforced on all endpoints.
