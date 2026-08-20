# exomind — TODO & roadmap

The stack: exomind (memory) + exosched (scheduler/push) + exoflow
(orchestrator) + exodoc (documentation auditor) + exoqms (universal
quality management) + exocrawl (research). Everything below is tracked in
exomind keys (`todo:*`) as well — the memory is the source of truth.

## Priority 1 — agent-performance multipliers

- [x] **Context continuity module (exocontext)** — when a working session
      grows long, auto-compress the oldest context into an exomind note
      (`ctx:session:<id>`), summarize decisions/state, and re-expand on
      resume. The real bottleneck for long autonomous runs is context
      budget; this turns exomind into a swap file for attention.
      DONE: auto-compression in `exocontext` — folds oldest entries into
      `agent:<id>:summary` (companion key, `ctx:summary:<id>` marker),
      budget `CTX_BUDGET_DEFAULT` 16384 (`EXO_CTX_BUDGET` overrides),
      decided:/state: lines carried forward, `(+N entries compressed)`,
      `## summary (compressed history)` re-expanded on resume;
      51/0 tested (20 new).
- [x] **Scheduler-driven agent freeze detection** (exoqms check
      `agent-health`, criterion c8) — DONE: fired reminder + no activity
      + no `agent:<id>:done` marker → FAIL with timestamps; verified
      live against a ghost agent and the real agent set.
- [x] **exocrawl: scheduled re-research flows** — an exoflow loop
      (`every 6h`) that re-runs `search → fetch → diff-note` for tracked
      topics (`topic:<id>` keys), so stale knowledge self-refreshes.
      DONE: `exoflow/contrib/topic-refresh.flow` (every 6h) +
      `research-loop.sh` driver (ops add/del/phase_search/phase_fetch/
      phase_diffnote); memory `topic:index`/`topic:<id>[:results|
      :snap|:snap.new|:note|:diff]` (snap.new TTL 21600, snap immutable,
      note append-only); diff-note is linewise-delta by hash, `=== date
      ===` blocks, removed lines never reported, identical = silence;
- [x] **exocrawl: norms corpus** — DONE (v1): RFC 2119, RFC 8259, PEP 8,
      WCAG 2.2, NN/g 10 harvested to `norm:*` keys via
      `exocrawl/contrib/fetch-norms.sh`; extend per Priority 4.

## Priority 2 — the universal QMS

- [x] **More language adapters in exoqms-code**: Go (unchecked errors are
      THE Go bug class — `err` ignored), Rust (unwrap/expect audit),
      JavaScript/TypeScript (promise rejection without catch), and a
      Dockerfile/CI yaml hygiene rule set (best-effort, line-based) —
      DONE: `go_scan`/`rust_scan`/`js_scan`/`docker_scan` in
      `exoqms/code/src/linemodes.c` (go-unchecked-err, rust-unwrap,
      js-eval, docker-unpinned ...) + adapter fixtures in
      `exoqms/code/test/test.sh`.
- [x] **QMS check `docs-coverage`** — require every new module to carry
      README + tests + standard.md conformance before merge (partly done
      by exodoc; wire it into the audit program as a hard gate) — DONE:
      the check gates every manifest module (or the repo root) on
      README + `test/test.sh` + `standard.md`/`docs/`, is part of the
      default `audit` program (`/audit?criteria=detect`), and is tested
      in `exoqms/test/test.sh` (145/0).
- [x] **QMS check `agent-health`** — DONE (see Priority 1).
- [x] **Trends: iteration velocity** — track time-to-merge per feature
      and rework rate in `metric:*`; the QMS already computes the trend;
      add a "rework" derived metric (test-failure→fix cycles).
      DONE: `rework` check in `exoqms/src/checks.c` — reopen cycles
      (fail→pass→fail) from audit records; writes
      `metric:velocity:<date>:rework_rate/rework_cycles/rework:<check>`
      and `ttm:<feature>` keys; suite determinism fixed by wiping
      `exoqms:audit:*` before the probe section (audit ids are
      `<epoch>:<rand32>` and `scheduled` is 1s-resolution, so same-second
      audits ordered arbitrarily); 160/0, 3 consecutive runs.
- [x] **Secrets check: allowlist per project** — `.exoqms.json` gains
      `secrets_allow` (pattern:path) so test fixtures with fake keys are
      declared instead of ignored wholesale — DONE: `secrets_allow`
      parsed in `exoqms/src/project.c` (line 139) and applied in the
      `secrets` check (`exoqms/src/checks.c`).

## Priority 3 — stack robustness

