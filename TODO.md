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
- [ ] **Scheduler-driven agent freeze detection** (exoqms check
      `agent-health`): for every agent in `--agents`, compare the
      timestamp of its latest activity note against exosched's fired
      reminders mentioning that agent. Reminder fired + no activity +
      no deliverable (no new `agent:<id>:*` keys, no pushed branch)
      → FAIL with evidence → the orchestrator redeploys the agent or
      does the task itself. This is the "silent freeze" detector.
- [ ] **exocrawl: scheduled re-research flows** — an exoflow loop
      (`every 6h`) that re-runs `search → fetch → diff-note` for tracked
      topics (`topic:<id>` keys), so stale knowledge self-refreshes.
- [ ] **exocrawl: norms corpus** — the international-standards extraction
      below, stored as `norm:*` keys so agents have standards locally
      without fetching them repeatedly.

## Priority 2 — the universal QMS

- [ ] **More language adapters in exoqms-code**: Go (unchecked errors are
      THE Go bug class — `err` ignored), Rust (unwrap/expect audit),
      JavaScript/TypeScript (promise rejection without catch), and a
      Dockerfile/CI yaml hygiene rule set (best-effort, line-based).
- [ ] **QMS check `docs-coverage`** — require every new module to carry
      README + tests + standard.md conformance before merge (partly done
      by exodoc; wire it into the audit program as a hard gate).
- [ ] **QMS check `agent-health`** — see Priority 1; implement with the
      exomind client the daemon already has.
- [ ] **Trends: iteration velocity** — track time-to-merge per feature
      and rework rate in `metric:*`; the QMS already computes the trend;
      add a "rework" derived metric (test-failure→fix cycles).
- [ ] **Secrets check: allowlist per project** — `.exoqms.json` gains
      `secrets_allow` (pattern:path) so test fixtures with fake keys are
      declared instead of ignored wholesale.

## Priority 3 — stack robustness

- [ ] **exocrawl: robots.txt + politeness profiles** — optional
      `--robots` mode consulting robots.txt with crawl-delay; default
      stays research-mode (pace-limited).
- [ ] **exocrawl: HTML extraction regression corpus** — collect the
      pages that fool the boilerplate heuristics (sticky promos, cookie
      banners with unusual classes) as fixtures; extraction quality
      becomes a measured, testable number.
- [ ] **exomind: prefix-index for /list and /search** — the hash index is
      exact-key; a sorted key prefix index would make `prefix=` queries
      O(log n + k) instead of full scans at 10k+ keys.
- [ ] **exomind: replication** — follower nodes tail the log for HA; the
      log format already supports it (append-only + CRC).
- [ ] **exosched: delivery receipts** — a fired timer's note should
      record whether any WS client ACKed; expose `/delivery` stats.
- [ ] **exoflow: timeout steps** — a step with a `timeout <s>` spec that
      exoflow marks overdue if not completed (uses exosched deadlines,
      already half-built).

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
