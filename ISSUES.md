# ISSUES.md — issue log

Every issue below is also recorded in exomind (`issue:<id>` keys) — the
memory is the source of truth; this file is the human-readable view.

Status legend: `open` (active) · `fixed` (resolution landed on main) ·
`mitigated` (workaround in place) · `tracked` (recorded, scheduled).

## Process issues

| id | issue | status | evidence / resolution |
|----|-------|--------|----------------------|
| ISSUE-001 | Agent processes silently freeze mid-task: reminders fire, zero artifacts after hours (b1/b3 iter6, b1 iter7, fixerA iter6) | mitigated | `agent-health` QMS check (c8) now detects frozen agents from fired reminders + missing activity/deliverable; orchestrator redeploys or takes over |
| ISSUE-002 | Shared-worktree branch races: agents commit onto the wrong branch (iter2, iter5, fixer) | mitigated | isolated git clones per agent; pre-commit `git rev-parse --abbrev-ref HEAD` check; observer verifies branch scope |
| ISSUE-003 | `pkill -x`/`pkill -f` by process name killed shared daemons (iter1 B1, iter3 B1) and the tool's own shell | fixed | swarm rule: kill only by port/PID; `[x]name` bracket patterns never matched own shell |
| ISSUE-004 | Test-port collisions between concurrently running builder suites | mitigated | port ranges per suite + env override; stale-daemon cleanup at suite start |
| ISSUE-005 | Build/test commands that hang freeze the whole session | mitigated | every command runs under a hard `timeout`; hung processes are killed, never awaited |
| ISSUE-015 | `/outdate`/`/link` reasons with spaces silently rejected (raw URLs) | fixed | URL-encoding required; spec documents it; reorg used `%20` |
| ISSUE-016 | QMS audit records (`exoqms:audit:*`, `exodoc:audit:*`) pollute `/list` on the main store | tracked | they are the persistent record store (`/audit?id=` reads them) — reserved prefixes documented in `p:exo:memmodel`; long-term: write under `p:` or rotate |

## Software issues

| id | issue | status | evidence / resolution |
|----|-------|--------|----------------------|
| ISSUE-006 | exoqms `json_field` truncated multi-element arrays (only first ignore/test/doc entry parsed) | fixed | NC 1786859171; depth-counted array copy in `exoqms/src/util.c`; config ignores now fully applied |
| ISSUE-007 | exocrawl search double-free (use-after-free on the query string across engine retries) | fixed | ASAN trace `search.c:207`; `free(enc)` moved out of the retry loop; ASAN-clean since |
| ISSUE-008 | exocrawl HTML extraction misses sticky promo/cookie banners with unusual classes | tracked | real-world regression corpus planned (TODO P3); pace-based politeness only |
| ISSUE-009 | Bing/DDG anti-bot (captcha, rate limits) intermittently blocks direct scraping | mitigated | UA rotation, bounded retries, per-engine pacing, engine filter per query |
| ISSUE-010 | exodoc live-audit drifts when deployed binaries lag the repo (0.1.0 daemon answering 0.2.0 docs) | fixed | redeploy on merge; `exodoc audit --live` gate in `make test-exodoc` |
| ISSUE-011 | exoqms `--repo` relative tool paths break child invocations (binaries resolved against the target repo) | fixed | absolute paths required for `--code/--ui/--svg/--exodoc`; documented |
| ISSUE-012 | OCS-Studio: shell injection in `system()`/`popen()` paths, world-writable `/tmp`, unchecked fork/exec child | fixed | commit `2e150f7` (shell_q, ocs_tmp_dir, checked child); universal QMS audit 100% |
| ISSUE-013 | exoflow loop feature shipped without regression tests (claim 51/51 unreproducible) | fixed | 27 regression tests added (fixer close-out); observer verifies claims with exact suite lines |
| ISSUE-014 | Debt markers (`TODO`/`XXX`) scattered in source, untracked | fixed | `debt` check + `debt:*` exomind keys; markers converted to tracked records |

## Open (from TODO.md)

- `todo:exocontext` — context-continuity/compression module (P1)
- `todo:norms-more` — expand the norms corpus (P4): ECMA-262, ISO 9241, RFC 3986, A11y checklist
- `todo:exocrawl-robots` — optional robots.txt politeness profile (P3)
- `todo:exomind-prefix-index` — O(log n) prefix/list queries (P3)
- `todo:qms-go-rust` — Go/Rust/TS adapters for exoqms-code (P2)
