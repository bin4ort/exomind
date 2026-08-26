# exogit standard v0.1.0 — the code-research standard for the AI-native stack

Normative document. This standard defines the requirements for the `exogit`
daemon — a stateless code-research tool for GitHub and GitLab — covering
its API surface, authentication model, error handling, and test
expectations. Every requirement below is checkable by `exodoc audit`
(doc checks c1–c7, c8) or by the QMS `component-tests` check.

## 1. Scope

This standard applies to every deployment of the `exogit` component of
the exomind stack. `exogit` is a stateless code-research daemon for
GitHub and GitLab. It provides search, file fetch, repo listing, and
design evolution extraction. It has no durable state — it is a
stateless research tool.

## 2. Identification

- **c1 purpose** — this document opens with a top-level heading naming
  `exogit` followed by a non-empty first paragraph describing its
  purpose (code-research for GitHub/GitLab, stateless).
- **c8 version** — the document contains a version token matching `v?X.Y.Z`
  (the first such token is the documented version: `v0.1.0`).

## 3. API surface

| method | path                                    | purpose                              |
|--------|-----------------------------------------|--------------------------------------|
| GET    | /                                       | full spec (self-describing)          |
| GET    | /ping                                   | liveness: `pong`                     |
| GET    | /search?q=<terms>[&lang=<l>][&n=<n>]    | search code on GitHub/GitLab         |
| GET    | /file?url=<url>[&linenos=1][&max=<m>]   | fetch ONE source file                |
| GET    | /repo?repo=<o/r>[&branch=<b>][&path=<p>]| list repository tree                 |
| GET    | /commits?repo=<o/r>&path=<f>&n=<n>      | design evolution (diff-only commits) |

Every endpoint is self-describing at `GET /`. Errors are `error: <reason>`
with appropriate HTTP 4xx/5xx status codes.

## 4. Build and run

Build: `make exogit` from the repo root produces `exogit/exogit`.
Run: `./exogit/exogit --serve --port 7660`.
Console mode: `./exogit/exogit /search?q=exomind`.

## 5. Search endpoint

`GET /search?q=<terms>` searches GitHub/GitLab for code matching the
query terms. With `EXOGIT_TOKEN` set: uses the authenticated API for
structured results (`full_name`, `path`, `html_url`, `score`). Without
authentication: performs best-effort web scraping of search result pages.

Optional parameters: `lang=<c|go|rust|py>` filters by language (default:
all). `n=<1-100>` controls result count (default: 10).

## 6. File fetch endpoint

`GET /file?url=<url>` fetches one source file from a repository. Accepts
GitHub raw URLs, GitHub blob URLs, GitLab blob URLs, and the
`?repo=<o/r>&path=<f>&branch=<b>` parameter form.

Returns line-numbered content truncated at `max` bytes (default 60000).
Add `linenos=0` to drop line numbers. The response is plain text,
token-efficient by default.

## 7. Repository listing endpoint

`GET /repo?repo=<o/r>` lists a repository tree showing file/directory
names and types (f for file, d for directory). Accepts the full-name
form (`owner/repo`) or a full URL. Optional `path=<dir>` lists a
subdirectory. Optional `branch=<branch>` selects a branch (default:
`main` or `master`).

## 8. Design evolution endpoint

`GET /commits?repo=<o/r>&path=<file>` returns the commit history of ONE
file, each commit with its message and the diff STRIPPED to only the
changed lines (context dropped). This is the key feature for understanding
how a feature evolved from 'introduced' to 'working'.

Optional `n=<1-100>` controls commit count (default: 10). Optional
`branch=<branch>` selects a branch (default: `main`). Each entry includes:
short hash, commit message, and diff hunk with only changed lines.

## 9. Authentication

Authentication uses a GitHub or GitLab personal access token (PAT).
Set `EXOGIT_TOKEN` (environment variable) or `--token <pat>` (CLI flag).

Authenticated mode enables:
- Structured `/search` responses with `full_name`, `path`, `html_url`
- Raised rate limits (60/hr → 5000/hr for GitHub)
- Access to private repositories (with appropriate PAT scopes)

The token is stored in memory only, never persisted to disk or logs.

## 10. Tests

`make test-exogit` runs the full suite (6 checks, <1s). The QMS gate
runs `./exogit/exogit --version`.

`bash test/test.sh` runs offline parser self-tests (repo URL parsing,
branch extraction, path extraction, is_gitlab detection) and a network
smoke test (fetching a real file). Coverage includes the console
operation surface: no-args prints the guide, `/search?q=...` runs
in-process without a server, `/file?url=...` fetches and parses,
`/repo?repo=...` lists a tree, `/commits?repo=...` extracts design
evolution, and `--version` works. The parser self-test covers GitHub
and GitLab URL patterns: `https://github.com/owner/repo/blob/branch/path`,
`https://gitlab.com/owner/repo/-/blob/branch/path`, and raw URLs.

## 11. Limitations

- Authentication is token-based only (no OAuth flow); users must manually
  generate PATs from GitHub/GitLab settings.
- Web scraping (unauthenticated `/search`) is rate-limited by remote
  servers and may be unreliable under load.
- No repository comparison, diff-against-base, or branch management
  endpoints — read-only research tool only.
- No issue tracking, PR management, or repository modification
  capabilities.
- Line-numbered output defaults to `linenos=1`; add `linenos=0` to
  drop line numbers for cleaner consumption.

## 12. Robustness

The daemon SHALL never crash on malformed input: oversized query strings
are rejected (400), unknown paths return 404, wrong methods return 405,
and malformed URLs are rejected with `error: invalid URL`. Bearer auth
(`--token` or `EXOGIT_TOKEN`) is enforced on all endpoints when
configured. Timeout handling: network operations have a configurable
timeout (default: 30s per request).

## 13. Error handling

All errors follow the `error: <reason>` format with appropriate HTTP
status codes:
- 400: malformed query parameters, invalid URLs
- 401: missing or invalid authentication token
- 403: rate limit exceeded, private repo without auth
- 404: repository or file not found
- 413: request body too large
- 500: internal server error
- 503: upstream service unavailable (GitHub/GitLab down)

Error messages are plain text, human-readable, and actionable where
possible (e.g., `error: missing EXOGIT_TOKEN — set it for structured search`).
