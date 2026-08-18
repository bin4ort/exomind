# exoqms standard v0.1.0 — the quality management standard for the AI-native stack

Normative document. In the spirit of the ISO 9000 family — ISO 9000:2015
(concepts and vocabulary), ISO 9001:2015 (requirements), ISO 9004:2018
(sustained success) and ISO 19011:2018 (audit programs) — this standard
defines what quality means for the exomind stack and how it is measured,
recorded and improved. Every clause below is implemented by the `exoqms`
daemon; the audit-program checks in section 5 are machine-executable and
their results are durable records in exomind.

## 1. Scope

This standard applies to every component of the exomind stack (exomind,
exosched, exoflow, exodoc, exoqms and any future component) and to every
agent that builds, operates or audits them. Conformance is determined by
the checks of the audit program (section 5); the results of each
audit are stored under `exoqms:audit:*` and published to the note feed.
The standard is normative: "SHALL" is a requirement, "SHOULD" a
recommendation, "MAY" a permission, in the vocabulary of ISO 9000 3.6.

## 2. Quality policy statement (ISO 9001 §5.2)

The exomind stack SHALL deliver components that are **machine-auditable
and self-describing**: every daemon exposes a spec at `GET /`, every
component ships a README conforming to the exodoc standard, and every
claim is backed by a running check. Quality is not a review stage at the
end; it is a continuous property: objectives are set per measurement
period, conformity is monitored by the metrics check, non-conformities
are logged with severity and root-cause analysis, and corrective actions
must be evidenced before closure. The organization (the swarm) commits
to: (1) raising, not hiding, non-conformities; (2) closing every
non-conformity with evidence; (3) never regressing the
`metric:iterN:tests_passing` trend between consecutive periods; and
(4) treating audit findings as the primary input to the next period's
objectives.

## 3. Quality objectives (ISO 9001 §6.2)

3.1 An objective SHALL have a description, a `metric_key` (a key
readable from exomind, e.g. `metric:iter5:tests_passing`), a `target`
(numeric or string) and a `period` (default `iter`). It is created with
`POST /objectives` and stored under `exoqms:obj:<id>`.

3.2 An objective is **met** when the current value of its metric key is
available: numeric targets are met when `value >= target`, string
targets when `value == target`. A missing metric key yields **no-data**;
the objective counts as not met for reporting purposes and the gap
SHALL be investigated (ISO 9001 §9.1.3 analysis and evaluation).

3.3 Objectives are established at the start of each measurement period
and evaluated in `GET /report`; the consolidated picture is the one-shot
quality status for agents and humans.

## 4. Monitoring and measurement (ISO 9001 §9.1, ISO 9004 sustained success)

4.1 The primary measured indicator is the tests-passing count per
iteration, read from exomind keys `metric:iterN:tests_passing`. `GET
/trends` lists the parsed values oldest to newest plus the verdict line
`trend up|flat|down`.

4.2 The trend between the two most recent iterations is the *sustained
success* indicator (ISO 9004 clause 9 — monitoring of sustained success):
**up** when the newest value exceeds the previous, **flat** on equality,
**down** on decline. Any verdict other than `up` sets the **stagnation
flag**. A `down` trend is a non-conformity trigger (clause 6.3) and
fails the `metrics` check.

4.3 Measurement periods are the stack iterations (`iter1`, `iter2`,
...); every period SHALL publish `metric:iterN:tests_passing` before its
closing audit. The dogfood check also measures swarm activity (clause
5.4) as a secondary indicator.

## 5. Audit program (ISO 19011:2018)

5.1 An audit program is created with `POST /audit` (body
`name<TAB>criteria`, criteria = comma-separated check ids, empty = the
full standard program below — `detect` is an alias for the same
program) and stored under `exoqms:audit:<id>`. Each audit records: the
criteria executed, the scheduled (run) time, one finding per check
(check id, result `pass|fail|skip`, evidence line) and a score equal to
`100 * pass / (pass + fail)` rounded to the nearest integer, where
`skip` does not count. The audit's note (`exoqms audit ... (score N%)`)
is written to the note feed.

5.2 Every check SHALL complete within 5 seconds. Checks that spawn child
processes (component tests, exodoc, exoqms-ui, exoqms-code, exoqms-svg)
run under a hard timeout; a child that overruns is SIGKILLed and the
check fails with evidence `timed out`. The audit program therefore never
hangs the daemon.

