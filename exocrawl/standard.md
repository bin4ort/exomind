# exocrawl standard v0.4.0-alpha.1 — the research standard for the AI-native stack

Normative document. This standard defines the requirements for the
`exocrawl` daemon — AI-native web research with private metasearch,
clean-text extraction, and concurrent scraping — covering its API surface,
extraction model, politeness profile, and test expectations. Every
requirement below is checkable by `exodoc audit` (doc checks c1–c7, c8)
or by the QMS `component-tests` check.

## 1. Scope

This standard applies to every deployment of the `exocrawl` component of
the exomind stack. `exocrawl` provides AI-native web research: independent
private metasearch (five engines), token-efficient HTML-to-text extraction,
concurrent scraping with per-host pacing, and opt-in robots.txt politeness.

## 2. Identification

- **c1 purpose** — this document opens with a top-level heading naming
  `exocrawl` followed by a non-empty first paragraph describing its
  purpose (AI-native web research daemon).
- **c8 version** — the document contains a version token matching `v?X.Y.Z`
  (the first such token: `v0.4.0-alpha.1`).

## 3. API surface

| method | path | purpose |
|--------|------|---------|
| GET | `/search?q=...&n=10&engines=ddg,mojeek,marginalia,bing,wikipedia\|all&json=1` | independent metasearch |
| GET | `/fetch?url=...&max=8000&links=1&images=1` | HTML to clean plain text |
| POST | `/scrape` | concurrent fetch-all (one URL per line) |
| GET | `/stats` | counters (fetches, errors, cache_hits, bytes) |
| GET | `/ping` | liveness: `pong` |
| GET | `/extract-quality?dir=<fixtures-dir>` | extraction quality vs goldfiles |

Every endpoint is self-describing at `GET /`. Errors are `error: <reason>`
with appropriate HTTP 4xx/5xx status codes.

## 4. Build and run

Build: `make exocrawl` from the repo root produces `exocrawl/build/exocrawl`.
Run: `./build/exocrawl --serve --port 7658 --cache http://127.0.0.1:7654`.
Console mode: `./build/exocrawl /fetch?url=https://example.com`.

## 5. Search

`/search` queries five independent search engines directly — DuckDuckGo
HTML, Mojeek, Marginalia, Bing, and the Wikipedia OpenSearch API — and
returns ranked results: `rank<TAB>title<TAB>url<TAB>snippet`. No third-party
aggregator, no API keys, no accounts, no cookies. Sponsored results are
filtered per engine. Engines are configurable via the `engines` query
parameter.

## 6. Extraction

`/fetch` downloads an HTML page and extracts clean plain text:
- Skipped tags: script, style, noscript, svg, iframe, form, nav, footer,
  aside, and elements with class/id matching: nav, menu, sidebar, ads,
  advert, sponsor, cookie, banner, popup, modal, comment, share, social,
  related, recommend, subscribe, newsletter, promo.
- HTML entities are decoded (named + numeric, UTF-8 output).
- Relative URLs are resolved against the page URL.
- Optional `## links` and `## images` sections.
- `max` caps the total output size.
- Code in `<pre>` blocks stays verbatim.

## 7. Scraping

`POST /scrape` accepts one URL per line (optionally tab-separated with
a max size). A worker pool fans out fetches concurrently (default
concurrency 16) with per-host pacing (default 200ms). 403/429 responses
are retried with a different user-agent identity. The answer is
`ok <n>` followed by per-URL results.

## 8. Caching

With `--cache <url>` (or `--cache exomind` = `127.0.0.1:7654`), extracted
text is stored under `exocrawl:cache:*` keys in exomind with a 24h TTL.
Repeat fetches serve the cached version.

## 9. Robots.txt politeness

Off by default (research mode). With `--robots [dir]` or env
`EXO_CRAWL_ROBOTS`: before every fetch the host's robots.txt is loaded
from cache or fetched once and cached. Disallow rules are prefix-matched
(broadest wins). Crawl-delay floors per-host request spacing. A
per-host pace override file (`<dir>/<host>.pace`) replaces the global
`--pace-ms`. `?polite=0` on `/fetch` bypasses the check.

## 10. Extraction quality

`/extract-quality?dir=<fixtures-dir>` runs the extractor over every
`<name>.html` that has a corresponding `<name>.txt` goldfile and prints
per-fixture and overall precision/recall/f1 on the line level. A fixture
is flagged `fooled=yes` when precision or recall < 1.0. The regression
corpus in `test/fixtures/extract/` holds pages that currently fool the
boilerplate heuristics.

## 11. Tests

`make test-exocrawl` — hermetic suite with a local mock web server
serving fixtures for all five engines plus test pages. Coverage includes
extraction (boilerplate removal, headings, links, images, entities, pre
verbatim), every engine's parser, ad filtering, redirect decoding, fetch
caps, scrape concurrency, 403 retry, stats counters, auth, robots.txt
politeness (disallow skip, crawl-delay spacing, per-host pace override,
default-off), and extraction quality numbers over the fooling corpus.
ASAN-clean.

## 12. Limitations

- No JavaScript execution — single-page apps yield static content only.
- No browser-grade CSS/layout: extraction is content-order based.
- Search engines can be rate-limited or captcha-gated; UA rotation and
  bounded retries mitigate this.
- `robots.txt` is not consulted by default; rate limits and pacing are
  the politeness mechanism.

## 13. Robustness

The daemon SHALL never crash on malformed input: missing URLs are rejected
(400 `error: missing url`), bad URL schemes are rejected (400 `error: url
must be http`), oversized bodies are rejected (413), and unknown paths
return 404. Bearer auth (`--token`) is enforced on all endpoints.
