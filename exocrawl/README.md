# exocrawl

**AI-native web research daemon — token-efficient, private, concurrent.**

The web is built for humans: HTML boilerplate, ads, cookie banners, and
search engines that optimize for sponsored content. exocrawl reduces the
web to what an AI needs — clean plain text, links, and images — with a
SearXNG-style private metasearch layer (no accounts, no cookies, no
tracking) and high-concurrency fetching with per-host pacing and identity
rotation.

## Why it exists

- **Token efficiency** — `/fetch` strips nav/ads/footers/cookie banners;
  headings become `# `, lists `- `, code stays verbatim. A 50 KB HTML page
  becomes a few hundred tokens.
- **Private search** — SearXNG JSON API across rotated instances plus a
  DuckDuckGo HTML fallback; sponsored results are filtered out. No API
  keys, no accounts, no cookies.
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
make exocrawl          # exocrawl/build/exocrawl (zero compile deps)
make test-exocrawl     # hermetic suite (local mock web, 26 checks)
./exocrawl/build/exocrawl --port 7658 --cache exomind
```

Flags: `--port` (7658), `--token`, `--concurrency` (16), `--pace-ms`
(150), `--cache exomind`, `--proxy http://...`.

## API

Self-describing: `GET /` prints the full spec.

| method | path | purpose |
|--------|------|---------|
| GET | `/search?q=...&n=10&engines=searxng,ddg&json=1` | private metasearch → `rank<TAB>title<TAB>url<TAB>snippet` |
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

## Honest limitations

- No JavaScript execution — single-page apps yield their static content only.
- No browser-grade CSS/layout: extraction is content-order based.
- Public SearXNG instances can be rate-limited or down; the engine list is
  rotated and configurable (edit `src/search.c` or extend the flag set).
- `robots.txt` is not consulted by default (research mode); rate limits and
  pacing are the politeness mechanism.

## Tests

`make test-exocrawl` — hermetic: local mock web serves a test page, fake
SearXNG JSON and DDG HTML; asserts extraction (boilerplate removal,
headings, links, images, entities, pre verbatim), /fetch caps, /scrape
concurrency, 403 retry, stats counters, auth. ASAN-clean.
