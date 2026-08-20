# exoflow contrib tools

## tracked-topic refresh loop (research-loop)

Stale knowledge self-refreshes: an exoflow loop runs `every 6h` and each
iteration re-runs `search -> fetch -> diffnote` for every tracked topic,
so what the swarm knows about a topic tracks what the web says about it.
Zero exocrawl changes: topics are tracked in exomind keys (the memory is
the source of truth) and exocrawl is used read-only through `/search` and
`/fetch`.

### the flow

`exoflow/contrib/topic-refresh.flow` is the loop definition (POST it as
the body of `POST /flow`; step 5th-column = claim timeout in seconds):

```
search    search every tracked topic (topic:index); store topic:<id>:results   timeout 600
fetch     fetch top results per topic; build topic:<id>:snap.new     (dep: search, timeout 1800)
diffnote  diff snap.new vs topic:<id>:snap; append delta to topic:<id>:note    (dep: fetch, timeout 120)
loop      every 6h
```

The 6h interval is the production cadence; for a test or demo, post the
file with `every 6h` replaced by `every 1s` and a `max N` cap. The loop is
lazy like every exoflow loop: iterations spawn on reads (/next, /flows,
/loops) once the newest record is terminal and `next_run` is due.

### the worker

`exoflow/contrib/research-loop.sh` drives the loop exactly like
`worker.sh` simulates: claim via `GET /next`, execute the phase named by
the step id, report `POST /step done|failed`. It also maintains the
tracked-topic registry:

```
research-loop.sh -m http://127.0.0.1:7654 add llmmon 'latest LLM research' 10
research-loop.sh -m http://127.0.0.1:7654 list
research-loop.sh -m http://127.0.0.1:7654 del llmmon
research-loop.sh -m ... -X http://127.0.0.1:7658 search|fetch|diffnote
research-loop.sh -u http://127.0.0.1:7676 -f <flow-id> -w research-agent \
                 -m ... -X ...            # drive the loop to exhaustion
```

### memory layout (exomind keys)

| key                    | meaning                                              |
|------------------------|------------------------------------------------------|
| `topic:index`          | space-separated registry of tracked topic ids        |
| `topic:<id>`           | definition: `query<TAB><q>` (required), `n<TAB><n>`, repeated `url<TAB><u>` |
| `topic:<id>:results`   | transient (TTL 6h): `url<TAB>title<TAB>snippet` from /search |
| `topic:<id>:snap.new`  | transient (TTL 6h): fresh knowledge snapshot         |
| `topic:<id>:snap`      | last snapshot the note was diffed against            |
| `topic:<id>:note`      | append-only delta log, one `=== <iso ts> ===` block per refresh |

### diff-note semantics

The diffnote step compares the new snapshot against `topic:<id>:snap` (the
previous note's content source) line by line and appends only the lines
that are new or changed, prefixed by a `=== <iso ts> ===` block header.
Removed lines are not reported (the promoted snapshot is the truth);
snapshot metadata (`# topic:`, `# query:`, `# refreshed:`) and blank
lines are structural and never enter the note, so a changing refresh
timestamp can never create a phantom delta. A refresh that changes
nothing appends nothing - **silence is information**: if the whole world
comes back byte-identical, the note stays byte-identical, and `GET
/notes` vs the note key tells any agent when the last real change was.

Notes are capped at `NOTECAP` (default 256 KiB); the oldest timestamped
blocks are trimmed when the cap is hit. Snapshots are capped at `MAXSNAP`
(default 64 KiB), and at most `MAXFETCH` (default 5) results per topic are
fetched per iteration. Topics with explicit `url:` lines fetch exactly
those instead of the search results.

Because the note is only appended to, an agent reading `topic:<id>:note`
plus `topic:<id>:snap` can reconstruct the deltas and the current truth.
Do not run the refresh loop's exocrawl with `--cache`: `/fetch` caching in
exomind (TTL 86400) would freeze pages for a day and defeat re-research.

### running it

Cron, a systemd timer, or an exosched reminder every ~6h running:

```
research-loop.sh -u http://127.0.0.1:7676 -f <flow-id> -w research-agent \
                 -m http://127.0.0.1:7654 -X http://127.0.0.1:7658
```

is enough: `/next` rejects nothing when it is early, and the lazy loop
only executes when the newest iteration is terminal and due. Step claim
timeouts (600/1800/120 s) mean a stuck worker makes its step overdue and
a later run can `failed` it so the next 6h iteration starts fresh.

### tests

`exoflow/test/test.sh` covers the registry ops, the diff-note logic
(delta only, promotion, silence), and a fast-forwarded loop (`every 1s`,
`max 5`) driven end-to-end against the suite's private exomind + exoflow
with `test/mock_research.py` standing in for exocrawl. `exocrawl`'s own
suite is untouched: the refresh worker consumes its public console
contract only.

## norms harvest loop (norms-loop)

The norms corpus (`norm:<id>` keys, registry `norm:index`) is harvested
by `exocrawl/contrib/fetch-norms.sh` — 19 freely-available international
norms capped at 30 KB each. `exoflow/contrib/norms-refresh.flow` is a
`every 24h` loop that re-runs the harvest so the corpus tracks the
sources; `exoflow/contrib/norms-loop.sh` drives it:

```
norms-loop.sh -u http://127.0.0.1:7676 -f <flow-id> -w norms-agent \
              -m http://127.0.0.1:7654 -X http://127.0.0.1:7658
```

The driver claims the `harvest` step via `/next` and appends a
`=== <ts> ===` block to `norm:refresh:note` only when at least one norm
was (re)fetched — silence is information: an unchanged corpus produces
no note growth. Offline mode: `-R <fixtures-dir>` runs the harvest with
`--dry-run` against fixture files (`exocrawl/test/fixtures/norms/`), so
the loop is exercisable without the network; the dry-run path is
byte-identical to a real run except the fetch leg.

`exocrawl/test/test.sh` covers the dry-run mechanics: 19-id registry,
cap, idempotence, soft failure on a missing fixture.