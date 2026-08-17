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
  state unless you opt into the exomind cache (`--cache exomind`, keys
  `exocrawl:cache:*`, 24 h TTL).
- **TLS via curl** — the one runtime dependency: the ubiquitous `curl`
  binary provides HTTPS transport; everything else is native C.

## Build & run

```sh
make exocrawl          # exocrawl v0.4.0-alpha.1/build/exocrawl (zero compile deps)
make test-exocrawl     # hermetic suite (local mock web, 26 checks)
./exocrawl/build/exocrawl --port 7658 --cache exomind
```

Flags: `--port` (7658), `--token`, `--concurrency` (16), `--pace-ms`
(150), `--cache exomind`, `--proxy http://...`.

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

All endpoints answer plain text, lowercase `ok`/`error:` style; `--token`
enforces Bearer auth.

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
- **Cache** — with `--cache exomind`, extracted text is stored under
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
  pacing are the politeness mechanism.

## Tests

`make test-exocrawl` — hermetic: local mock web serves fixtures for all
five engines plus a test page; asserts extraction (boilerplate removal,
headings, links, images, entities, pre verbatim), every engine's parser,
ad filtering, redirect decoding, /fetch caps, /scrape concurrency, 403
retry, stats counters, auth. ASAN-clean.

## License

GPL-3.0-only — see [LICENSE](../LICENSE).