- [x] **exocrawl: robots.txt + politeness profiles** — optional
      `--robots` mode consulting robots.txt with crawl-delay; default
      stays research-mode (pace-limited).
      DONE: `--robots [dir]` / `EXO_CRAWL_ROBOTS` (off by default):
      caches `<dir>/<host>.txt` + `<dir>/<host>.pace`, prefix-match
      Disallow (`/` whole site, `*`/`$` stripped), `?polite=0` bypass,
      effective spacing = max(`--pace-ms`, host.pace, Crawl-delay);
      fixed a real `pace_wait` undercount and a 1MB-per-extraction leak
      (ASAN-clean); tested in `exocrawl/test/test.sh` (60/0).
- [x] **exocrawl: HTML extraction regression corpus** — collect the
      pages that fool the boilerplate heuristics (sticky promos, cookie
      banners with unusual classes) as fixtures; extraction quality
      becomes a measured, testable number.
      DONE: `exocrawl/test/fixtures/extract/` (4 truthfully-fooling
      pages + goldfiles: sticky-promo, cookie-banner, paywall-modal,
      newsletter-popup) + `GET /extract-quality?dir=` metric op
      (per-fixture precision/recall/f1, lines=m/e, `fooled=yes` iff
      p or r < 1.0, exit 0/1/2); measured baseline p 0.762 / f1 0.865,
      fooled=4 — quality is now a tested number.
- [x] **exomind: prefix-index for /list and /search** — the hash index is
      exact-key; a sorted key prefix index would make `prefix=` queries
      O(log n + k) instead of full scans at 10k+ keys — DONE: sorted
      candidate index (`src/store.c`) with prefix range walks
      (`list_keys`, `search_keys`) at ~line 492; benchmarked in
      `test/test.sh`.
- [x] **exomind: replication** — follower nodes tail the log for HA; the
      log format already supports it (append-only + CRC).
      DONE: primary `GET /repl?from=<off>` streams raw records
      (base64, `repl from <from> next <next> count <n>`; torn tail →
      500 `repl error torn`); `store_raw_len/raw_at/import_raw/reset`
      accessors; follower `--replicate <host:port>` polls
      `EXO_REPL_POLL_MS` (2s), CRC-validated pwrite+fdatasync import
      (tombstones remove), divergence → `store_reset` + resync;
      `repl:` stats line (role/lag/next/last_sync/errors/resyncs);
      corruption-recovery tested (exomind suite 354 PASS).
- [x] **exosched: delivery receipts** — a fired timer's note should
      record whether any WS client ACKed; expose `/delivery` stats —
      DONE: `delivery.c` tracks per-fire ack/none, fired note carries
      `delivery:ack`/`delivery:none`, `GET /delivery` exposes stats;
      tested in `exosched/test/test.sh` (lines 485+).
- [x] **exoflow: timeout steps** — DONE: 5th column `timeout_s`; claim
      sets deadline = now + timeout; lazy sweep marks overdue; unclaim
      resets the clock; 109/109 tests.

## Priority 4 — norms & knowledge corpus (the scraper's harvest)

Freely-available international norms, extracted by exocrawl into
`norm:<id>` exomind keys (capped), registry at `norm:index`:

- [x] **Programming**: RFC 2119 (MUST/SHOULD — the language of every
      spec), RFC 8259 (JSON), RFC 3986 (URI), PEP 8 / PEP 20 (Python),
      ISO C/C++ core guidelines (isocpp.github.io), ECMA-262 (JS — huge;
      extract TOC + key sections only).
- [x] **Web design**: W3C WCAG 2.2 (accessibility), W3C CSS spec
      (selectors/box model chapters), WHATWG HTML (landmarks/semantics
      sections), Google Material Design principles (page).
- [x] **Digital design**: ISO 9241-11 (usability — summary page), NNG
      heuristics (10 usability heuristics), A11y Project checklist.
- [x] **Documentation**: Diátaxis framework, Microsoft Style Guide
      (summary), RFC 7322 (RFC style — meta but fitting), ISO 9001
      clause 7.5 (documented information — already in exoqms standard).
- [x] Script `exocrawl/contrib/fetch-norms.sh` + an exoflow flow that
      runs it on a schedule and notes the diffs.
      DONE (P4): 19 ids total (12 added: pep20, isocpp, ecma262,
      css-selectors, html-landmarks, material-design, iso9241-11,
      a11y-check, diataxis, ms-style, rfc7322, iso9001-75) harvested to
      `norm:*` (cap MAX_NORM 30000/key; ecma262 deliberately oversized
      as the cap litmus), registry `norm:index`; fixtures +
      `--dry-run <dir>` offline mode; `exoflow/contrib/norms-refresh.flow`
      (`loop every 24h`, harvest step 600s timeout) + `norms-loop.sh`
      driver; note only when ids changed; exocrawl suite 68/0.

## Process

- Every TODO here gets a `todo:*` key in exomind when picked up; closing
  it means the work landed on main with tests and the QMS gate green.
- Long-running agent work must set exosched reminders (`agent:<id>:` in
  the message) — the freeze detector depends on it.
