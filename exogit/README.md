# exogit v0.1.0 — AI-native code-research daemon (GitHub / GitLab)

A code-research daemon for GitHub and GitLab: search source code, fetch
individual files, list repository trees, and extract design evolution
from commit diffs. It runs with zero external dependencies (C11 + libc
only) and stores no state — it is a stateless research tool that can
serve as a console operation or an HTTP daemon.

Part of the [exomind stack](../README.md) — the main README is the full
stack reference (this file has the complete exogit documentation).

```
make exogit         # builds exogit/exogit (from the repo root)
make test-exogit    # runs exogit/test/test.sh (6 checks, <1s)
```

## Build

`make exogit` from the repo root (or `make -C exogit` directly)
produces `exogit/exogit` — a single C11 binary, zero dependencies
beyond libc + pthread. No JS, no cookies, no accounts required.

## Usage

The same binary has two lives: one-shot **console operations** and the
**HTTP daemon**.

```
./exogit/exogit                     # prints the API guide and exits
./exogit/exogit /search?q=exomind  # one-shot search
./exogit/exogit /file?url=...      # one-shot file fetch
./exogit/exogit /repo?repo=...     # one-shot repo listing
./exogit/exogit /commits?...       # one-shot design evolution extraction
./exogit/exogit --serve --port 7660
```

With no arguments the API guide (the same text the daemon's `GET /`
serves) is printed and nothing is bound. A first argument starting with
`/` is a console operation: it runs in-process through the internal
dispatcher — no server and no socket is ever opened — and prints the
response body. Exit status is `0` on success, `1` on an API error,
`2` on a usage error.

Console operations default to `GET`; the mutating ones are `POST`
(none currently). A `POST` body comes from `--body <text>` when given,
otherwise from stdin (only when stdin is not a tty).

The daemon binds a port **only** when started with `--serve` (or an
explicit `--port`); with neither, no socket is opened:

```
./exogit/exogit --serve --port 7660 [--token <pat>]
./exogit/exogit --serve --host 0.0.0.0 --port 7660
```

`--port` defaults to 7660, `--host` to 127.0.0.1. Set `--token` (or env
`EXOGIT_TOKEN`) to use a GitHub/GitLab personal access token for
authenticated API calls — this raises rate limits and enables structured
`/search` responses. `--help`, `--version` and the `--help <name>`
stack help work as before.

## API

| method | path                                    | purpose                              |
|--------|-----------------------------------------|--------------------------------------|
| GET    | /                                       | full spec (self-describing)          |
| GET    | /ping                                   | liveness: `pong`                     |
| GET    | /search?q=<terms>[&lang=c][&n=10]       | search code on GitHub/GitLab         |
| GET    | /file?url=<url>[&linenos=1][&max=60000] | fetch ONE source file                |
| GET    | /repo?repo=<o/r>[&branch=main][&path=]  | list repository tree                 |
| GET    | /commits?repo=<o/r>&path=<file>         | design evolution (diff-only commits) |

Errors are `error: <reason>` with HTTP 4xx/5xx.

## Endpoints

### /search — Code Search

Find code on GitHub/GitLab. With `EXOGIT_TOKEN` set (GitHub or GitLab
PAT): clean API search returning structured data (`full_name`, `path`,
`html_url`). Without authentication: best-effort web scraping of
`/blob/` links and result snippets.

```
GET /search?q=exomind+memory&lang=c&n=5
```

Narrow the prompt, then read the real implementations. The `lang` filter
defaults to all languages; common values are `c`, `go`, `rust`, `py`.
`n` controls result count (default 10, max 100).

### /file — Source File Fetch

Fetch one source file from a repository. Returns line-numbered content,
truncated at `max` bytes (default 60000, configurable). Token-efficient
by default.

```
GET /file?url=https://github.com/str4y4ok/exomind/blob/main/src/main.c
GET /file?repo=exomind&path=src/main.c&branch=main&linenos=0
```

Accepts GitHub raw URLs, GitHub blob URLs, GitLab blob URLs, and the
`repo/path/branch` parameter form. `linenos=0` drops line numbers.

### /repo — Repository Listing

List a repository tree (or subdirectory) showing file/directory names
and types (f for file, d for directory).

```
GET /repo?repo=exomind&path=src&branch=main
GET /repo?repo=https://github.com/octocat/Hello-World
```

Returns a plain-text tree suitable for agent consumption.

### /commits — Design Evolution Extractor

**THE KEY FEATURE.** Returns the commit history of ONE file, each commit
with its message and the diff STRIPPED to only the changed lines
(context dropped). Lets an agent read how a feature evolved from
'introduced' to 'working' without wading through unchanged code.

```
GET /commits?repo=exomind&path=src/main.c&branch=main&n=10
```

Each commit entry includes:
- Commit hash (short)
- Commit message
- Diff hunk with ONLY the changed lines (no context lines)

This is the most powerful endpoint for understanding design decisions —
see how a function evolved across commits without reading every version.

## Authentication

Set `EXOGIT_TOKEN` (env) or `--token <pat>` (CLI) to use a GitHub or
GitLab personal access token. This:
- Raises API rate limits (60/hr → 5000/hr for GitHub)
- Enables structured `/search` responses with `full_name`, `path`,
  `html_url` fields
- Allows access to private repositories (with appropriate scopes)

No cookies, no JavaScript, no account creation. Token is stored in
memory only, never persisted to disk.

## Console Operations

The binary can run a single API operation and exit — no daemon needed:

```sh
# Search for exomind in C
./exogit/exogit /search?q=exomind+memory&lang=c

# Fetch a file
./exogit/exogit /file?url=https://github.com/str4y4ok/exomind/blob/main/src/main.c

# List a repo tree
./exogit/exogit /repo?repo=exomind&path=src

# Extract design evolution
./exogit/exogit /commits?repo=exomind&path=src/main.c
```

Perfect for scripting, CI pipelines, and one-off research.

## Limitations

- Authentication is token-based (no OAuth flow); users must manually
  generate PATs from GitHub/GitLab settings.
- Web scraping (unauthenticated /search) is rate-limited by the
  remote servers and may be unreliable under load.
- No repository comparison or diff-against-base endpoints.
- No branch creation, PR, or issue management — read-only research tool.
- Line-numbered output defaults to `linenos=1`; add `linenos=0` to
  drop line numbers for cleaner consumption.

## Tests

`make test-exogit` runs the full suite; the QMS gate runs
`./exogit/exogit --version` and `./exogit/test/test.sh`.

`bash test/test.sh` runs offline parser self-tests (repo URL parsing,
branch extraction, path extraction) and a network smoke test (fetching
a real file). Coverage includes the console operation surface: no-args
prints the guide without binding, `/search?q=...` runs in-process
without a server, `/file?url=...` fetches and parses, `/repo?repo=...`
lists a tree, and `--version` works. The parser self-test covers
GitHub and GitLab URL patterns, owner/repo/branch/path extraction, and
is_gitlab detection.

## License

GPL-3.0-only — see [LICENSE](../LICENSE).
