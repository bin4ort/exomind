# exocrawl v0.4.0-alpha.1

**AI-native web research daemon — token-efficient, private, concurrent.**

The web is built for humans: HTML boilerplate, ads, cookie banners, and
search engines that optimize for sponsored content. exocrawl reduces the
web to what an AI needs — clean plain text, links, and images — with a
SearXNG-style private metasearch layer (no accounts, no cookies, no
tracking) and high-concurrency fetching with per-host pacing and identity
rotation.

Part of the [exomind stack](../README.md) — the main README is the full
stack reference (this file has the complete exocrawl documentation).

## Why it exists

- **Token efficiency** — `/fetch` strips nav/ads/footers/cookie banners;
  headings become `# `, lists `- `, code stays verbatim. A 50 KB HTML page
  becomes a few hundred tokens.
- **Independent private search** — five engines fetched directly and parsed
  by our own adapters: DuckDuckGo HTML, Mojeek, Marginalia, Bing, and the
  Wikipedia opensearch API. No third-party aggregator (no SearXNG), no API
  keys, no accounts, no cookies; sponsored results filtered per engine.
- **Broad scraping** — `/scrape` fetches many URLs concurrently (worker
  pool) with per-host pacing (default 150 ms) and rotating identities;
  403/429 gets bounded retries with a different identity.
- **200% privacy** — stateless requests, no JS, no referrer, no persistent
  state unless you opt into the exomind cache (`--cache http://127.0.0.1:7654`,
  keys `exocrawl:cache:*`, 24 h TTL).
- **TLS via curl** — the one runtime dependency: the ubiquitous `curl`
  binary provides HTTPS transport; everything else is native C.

## Build & run

```sh
make exocrawl          # exocrawl v0.4.0-alpha.1/build/exocrawl (zero compile deps)
make test-exocrawl     # hermetic suite (local mock web, 47 checks)
./exocrawl/build/exocrawl --serve --port 7658 --cache http://127.0.0.1:7654
```

Flags: `--serve` (the only way to run the HTTP server, together with
`--port`), `--token`, `--concurrency` (16), `--pace-ms` (150),
`--cache <url>` (`exomind` alone means `http://127.0.0.1:7654`),
`--robots [dir]` (or env `EXO_CRAWL_ROBOTS`), `--proxy http://...`.

Without `--serve` (and without `--port`) the binary never binds a port:
with `--port` is explicit, with a leading `/` operation it runs that one
operation in-process.

## Console operations

Each API endpoint also runs as a one-shot, in-process console operation —
same routing, same output, no socket, no daemon. `--body <text>` supplies
the request body (or stdin for POST ops, when stdin is not a terminal).

```sh
exocrawl /fetch?url=https://example.com&links=1
exocrawl /stats
printf 'https://a.example/\nhttps://b.example/\t4000\n' | exocrawl /scrape
exocrawl /search?q=llm+evals&n=5 --body ignored       # GET ops take no body
exocrawl /extract-quality?dir=test/fixtures/extract   # extraction quality
```

Exit codes: `0` success, `1` operation failed (e.g. `error: missing url`),
`2` unknown operation. `--extract <file.html>` is a legacy offline
extraction op (no exomind, no network) kept for the extraction regression
corpus; it prints the extracted text of a local HTML file.

No arguments at all prints the guide (the same text `GET /` serves) and
exits 0.

## API

Self-describing: `GET /` prints the full spec (the `/` endpoint itself
is the documentation; this file is the human view).

| method | path | purpose |
|--------|------|---------|
| GET | `/search?q=...&n=10&engines=ddg,mojeek,marginalia,bing,wikipedia\|all&json=1` | independent metasearch → `rank<TAB>title<TAB>url<TAB>snippet` |
| GET | `/fetch?url=...&max=8000&links=1&images=1` | HTML → clean plain text (+ `## links` / `## images` sections) |
| POST | `/scrape` | one URL per line `[TAB max]`, concurrent fetch-all |
| GET | `/stats` | counters (fetches, errors, cache_hits, bytes) |
| GET | `/ping` | `pong` |
| GET | `/extract-quality?dir=<fixtures-dir>` | extraction quality vs `<name>.txt` goldfiles: per-fixture + overall precision/recall/f1 (line-matched), fooled fixtures flagged |

