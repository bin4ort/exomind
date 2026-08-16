# exoflow — the orchestrator for agent swarms

`exoflow` v0.2.0 is a dependency-graph task orchestrator for AI-agent swarms. A flow
is a DAG of steps; an arbitrary number of agents pull work from it with
`GET /next`, execute, and report back with `POST /step`. exoflow guarantees
that every step is claimed by exactly one worker and only becomes runnable
once all of its dependencies are done. Durable state lives in
[exomind](https://github.com/bin4ort/exomind) (external long-term memory);
step deadlines are enforced through [exosched](https://github.com/bin4ort/exomind)
scheduled reminders; every claim, completion and deadline is audited as an
exomind note. Since 0.2.0 a flow can be a **loop**: when its last
iteration reaches a terminal state, exoflow lazily spawns the next one
(`iter <n+1>`), repeating with a fixed interval up to `max` / `until`
limits.

```
exomind (state)   <-+-  exoflow   <-+-- GET /next?flow=F&worker=W   (workers)
exosched (alarms) <-+      |       +-- POST /step?flow=F&id=s done
                      (audit notes into exomind)
```

## quickstart

Build (from the repo root; the root Makefile wires the sub-project in):

```
make exomind      # build the state backend
make exosched     # build the alarm backend
make exoflow      # build the orchestrator
```

Run the trio (each daemon answers plain-text on its port):

```
build/exomind --port 7654 --data /tmp/xm.dat &
exosched/build/exosched --port 7655 --exomind http://127.0.0.1:7654 &
exoflow/build/exoflow --port 7676 --exomind http://127.0.0.1:7654 \
    --exosched http://127.0.0.1:7655 &
```

(shared instances already run on 7654/7655 in the exomind swarm — use your
own ports for testing, e.g. 7674/7675/7676.)

Check it is alive — `GET /` is self-describing:

```
curl -s localhost:7676/
```

## a full diamond flow, by hand

Create a 5-step diamond where `s4` joins `s2` and `s3`, with a deadline on
`s4` (`now + 120s`):

```
curl -s -X POST localhost:7676/flow --data-binary "ship
s1<TAB>build<TAB>
s2<TAB>test<TAB>s1
s3<TAB>lint<TAB>s1
s4<TAB>package<TAB>s2,s3<TAB>$(date +%s --date '+120s')
s5<TAB>publish<TAB>s4"
# -> ok <flow-id> 5
```

Then drive it with the contrib worker (see below):

```
exoflow/contrib/worker.sh -u http://127.0.0.1:7676 -f <flow-id> -w w1
```

...or claim and finish steps by hand:

```
curl -s "localhost:7676/next?flow=<flow-id>&worker=cli"   # ok s1  (auto-claims)
curl -s -X POST "localhost:7676/step?flow=<flow-id>&id=s1" -d "done cli"
curl -s "localhost:7676/flow?id=<flow-id>"                # TSV state
curl -s localhost:7676/flows                              # all flows
```

## the worker loop (`exoflow/contrib/worker.sh`)

A simulated agent that runs the orchestration loop against any compliant
exoflow: repeatedly `GET /next?flow=F&worker=ME` (which auto-claims one
runnable step), "executes" it (a short sleep; if the step description starts
with `fail:` the step is failed on purpose), then reports back with
`POST /step`. `none` means no runnable work remains and the worker exits 0.

```
worker.sh -u URL -f FLOW -w NAME [-m MAX] [-s SLEEP] [-r PARK] [-e EXOMIND] [-q]
  -u URL      exoflow base URL (required)
  -f FLOW     flow id (required)
  -w NAME     worker name, included in audit notes (required)
  -m MAX      safety cap on claimed steps (default 64)
  -s SLEEP    simulated work seconds per step (default 0.5)
  -r PARK     on `none`, re-poll up to PARK times (default 1 = exit 0
              immediately, per the loop spec). Parking keeps a worker
              available so multi-worker runs genuinely contend for steps
  -e EXOMIND  optional exomind URL; every claim/step is also appended as a
              timestamped note "FLOW|STEP|ACT|WORKER" (| separators —
              exomind escapes control characters, so tabs would not round
              trip) so that ordering can be verified from note timestamps
  -q          quiet mode

exit: 0 = no work left, 1 = protocol/driver error, 2 = step failed
```

Every action is logged on stdout as `worker <name>: claimed s2` /
`worker <name>: done s2` / `worker <name>: no work left`. Run two of them in
parallel against one flow to see claim exclusivity in action — each step is
claimed by exactly one worker (park the workers with `-r` so they contend at
every level of the graph):

```
exoflow/contrib/worker.sh -u http://127.0.0.1:7676 -f <flow-id> -w w1 -r 10 > w1.log &
exoflow/contrib/worker.sh -u http://127.0.0.1:7676 -f <flow-id> -w w2 -r 10 > w2.log &
wait
```

## architecture

- **State lives in exomind keys.** Every flow and step (status, deps,
  claims, deadlines) is durably stored under `exoflow:*` keys, so the
  daemon survives restarts with no local data file: SIGKILL the exoflow
  process, start it again on the same ports/backend and the flow state is
  intact.
- **Claim timeouts.** A step may carry a 5th column `timeout_s`: the
  claim via `/next` sets its deadline to `now + timeout`, and the lazy
  sweep marks the step `overdue` if the worker does not finish in time;
  `unclaim` resets the clock. A stuck worker therefore releases the step
  deterministically, and the freeze detector (`agent-health` in exoqms)
  catches silent workers on top.
- **Deadlines via exosched.** A step with a `deadline_epoch` registers a
  scheduled reminder with exosched; when it fires (or on the next read, via
  the lazy deadline sweep) exoflow marks the step `overdue` and writes an
  audit note. `GET /flow?id=` reflects the overdue state.
- **Audit via notes.** Claims, step completions/failures, cancellations and
  deadline misses are written as timestamped exomind notes, which gives you
  an append-only ledger you can query with `GET /notes?q=<flow-id>`.
- **Loops are lazy.** A loop spawns its next iteration on the next read
  (`/flows`, `/flow?id=`, `/loops`, `/next`) or startup reload after the
  newest iteration is terminal and `next_run` has arrived — no background
  threads, no clock dependencies, survives SIGKILL. The exosched reminder
  `exoflow:loop:<id>` is feed candy; the lazy check is authoritative.

## loops

Make any flow a loop by adding an optional LAST line to the POST /flow
body:

```
loop<TAB>every <n><s|m|h><TAB>[max <n>] [until <epoch>]
```

`every 2s` / `every 1m` / `every 3h` set the interval; `max` caps the
number of COUNTED iterations (default unlimited); `until` stops new runs
at/after the given epoch. A line starting with `loop<TAB>every` that does
not parse is rejected with `error: bad loop spec`; any other last line is
a plain step, so old bodies keep working byte for byte.

Scheduling is **lazy**, exactly like deadlines: no background thread, no
timer-driven spawn. On every `GET /flows`, `GET /flow?id=`, `GET /loops`,
`GET /next` and every startup reload, exoflow checks whether the NEWEST
iteration of a loop is terminal (all done / all failed / cancelled) and
its `next_run` has arrived; if so it spawns the next iteration:

- same steps, all pending;
- flow name `iter <n+1>`;
- parent link `parent=<first flow id>` (the first iteration has none);
- `next_run` advanced by the interval on every record of the loop;
- audit note `flow loop <id> -> iter <n+1> at <epoch>`;
- best-effort exosched reminder `exoflow:loop:<id>` at the new next_run
  (feed candy only, like deadline reminders — the lazy check is
  authoritative).

Reaching `max` writes `flow loop <id> finished (max reached)` and zeroes
`next_run`; passing `until` writes `finished (until reached)`. Because
the check is lazy, a loop that became terminal while the daemon was down
resumes from persisted state at the next read — SIGKILL mid-loop is safe.

- `POST /flow?id=<f>&action=stop-loop` halts future iterations of the
  loop (note written, existing records kept).
- `DELETE /flow?id=<f>` on any record of a loop also halts the loop and
  removes that record.
- Cancelling one iteration (`action=cancel`) does NOT count toward `max`:
  the loop continues and a replacement iteration is spawned without
  consuming budget.
- `GET /loops` lists every iteration:
  `loop<TAB><id><TAB>iter <n><TAB>next <epoch><TAB>interval <s>`
  (`json=1` for JSON).

Persistence: format version 2 — the header is `exoflow<TAB>2<TAB>name`
and looping records carry one trailing line
`loop<TAB><interval s><TAB><max><TAB><until><TAB><iter><TAB><budget><TAB><next_run><TAB><parent><TAB><stopped>`.
Version-1 records load as non-looping flows (both formats are read).

## API reference

| method | path                     | body / params            | reply |
|--------|--------------------------|--------------------------|-------|
| GET    | `/`                      | —                        | self-describing text |
| GET    | `/ping`                  | —                        | `pong` |
| POST   | `/flow`                  | line 1 = flow name; then `id<TAB>desc<TAB>deps` lines, deps comma-separated (empty allowed); optional 4th field `deadline_epoch`, optional 5th field `timeout_s`; optional LAST line `loop<TAB>every <n><s|m|h><TAB>[max <n>] [until <epoch>]` | `ok <flow-id> <nsteps>` |
| GET    | `/flow?id=`              | —                        | TSV state (one line per step; `loop` line for loops) |
| GET    | `/flows`                 | —                        | flow list |
| GET    | `/loops`                 | —                        | loop iterations list |
| GET    | `/next?flow=&worker=`    | —                        | `ok <stepid>` (auto-claims) or `none` |
| POST   | `/step?flow=&id=`        | `done [note]` / `failed [note]` / `unclaim` | `ok` |
| POST   | `/flow?id=&action=cancel`| —                        | `ok` |
| POST   | `/flow?id=&action=stop-loop` | —                    | `ok` |
| DELETE | `/flow?id=`              | —                        | `ok` |

Auth: start exoflow with `--token <secret>`; every endpoint then requires
`Authorization: Bearer <secret>` and answers `401` without it.

## limitations

- exoflow is a *pull* orchestrator: workers must call `GET /next`; there is
  no push of new work to idle workers.
- A step claimed by a worker that dies is released only via an explicit
  `unclaim` (or the deadline sweep marking it `overdue`); there is no
  lease timeout / requeue-on-heartbeat.
- Deadline enforcement is best-effort lazy: the authoritative sweep runs on
  reads (`/flow`, `/next`) and startup reload, so an overdue step is
  reflected at the next read, not at the exact deadline instant.
- Loop scheduling is lazy too: the next iteration spawns at the first read
  after the newest iteration is terminal and `next_run` has arrived (the
  exosched reminder `exoflow:loop:<id>` only surfaces the due moment in
  the note feed). There is no timer-driven push.

## integration tests

`exoflow/test/test-integration.sh` spawns a private stack — exomind on
**7674**, exosched on **7675**, exoflow on **7676**, temp data under
`/tmp/b2-exoflow-int` — and proves the loop end-to-end:

1. creates the 5-step diamond (`s1 → s2,s3 → s4 → s5`) with a deadline on
   `s4`, runs **two `contrib/worker.sh` workers in parallel**, and asserts
   every step is done, no step was claimed by both workers, and `s4` was
   claimed only after both `s2` and `s3` were done (compared via note
   timestamps in the private exomind);
2. creates a flow whose step deadline is `now+2s`, waits, and asserts
   `/flow` marks it overdue and an audit note exists;
3. SIGKILLs exoflow mid-flow, restarts it on the same ports/backend, and
   asserts the state survived and the flow still completes;
4. restarts exoflow with `--token` and asserts 401 without / success with
   the token;
5. kills all three daemons, removes temp data, prints
   `=== results: N passed, 0 failed ===`.

Run it (builds exoflow first if needed; if `exoflow/` is missing from your
clone it fetches and merges `feat/exoflow` from origin):

```
make test-exoflow     # B1 unit tests + this integration suite
# or, standalone:
timeout 300 bash exoflow/test/test-integration.sh
```

Environment hooks: `EXOFLOW_BIN` (binary path) and `EXOFLOW_ARGS` (extra
daemon flags, e.g. `--token` in production-style runs) override the
defaults. The suite never touches shared swarm instances (7654/7655) and
only uses its own ports.

## License

GPL-3.0-only — see [LICENSE](../LICENSE).