5.3 The criteria checklist — the normative criteria set — consists of:

| check | id | criterion (passes when) | implementation |
|-------|----|-------------------------|----------------|
| c1 | `component-tests` | every component that declares a `test_cmd` (5th column of `docs/stack.tsv`) runs it and exits 0 | child process `sh -c <test_cmd>` with cwd = component dir, 5s timeout, exit status read |
| c2 | `doc-compliance` | `exodoc audit --live` reports 0 fails | child process `<exodoc> audit --live --stack docs/stack.tsv --base <repo> --exomind <url>`; summary line `=== audit: N pass, M fail (score S%) ===` parsed |
| c3 | `dogfood` | every active agent id has `agent:<id>:status`, and >= N notes exist in the last 24h | exomind `GET /get` per agent; `GET /notes?limit=200` with 24h timestamps; N from config `exoqms:config:notes24h` (default 5); scheduler health is reported as evidence only |
| c4 | `ui-audit` | `exoqms-ui` finds 0 findings on the audit target | child process `<ui> <target> --json`; exit 0 = pass, exit 1 = findings (counted), timeout = fail; skipped without `--ui` or `?target=` |
| c5 | `metrics` | the `metric:iterN:tests_passing` trend between the two most recent iterations is not `down` | exomind `GET /list?prefix=metric:iter` filtered to `:tests_passing`, sorted by iteration, parsed numerically; `down` fails, `flat` passes with the stagnation flag set |
| c6 | `code-safety` | the stack's own C source contains 0 **major** error-handling defects (unchecked returns of critical libc calls, missing error paths, null-deref paths); minor findings are reported but non-fatal | child process `<code> <srcdirs...> --json`; default srcdirs = the manifest dirs (column 2 of `docs/stack.tsv`) resolved against the repo root — the stack audits its own code; `?target=<dir>` overrides; the JSON findings array is parsed for severities; pass iff 0 major |
| c7 | `asset-logic` | the stack's own SVG assets pass the shape rule-set with 0 **major** findings (minor findings reported but non-fatal) | child process `<svg> <target> --shape auto --json`; target = audit `?target=` or the repo root; the JSON findings array is parsed for severities; pass iff 0 major |
| c8 | `agent-health` | no agent is silently frozen: every configured agent whose exosched reminder has fired has either activity notes AFTER the fire or an `agent:<id>:done` deliverable marker | exomind `GET /notes?limit=500&q=fired` (fired-timer notes, agent ids extracted from `agent:<id>` tokens) vs `GET /notes?limit=100&q=agent%3A<id>` (agent activity, fired notes excluded) + `GET /get agent:<id>:done`; fail lists the frozen agents with both timestamps — the orchestrator then redeploys the agent or takes the task over |
| c9 | `debt` | `debt-*` findings from the rules scan do not exceed the `thresholds.debt` from `.exoqms.json` (default 10) | the shared `exoqms-code <repo> --rules <dir> --json` scan, findings partitioned by check-id prefix `debt-*`; `--code` and rule files required, else `skip` |
| c10 | `hygiene` | 0 `hygiene-*` findings from the rules scan | same shared rules scan as c9, partitioned by `hygiene-*` |
| c11 | `secrets` | 0 `secrets-*` findings from the rules scan | same shared rules scan, partitioned by `secrets-*`; matched lines are masked to `***` in the evidence; path substrings from `.exoqms.json` `secrets_allow` exclude findings |
| c12 | `docs-coverage` | every module listed in `docs/stack.tsv` ships a README, a runnable test suite at `test/test.sh` and a standards reference (`standard.md` or a `docs/` directory) in its module dir | filesystem checks per manifest row (`name<TAB>dir`); without a manifest the repo root itself is the module (universal mode); fail names each missing file per module |
| c13 | `kit-fidelity` | when the repo carries a `kit/`, the contract ledger audits green (every contract entry exemplified and verified) | child process `<kit> audit --kit <repo>/kit --json`; exit 0 = pass, major findings = fail; skipped without `--kit` or without a `kit/` directory |
| c14 | `memory-awareness` | when exomind carries a `mandate` key, every configured agent has acknowledged it via `agent:<id>:ready` | exomind `GET /get mandate` + `GET /get agent:<id>:ready` per agent; skipped when no mandate is set; fail lists the agents that have not acknowledged |
| c15 | `issue-tracking` | no issue in the detection registry is recurring: every `issue:<check>` record with status `open` must have `consec` < 2 | exomind `GET /list?prefix=issue:` then `GET /get` per key; the registry is written by every audit finding (`issue:<check>` upserted per audit); fail lists `RECURRING <check>` entries with their counters |