All endpoints answer plain text, lowercase `ok`/`error:` style; `--token`
enforces Bearer auth.

## Robots.txt politeness (`--robots [dir]`, env `EXO_CRAWL_ROBOTS`)

Off by default — research mode is pace-limited only and never consults
robots.txt. With robots mode on, before every `/fetch` and `/scrape`
target the host's robots.txt is loaded from the cache (`<dir>/<host>.txt`,
where `<host>` is the URL authority, port included) or fetched once from
the host (same scheme as the target) and cached there. Enforced:

- **Disallow** — path rules are matched as prefixes (`/` covers the whole
  site, `*`/trailing `$` stripped, UA-agnostic: the broadest restriction
  wins). A disallowed target is skipped with a clear
  `error: robots.txt disallows <rule>` note; `?polite=0` on `/fetch`
  explicitly bypasses the check.
- **Crawl-delay** — `Crawl-delay: N` (seconds, may be fractional) floors
  the per-host request spacing; the robots.txt fetch itself counts into
  that host's budget.
- **Per-host pace override** — `<dir>/<host>.pace` (one integer, ms)
  replaces the global `--pace-ms` for that host (site Crawl-delay still
  floors it).

## Extraction quality measurement

`/extract-quality?dir=<fixtures-dir>` runs the extractor over every
`<name>.html` that has a `<name>.txt` goldfile and prints per-fixture and
overall **precision / recall / f1** on the line level (trimmed, non-empty
lines; `extra` = leaked lines the goldfile does not contain; a fixture is
`fooled=yes` when precision or recall < 1.0). The regression corpus in
`test/fixtures/extract/` holds pages that currently fool the boilerplate
heuristics (sticky promo, consent layer, paywall gate, signup drawer —
all with class names outside the boilerplate keyword list); the numbers
above them are the measured, testable baseline.

## Extraction rules

- Skipped: `script/style/noscript/svg/iframe/form/nav/footer/aside/...` by
  tag; `nav`, `menu`, `sidebar`, `ads`, `advert`, `sponsor`, `cookie`,
  `banner`, `popup`, `modal`, `comment`, `share`, `social`, `related`,
  `recommend`, `subscribe`, `newsletter`, `promo` by class/id.
- HTML entities decoded (named + numeric, incl. UTF-8 output).
- Relative URLs resolved against the page URL.
- Limits: 200 links / 100 images per page; `max` caps the whole output.

## Internals

- **Transport** — the curl binary provides TLS; everything else is
  native C: request building, UA rotation, bounded retries, per-host
  pacing (a worker pool fans out `/scrape`), HTML→text extraction, and
  the per-engine parsers.
- **Cache** — with `--cache http://host:port` (or the shorthand
  `--cache exomind` = `127.0.0.1:7654`), extracted text is stored under
  `exocrawl:cache:*` keys (24 h TTL) and served on repeat fetches.
- **Identity** — stateless requests with rotating user agents; no
  cookies, no referrer, no JS.

## Honest limitations

- No JavaScript execution — single-page apps yield their static content only.
- No browser-grade CSS/layout: extraction is content-order based.
- Engines can be rate-limited or captcha-gated (Bing especially); UA
  rotation and bounded retries mitigate this, and the engine list is
  configurable (edit the table in `src/search.c`).
- `robots.txt` is not consulted by default (research mode); rate limits and
  pacing are the politeness mechanism. Opt into robots.txt politeness with
  `--robots [dir]` (see above).

## Tests

`make test-exocrawl` — hermetic: local mock web serves fixtures for all
five engines plus a test page; asserts extraction (boilerplate removal,
headings, links, images, entities, pre verbatim), every engine's parser,
ad filtering, redirect decoding, /fetch caps, /scrape concurrency, 403
retry, stats counters, auth, robots.txt politeness (disallow skip,
crawl-delay spacing, per-host pace override, default-off), and
/extract-quality numbers over the fooling corpus. ASAN-clean.

## License

GPL-3.0-only — see [LICENSE](../LICENSE).
