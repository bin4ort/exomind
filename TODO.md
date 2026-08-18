# exomind — TODO & roadmap

The stack: exomind (memory) + exosched (scheduler/push) + exoflow
(orchestrator) + exodoc (documentation auditor) + exoqms (universal
quality management) + exocrawl (research). Everything below is tracked in
exomind keys (`todo:*`) as well — the memory is the source of truth.

## Priority 1 — agent-performance multipliers

- [ ] **Context continuity module (exocontext)** — when a working session
      grows long, auto-compress the oldest context into an exomind note
      (`ctx:session:<id>`), summarize decisions/state, and re-expand on
      resume. The real bottleneck for long autonomous runs is context
      budget; this turns exomind into a swap file for attention.
- [x] **Scheduler-driven agent freeze detection** (exoqms check
      `agent-health`, criterion c8) — DONE: fired reminder + no activity
      + no `agent:<id>:done` marker → FAIL with timestamps; verified
      live against a ghost agent and the real agent set.
- [ ] **exocrawl: scheduled re-research flows** — an exoflow loop
      (`every 6h`) that re-runs `search → fetch → diff-note` for tracked
      topics (`topic:<id>` keys), so stale knowledge self-refreshes.
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
- [ ] **Trends: iteration velocity** — track time-to-merge per feature
      and rework rate in `metric:*`; the QMS already computes the trend;
      add a "rework" derived metric (test-failure→fix cycles).
- [x] **Secrets check: allowlist per project** — `.exoqms.json` gains
      `secrets_allow` (pattern:path) so test fixtures with fake keys are
      declared instead of ignored wholesale — DONE: `secrets_allow`
      parsed in `exoqms/src/project.c` (line 139) and applied in the
      `secrets` check (`exoqms/src/checks.c`).

## Priority 3 — stack robustness

- [ ] **exocrawl: robots.txt + politeness profiles** — optional
      `--robots` mode consulting robots.txt with crawl-delay; default
      stays research-mode (pace-limited).
- [ ] **exocrawl: HTML extraction regression corpus** — collect the
      pages that fool the boilerplate heuristics (sticky promos, cookie
      banners with unusual classes) as fixtures; extraction quality
      becomes a measured, testable number.
- [x] **exomind: prefix-index for /list and /search** — the hash index is
      exact-key; a sorted key prefix index would make `prefix=` queries
      O(log n + k) instead of full scans at 10k+ keys — DONE: sorted
      candidate index (`src/store.c`) with prefix range walks
      (`list_keys`, `search_keys`) at ~line 492; benchmarked in
      `test/test.sh`.
- [ ] **exomind: replication** — follower nodes tail the log for HA; the
      log format already supports it (append-only + CRC).
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

- [ ] **Programming**: RFC 2119 (MUST/SHOULD — the language of every
      spec), RFC 8259 (JSON), RFC 3986 (URI), PEP 8 / PEP 20 (Python),
      ISO C/C++ core guidelines (isocpp.github.io), ECMA-262 (JS — huge;
      extract TOC + key sections only).
- [ ] **Web design**: W3C WCAG 2.2 (accessibility), W3C CSS spec
      (selectors/box model chapters), WHATWG HTML (landmarks/semantics
      sections), Google Material Design principles (page).
- [ ] **Digital design**: ISO 9241-11 (usability — summary page), NNG
      heuristics (10 usability heuristics), A11y Project checklist.
- [ ] **Documentation**: Diátaxis framework, Microsoft Style Guide
      (summary), RFC 7322 (RFC style — meta but fitting), ISO 9001
      clause 7.5 (documented information — already in exoqms standard).
- [ ] Script `exocrawl/contrib/fetch-norms.sh` + an exoflow flow that
      runs it on a schedule and notes the diffs.

## Process

- Every TODO here gets a `todo:*` key in exomind when picked up; closing
  it means the work landed on main with tests and the QMS gate green.
- Long-running agent work must set exosched reminders (`agent:<id>:` in
  the message) — the freeze detector depends on it.