5.4 Severity rule for the field-module checks (c4–c7 except c6's
variant): the modules grade each finding `major` or `minor`. A check
**passes iff it reports 0 major findings**; minor findings are recorded
in the evidence line but do not fail the check. Rationale: majors are
conformance breaches (broken UI, unsafe C code, broken asset logic);
minors are improvement candidates that SHALL be tracked but must not
gate the audit program (ISO 9001 §10.2 treats them as opportunities).

5.5 Agent id list for c3: the list given as the third body field of
`POST /audit` if present, else the config value `exoqms:config:agents`
(overridable with `--agents` at startup; the swarm default is
`a,b,b1,b2,b3`).

5.6 Auditor competence (ISO 19011 7.2): the auditor is the daemon
itself — deterministic, reproducible, and incapable of favoritism. Audit
results are tamper-evident only in the sense that they are durable
records in exomind with the note feed as the audit trail.

## 6. Non-conformities and corrective action (ISO 9001 §8.7, §10.2)

6.1 A non-conformity (NC) SHALL have: title, description, severity
(`major` = breach of a normative clause or regression; `minor` = gap
without direct consequence), source (the audit or check that found it,
or `api` for manual entry), detection time, and a lifecycle status. It
is created with `POST /nc` and stored under `exoqms:nc:<id>` with id of
form `<epoch>:<hex>`.

6.2 Lifecycle — statuses and allowed transitions:

    open --analyse--> analysis --correct--> corrective --verify--> verify --close--> closed

Every transition is a `POST /nc?id=<id>&action=<action>` and is recorded
as a note in the exomind feed (`nc <id> transition: <from> -> <to>
(<body>)`) — the audit trail SHALL be complete for every status change.
A transition to a state not reachable from the current one is rejected
with 400 and an error naming the expected status, e.g.
`error: invalid transition: analyse from corrective (expected open)`.

6.3 Closure criteria (ISO 9001 §10.2.2): an NC may be closed from any
status when the request body carries `corrective_action<TAB>evidence`
(a third field is appended as a note, a fourth records `closed_by`,
default `api`). From `verify` the body may be only a note. Closure sets
`status=closed`, `closed_at` and `closed_by`. Closing SHALL NOT be
possible without recorded corrective action and evidence unless the
corrective phase (`verify`) has already been completed — the simple rule
is: **any status can close, but only with corrective_action and
evidence**. A closed NC is excluded from the `open_ncs` count in
`GET /report`.

## 7. Reports and records

7.1 `GET /report` consolidates the quality picture: per-objective status
(met/not/no-data), the objectives summary, the count of open NCs, the
most recent audit (id, score, status) and the trend verdict with the
stagnation flag.

7.2 Durable records: `exoqms:obj:*` (objectives), `exoqms:nc:*`
(non-conformities with full lifecycle fields), `exoqms:audit:*` (audit
programs: id, name, criteria, run time, status, score, findings with
evidence), plus config keys `exoqms:config:agents` and
`exoqms:config:notes24h`. The note feed holds every milestone:
objective creation, NC creation, every NC transition, every audit score.

7.3 Durability: exoqms keeps no local state. On startup it reloads all
`exoqms:*` keys from exomind; if exomind is down, the reload is retried
every second in the background and the daemon serves requests with an
empty registry meanwhile. Records written while exomind is down are
rejected (500 `error: exomind unavailable`) rather than lost silently.

## 8. Robustness and security

8.1 The daemon SHALL never crash: malformed requests receive 400
`error:` responses, oversized bodies 413, unknown paths 404, wrong
methods 405. All inputs are length-bounded; all stored values are
tab-escaped and JSON-escaped as appropriate.

8.2 When started with `--token` (or `EXOQMS_TOKEN`), every request SHALL
carry `Authorization: Bearer <token>`; missing or wrong tokens receive
401 `error: unauthorized`.

## 9. Conformance to this standard

A component of the stack is **QMS-conformant** when, at its closing
audit, the `metrics` check passes (trend not down), `doc-compliance`
passes (0 fails), and every open non-conformity raised against it is
either closed with evidence or tracked with an active corrective
action. The `GET /report` output is the summary of that conformance
state at any moment.
