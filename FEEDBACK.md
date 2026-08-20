# FEEDBACK.md — issues & friction log (from using the stack)

Every friction encountered while using exomind, every idea for a function
that would have saved time/tokens at a specific operation, and every
"if only there was a ..." moment goes here FIRST. Entries that are real
defects or new work items are mirrored into `ISSUES.md` / `TODO.md`.
This file is checked and updated on EVERY completion gate, always last,
before any final answer.

## How to use
- `[idea]` — a missing capability that would save tokens/time
- `[bug]` — wrong behavior observed while using the stack
- `[friction]` — workflow annoyance with a suggested fix
- Each entry: what happened, what would have helped, reference (issue id / caller)

## Journal

### 2026-08-18 — v0.4.0-alpha.1 system-wide switch session
- [friction] `pkill -f 'pattern'` matched the invoking shell itself and
  killed the session (pattern appears in the bash -c cmdline). Fix used:
  `[x]` bracket self-exclusion. Idea: a shared swarm rule doc entry exists
  (ISSUE-003) but tool-agnostic; suggest adding to README tips.
- [bug] makepkg `check()` swallowed suite failures (`>/dev/null`), so a
  build failure looked like a mystery. Fixed: check() logs to test.log and
  prints its tail on failure (committed 7aecb11).
- [bug] Backup filenames were second-resolution → two backups in the same
  second overwrote each other (broke check() under makepkg). Fixed:
  monotonic µs suffix (69d7868).
- [bug] The source tarball self-included the packaging tarballs that were
  tracked in git (1.4MB → 368KB after untrack). Root cause: release
  artifacts in git. Fixed: gitignored + release.sh fail-fast.
- [idea] `release.sh` failure output: after makepkg fails it rm -rf's the
  work dir, losing the log. The tail IS printed now; storing the full log
  at `dist/makepkg.log` would help debugging.
- [idea] `pacman -U` gave only "could not find or read package"; a quick
  `bsdtar -tf` check + `.PKGINFO` read isolates file-vs-alpm faults in
  seconds. A `--selfcheck` flag on exoqms/exomind doing archive+PKGINFO
  validation would future-proof installs.
- [friction] Version headers lived per-module with no single source of
  truth; bumping took 11 files. Idea: `common/exo_version.h` shared
  (single `EXO_VERSION`), modules format their own name.
- [idea] Live-ops memory: before/after states of daemon rewires are not
  recorded anywhere; a `POST /note ctx:op:<ts>` on every maintenance op
  would make `/recall` a true audit trail of infra changes.
- [friction] The exodoc live gate FAILs when binaries and READMEs drift in
  version token; that's good, but the failure message names only the
  module and versions — suggesting the fix (which README line + token to
  change) would save a grep round-trip.
- [idea] systemd user units: `Wants=` + `After=` chain works, but there is
  no per-unit `--help` documentation; `exo-stack` target unit +
  `systemctl --user list-dependencies exo-stack.target` would be the
  one-stop status command.

### Open arena — where ideas are expected to flow (this session)
- [idea] Console-mode `<options>` = exact operations (see task: rework
  module console to accept `/op?args` directly; no `--flags`, no server
  routing unless the server binary is explicitly launched).
- [idea] A single server entry point (exomind-server) routing to all
  modules, with `GET /` of each module surface being an agent-facing
  guide page.
- [bug] 3 exoqms detection-registry suite tests fail (issue:<check>
  records missing on QMS_A under the suite's stub environment) — the
  manual 7688 demo worked; suite stub interplay unverified.
### 2026-08-18 — console rework wave (all 11 modules), agents' logs merged
- [idea] DELETE /flow unexpressible via console op map (no action=delete
  in route()); an `action=delete` alias on the daemon's /flow would close
  the gap (exoflow).
- [friction] console output is wire-faithful (no trailing newline) which
  glues to the shell prompt; considered appending \n in console mode only
  — REJECTED to keep suite assertions byte-exact (exoflow).
- [friction] one-shot runs print chatty "no flows to reload"/reload lines
  to stderr; scripting-friendly = silence unless --log-level debug
  (exoflow).
- [friction] --port N implies server mode; a stray --port in a wrapper
  script silently turns an op into a daemon. Worth a stderr hint when a
  console op AND --port/--serve are both given (exoflow + exoqms noted).
- [idea] byte-assert no-args guide == curl GET / text in suites to pin
  "guide = spec page" forever (exoflow).
- [bug] exoqms qms_reload truncated reloaded audit records at the first
  tab (parse_record(v,f,8)): after daemon restart every audit lost
  check/result/evidence. Fixed via parse_audit_record() in model.c
  (exoqms). NOW ISSUE-017 (fixed).
- [friction] piped console bodies carry a trailing newline into the last
  TSV field (target becomes "5\n"); exoqms now strips the stdin line
  terminator. exomind aligned in this session (crawl-context + exoqms).
- [friction] `exoqms /audit?criteria=detect` 400s: "detect" is not a
  known check id. Documented real ids; consider aliasing "detect" to the
  standard program (user-facing example in the design brief).
- [idea] exit-code split 0 ok / 1 op failed / 2 unknown op lets scripts
  tell a typo from a failure — adopted stack-wide (exomind aligned from
  0/1 to 0/1/2).
- [friction] behavior change: bare `exo<module>` no longer serves (guide
  instead); anything started without --serve/--port silently stops
  serving after upgrade — a packaging/upgrade note is needed.
- [bug] exocrawl `--cache <arg>` was parsed but hard-coded to
  127.0.0.1:7654; now parses host[:port]/URL (fixed by crawl agent).
- [bug] exocrawl/exocontext GET / spec strings pinned v0.1.0 while the
  version header says v0.4.0-alpha.1 (fixed in this session; sweeps
  found zero remaining stale version literals in sources).
- [friction] exocrawl handler signature churn (http_out->dout_t) when
  extracting dispatch; a stack-wide http_dispatch-
  (method,path,query,body,out,status,ctype) signature would make future
  modules drop-in.
- [friction] the /exo<module> prefix-strip now exists twice per module
  (console + server); a shared strip helper in common/exo.h would remove
  the duplication stack-wide.
- [idea] network console ops (/search /fetch) hit the live web by
  design; a --offline flag or default fail-on-no-engine-base would make
  suites hermetic without a mock (crawl).
- [bug] exodoc/exokit/exoqms-ui(exoqms-svg) test.sh pinned stale
  v0.1.0 versions — four suites FAILED at baseline; tests re-pinned to
  the source header version. NOW ISSUE-018 (fixed). Lesson: version pins
  must come from the source (--version output), never literals.
- [friction] absolute file paths start with `/` and are indistinguishable
  from ops (batch tools); is_op() disambiguates (known op name or
  contains `?`) — surprising for typos like `/chek`.
- [friction] op query values are literal (& separates, no URL decoding);
  `&`/spaces in file paths unexpressible — %26/encoding for batch path
  params worth adding.
- [friction] batch op forms silently ignore trailing argv
  (`exodoc /audit --json` runs a plain audit); exit 2 on unexpected
  trailing args would catch typos.
- [idea] batch ops synthesize the subcommand argv (delegation) so exit
  codes match the subcommand by construction; exodoc refactored to a
  shared audit_run() — keep this pattern.

## Completion waves — merged feedback (console rework → P4)

- [lesson] new endpoints must ship with their README "## API" row in
  the same commit: `GET /repl` (replication) was added to the source
  but documented nowhere, so the doc gate failed live-vs-doc
  (`live-only GET /repl`) until the row landed. The gate caught it
  immediately — that's the gate working.
- [lesson] console-op invocation order matters: flags cannot precede
  the `/op` argument (`exomind --data f /op` → "unknown argument");
  the op is argv[1] by contract, flags qualify it. Probe scripts must
  follow suite convention (op first).
- [lesson] exocontext console ops need `--exomind URL` — session state
  lives in the memory backend, not locally; a bare `/context` probe
  without the flag is a usage error, not a bug.
- [lesson] never let test helpers default to a live store (ISSUE-020):
  a dry-run arg binding bug made the norms suite write 12 fixture keys
  into the main 7654 memory. Defaults must be inert or private.
- [lesson] agent outputs are reviewed, not trusted: two of the
  wave-2 agents returned empty result texts (the work was verified
  present via git + suite evidence instead: e.g. the exoqms-code
  adapters already existed at commit 30f1b3d and needed nothing).
- [lesson] deterministic suites need deterministic inputs: audit ids
  (`<epoch>:<rand32>`) + 1s-resolution `scheduled` made exoqms
  rework/velocity tests order-dependent; suites now wipe
  `exoqms:audit:*` before probe sections and use fresh private stores.
- [lesson] needle-based assertions must quote against the real blob:
  router test failures were cross-matches (`"text":"pong"` matched the
  wrong op id) or naive (`"exokit: kit initialized"` cut inside a
  longer echoed sentence). Reproduce the exact bytes before fixing.
- [lesson] ASAN is the leak oracle: exocrawl pace_wait undercount and a
  1MB-per-extraction leak only surfaced under sanitizers during the
  robots/extract-quality work (60/0 ASAN-clean since).
- [friction] daemons write setup chatter to stderr even for console
  ops (`reloaded timer ...` under exosched /delivery; `loaded N entries
  from <file>` under exomind console ops) — consumers must read
  stdout/stderr separately, and suites match stdout.
- [friction] `sudo`/passwordless root was unavailable mid-project, so
  the deploy used `make install PREFIX=$HOME/.local` + repointed
  systemd ExecStart; the root `/usr/bin` copies and `packaging/`/
  `dist/` are stale-but-harmless artifacts of the pre-rework build.
- [idea] replication peers could log authoritative divergences to a
  `repl:*` note on both sides instead of stderr-only.
- [idea] robots pace-merging (max of --pace-ms/robots-delay/per-host
  pace) proved itself on mixed hosts; a `robots:index` summary key
  would make site policy visible at `/list`.
