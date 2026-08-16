#!/usr/bin/env bash
# exoflow test suite: private exomind (7671) + exosched (7672) + exoflow
# (7673) instances, temp data in /tmp. Covers dep-order claims, exclusive
# claims, transitions, deadlines -> overdue, cancel, delete, restart
# recovery, exomind-down startup retry, auth, validation errors, fuzz.
set -u
cd "$(dirname "$0")"

ROOT="$(cd ../.. && pwd)"
XM_BIN=$ROOT/build/exomind
XS_BIN=$ROOT/exosched/build/exosched
XF_BIN=$ROOT/exoflow/build/exoflow
XM_PORT=7671
XS_PORT=7672
XF_PORT=7673
AUTH_PORT=7685
DEAD_PORT=7698
XM_URL="http://127.0.0.1:$XM_PORT"
XS_URL="http://127.0.0.1:$XS_PORT"
XF_URL="http://127.0.0.1:$XF_PORT"
AUTH_URL="http://127.0.0.1:$AUTH_PORT"
WORK=/tmp/exoflow_test
XM_DATA=$WORK/exomind.dat
XM_LOG=$WORK/exomind.log
XS_LOG=$WORK/exosched.log
XF_LOG=$WORK/exoflow.log
AUTH_LOG=$WORK/auth.log

PASS=0
FAIL=0
XM_PID=""
XS_PID=""
XF_PID=""

say() { printf '%s\n' "$*"; }

check() { # name, result, detail
    if [ "$2" = "0" ]; then
        PASS=$((PASS + 1))
        say "PASS  $1"
    else
        FAIL=$((FAIL + 1))
        say "FAIL  $1$([ -n "${3:-}" ] && say "      $3")"
    fi
}

port_pid() { # returns pid listening on a port, or empty
    ss -tlnp 2>/dev/null | grep ":$1 " | grep -oP 'pid=\K[0-9]+' | head -1
}

stop_port() { # port
    local pid
    pid=$(port_pid "$1")
    [ -n "$pid" ] && kill "$pid" 2>/dev/null
    sleep 0.3
}

cleanup() {
    [ -n "$XF_PID" ] && kill "$XF_PID" 2>/dev/null
    [ -n "$XS_PID" ] && kill "$XS_PID" 2>/dev/null
    [ -n "$XM_PID" ] && kill "$XM_PID" 2>/dev/null
    stop_port $XF_PORT
    stop_port $XS_PORT
    stop_port $XM_PORT
    stop_port $AUTH_PORT
    stop_port $DEAD_PORT
}
trap cleanup EXIT

for bin in "$XM_BIN" "$XS_BIN" "$XF_BIN"; do
    if [ ! -x "$bin" ]; then
        say "missing $bin - build first (make -C .. exosched && make -C .. exoflow)"
        exit 1
    fi
done
command -v python3 >/dev/null || { say "python3 required"; exit 1; }
command -v ss >/dev/null || { say "ss (iproute2) required"; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK"

# --- start private exomind + exosched + exoflow ---------------------------
stop_port $XM_PORT
stop_port $XS_PORT
stop_port $XF_PORT
stop_port $AUTH_PORT
stop_port $DEAD_PORT
setsid nohup "$XM_BIN" --port $XM_PORT --data "$XM_DATA" \
    >"$XM_LOG" 2>&1 < /dev/null &
XM_PID=$!
sleep 0.5
setsid nohup "$XS_BIN" --port $XS_PORT --exomind "$XM_URL" \
    >"$XS_LOG" 2>&1 < /dev/null &
XS_PID=$!
sleep 0.5
setsid nohup "$XF_BIN" --port $XF_PORT --exomind "$XM_URL" \
    --exosched "$XS_URL" >"$XF_LOG" 2>&1 < /dev/null &
XF_PID=$!
sleep 0.8

say "=== exoflow test suite ==="

r=$(curl -s -m 3 "$XF_URL/ping")
check "ping" "$([ "$r" = "pong" ] && echo 0 || echo 1)" "got: $r"

r=$(curl -s -m 3 "$XF_URL/")
check "GET / is self-describing" \
    "$(printf '%s' "$r" | grep -q exoflow && printf '%s' "$r" | grep -q '/next' && echo 0 || echo 1)"

# --- create a flow with deps (parallel branches) ---------------------------
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'build docs\ndocs\twrite the docs\t\ncode\twrite the code\tdocs\ntest\trun the tests\tcode\nship\tship it\tdocs')
F1=$(echo "$r" | awk '{print $2}')
N1=$(echo "$r" | awk '{print $3}')
check "create flow replies 'ok <id> <nsteps>'" \
    "$(printf '%s' "$r" | grep -qE '^ok [0-9]+:[0-9a-f]+ 4$' && echo 0 || echo 1)" "$r"
check "flow persisted under exoflow:flow:<id>" \
    "$(curl -s -m 3 "$XM_URL/get?key=exoflow:flow:$F1" | grep -q "build docs" && echo 0 || echo 1)"

r=$(curl -s -m 3 "$XF_URL/flow?id=$F1")
check "/flow TSV: header + 4 step lines" \
    "$(printf '%s' "$r" | head -1 | awk -F '\t' -v id="$F1" 'NF==4 && $1=="flow" && $2==id && $4=="active" {ok=1} END {exit ok?0:1}' && [ "$(printf '%s' "$r" | grep -c '^step')" = "4" ] && echo 0 || echo 1)" "$r"
check "/flow deadline column 0 when none" \
    "$(printf '%s' "$r" | grep '^step' | awk -F '\t' '$5=="0" {ok=1} END {exit ok?0:1}' && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/flow?id=does-not-exist" -w '\n%{http_code}')
check "unknown flow -> 404 missing" \
    "$(printf '%s' "$r" | grep -q missing && printf '%s' "$r" | grep -q '^404$' && echo 0 || echo 1)" "$r"

# --- /next respects dep order and claims exclusively -----------------------
r=$(curl -s -m 3 "$XF_URL/next?flow=$F1&worker=w1")
check "/next claims first ready step (docs, no deps)" \
    "$([ "$r" = "ok docs" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/next?flow=$F1&worker=w2")
check "/next claim is exclusive (w2 gets none)" \
    "$([ "$r" = "none" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/next?flow=$F1&worker=w3")
check "/next won't skip a dep (code+ship need docs)" \
    "$([ "$r" = "none" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/flow?id=$F1")
check "claimed step shows owner" \
    "$(printf '%s' "$r" | grep '^step' | awk -F '\t' '$2=="docs" && $3=="claimed" && $4=="w1" {ok=1} END {exit ok?0:1}' && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/step?flow=$F1&id=code" -d 'done premature')
check "done with deps pending -> error: deps pending" \
    "$([ "$r" = "error: deps pending" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/step?flow=$F1&id=code" -d 'failed broken build')
check "failed from pending -> ok" \
    "$([ "$r" = "ok" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/step?flow=$F1&id=code" -d 'unclaim')
check "unclaim on failed step -> error: not claimed" \
    "$([ "$r" = "error: not claimed" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/step?flow=$F1&id=docs" -d 'done the docs are written')
check "done a claimed step -> ok" \
    "$([ "$r" = "ok" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/step?flow=$F1&id=docs" -d 'done again')
check "done twice -> error: already done" \
    "$([ "$r" = "error: already done" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/next?flow=$F1&worker=w2")
check "failed branch skipped, /next takes ship (deps done)" \
    "$([ "$r" = "ok ship" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/step?flow=$F1&id=ship" -d 'unclaim')
check "unclaim a claimed step -> ok" \
    "$([ "$r" = "ok" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/flow?id=$F1")
check "unclaimed step back to pending without owner" \
    "$(printf '%s' "$r" | grep '^step' | awk -F '\t' '$2=="ship" && $3=="pending" && $4=="" {ok=1} END {exit ok?0:1}' && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/next?flow=$F1&worker=w3")
check "unclaimed step claimable again" \
    "$([ "$r" = "ok ship" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/step?flow=$F1&id=test" -d 'done tests pass')
check "failed dep blocks its branch (test -> deps pending)" \
    "$([ "$r" = "error: deps pending" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/step?flow=$F1&id=ship" -d 'done shipped with note: hello swarm')
check "done the surviving branch -> ok" \
    "$([ "$r" = "ok" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/next?flow=$F1&worker=w4")
check "/next has nothing left (test blocked by failed code)" \
    "$([ "$r" = "none" ] && echo 0 || echo 1)" "$r"

# --- clean linear flow -> done + audit --------------------------------------
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'linear\na\tstep a\t\nb\tstep b\ta\nc\tstep c\tb')
F2=$(echo "$r" | awk '{print $2}')
r=$(curl -s -m 3 "$XF_URL/next?flow=$F2&worker=l1")
check "linear: claim a" "$([ "$r" = "ok a" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/step?flow=$F2&id=a" -d 'done a ok')
check "linear: done a" "$([ "$r" = "ok" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/next?flow=$F2&worker=l1")
check "linear: claim b after a done" "$([ "$r" = "ok b" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/step?flow=$F2&id=b" -d 'done b ok')
check "linear: done b" "$([ "$r" = "ok" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/next?flow=$F2&worker=l2")
check "linear: claim c after b done" "$([ "$r" = "ok c" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/step?flow=$F2&id=c" -d 'done c ok: all green')
check "linear: done c" "$([ "$r" = "ok" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/flows")
check "flow status becomes done" \
    "$(printf '%s' "$r" | grep "^flow" | awk -F '\t' -v id="$F2" '$2==id && $4=="done" {ok=1} END {exit ok?0:1}' && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XM_URL/notes?q=flow%20$F2")
check "audit trail exists in exomind notes" \
    "$(printf '%s' "$r" | grep -q "flow $F2 step c -> done by l2: c ok: all green" && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/next?flow=$F2&worker=w5")
check "/next on done flow -> none" \
    "$([ "$r" = "none" ] && echo 0 || echo 1)" "$r"

# --- json endpoints --------------------------------------------------------
r=$(curl -s -m 3 "$XF_URL/next?flow=$F2&worker=w6&json=1")
check "/next?json=1 is valid JSON" \
    "$(printf '%s' "$r" | python3 -c "
import json,sys
d=json.load(sys.stdin)
sys.exit(0 if d['flow']=='$F2' and d['step'] is None else 1)" && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/flow?id=$F2&json=1")
check "/flow?json=1 is valid JSON" \
    "$(printf '%s' "$r" | python3 -c "
import json,sys
d=json.load(sys.stdin)
sys.exit(0 if d['id']=='$F2' and d['status']=='done' and len(d['steps'])==3 else 1)" && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/flows?json=1&status=done")
check "/flows?json=1&status=done filters" \
    "$(printf '%s' "$r" | python3 -c "
import json,sys
d=json.load(sys.stdin)
sys.exit(0 if all(t['status']=='done' for t in d) and any(t['id']=='$F2' for t in d) else 1)" && echo 0 || echo 1)" "$r"

# --- validation errors -----------------------------------------------------
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'dup\na\tfirst\t\na\tsecond\t')
check "duplicate step id rejected" \
    "$([ "$r" = "error: duplicate step a" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'unk\nb\tsecond\ta,zzz\na\tfirst\t')
check "unknown dep rejected" \
    "$([ "$r" = "error: unknown dep zzz" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'cyc\nx\tone\ty\ny\ttwo\tx')
check "cyclic deps rejected" \
    "$([ "$r" = "error: cyclic deps" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/flow" -d '')
check "empty body rejected" \
    "$([ "$r" = "error: empty body" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'garbage\njust-one-column')
check "malformed step line rejected" \
    "$([ "$r" = "error: bad step line" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/step?flow=$F1&id=nope" -d 'done')
check "unknown step -> error: no such step" \
    "$([ "$r" = "error: no such step" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/step?flow=$F1&id=docs" -d 'frobnicate')
check "bad action rejected" \
    "$([ "$r" = "error: bad action" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/next?flow=$F1&worker=")
check "/next without worker -> error: missing worker" \
    "$([ "$r" = "error: missing worker" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/flow?id=$F1&action=banana")
check "bad cancel action rejected" \
    "$([ "$r" = "error: bad action banana" ] && echo 0 || echo 1)" "$r"

# --- fuzz: garbage bodies never crash --------------------------------------
CRASHED=0
for i in 1 2 3 4 5; do
    r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary "$(head -c 300 /dev/urandom)")
    s=$(curl -s -m 3 -o /dev/null -w '%{http_code}' -X POST "$XF_URL/flow" --data-binary "x$(head -c 50 /dev/urandom)")
    [ "$s" = "400" ] || CRASHED=1
    r=$(curl -s -m 3 -X POST "$XF_URL/step?flow=$F1&id=docs" --data-binary "$(head -c 50 /dev/urandom)")
done
check "garbage bodies -> 400, no crash" "$CRASHED" "$r"
check "daemon still alive after fuzz" \
    "$([ "$(curl -s -m 3 "$XF_URL/ping")" = "pong" ] && echo 0 || echo 1)"

# --- deadline -> overdue ----------------------------------------------------
DL=$(( $(date +%s) + 2 ))
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'deadline flow\nslow\tdo the slow thing\t\t'$DL)
F3=$(echo "$r" | awk '{print $2}')
check "create flow with deadline" \
    "$(printf '%s' "$r" | grep -qE '^ok [0-9]+:[0-9a-f]+ 1$' && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XS_URL/timers")
check "exosched timer registered for deadline" \
    "$(printf '%s' "$r" | grep -q "exoflow $F3 slow" && echo 0 || echo 1)" "$r"
sleep 3.5
r=$(curl -s -m 3 "$XF_URL/flow?id=$F3")
check "expired deadline -> step overdue" \
    "$(printf '%s' "$r" | grep '^step' | awk -F '\t' '$2=="slow" && $3=="overdue" {ok=1} END {exit ok?0:1}' && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XM_URL/notes?q=flow%20$F3")
check "overdue audit note exists" \
    "$(printf '%s' "$r" | grep -q "flow $F3 step slow -> overdue by exoflow" && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XM_URL/notes?q=fired")
check "exosched fired the deadline reminder (note in feed)" \
    "$(printf '%s' "$r" | grep -q "fired timer .*exoflow $F3 slow" && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/step?flow=$F3&id=slow" -d 'done late but done')
check "overdue step can still be done" \
    "$([ "$r" = "ok" ] && echo 0 || echo 1)" "$r"

# --- cancel -----------------------------------------------------------------
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'cancel me\nc1\tstep one\t\nc2\tstep two\tc1\nc3\tstep three\t')
F4=$(echo "$r" | awk '{print $2}')
curl -s -m 3 "$XF_URL/next?flow=$F4&worker=w1" >/dev/null
r=$(curl -s -m 3 -X POST "$XF_URL/flow?id=$F4&action=cancel")
check "cancel flow -> ok" \
    "$([ "$r" = "ok" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/flow?id=$F4")
check "cancel: non-terminal steps cancelled, done untouched" \
    "$(printf '%s' "$r" | awk -F '\t' '/^step/ {if ($1=="c1" && $3!="cancelled") bad=1; if ($1=="c2" && $3!="cancelled") bad=1; if ($1=="c3" && $3!="cancelled") bad=1} END {exit bad?1:0}' && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/step?flow=$F4&id=c1" -d 'done')
check "done a cancelled step -> error: step is cancelled" \
    "$([ "$r" = "error: step is cancelled" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/next?flow=$F4&worker=w2")
check "no claims after cancel" \
    "$([ "$r" = "none" ] && echo 0 || echo 1)" "$r"

# --- delete ------------------------------------------------------------------
r=$(curl -s -m 3 -X DELETE "$XF_URL/flow?id=$F4")
check "DELETE flow -> ok" \
    "$([ "$r" = "ok" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X DELETE "$XF_URL/flow?id=$F4")
check "DELETE twice -> missing" \
    "$([ "$r" = "missing" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XM_URL/list?prefix=exoflow:flow:$F4")
check "deleted flow's exomind key is gone" \
    "$([ -z "$(printf '%s' "$r" | grep "$F4")" ] && echo 0 || echo 1)" "$r"

# --- restart recovery -------------------------------------------------------
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'restart me\np\tprepare\t\nw\twork\tp\nd\tdeliver\tw\t')
F5=$(echo "$r" | awk '{print $2}')
curl -s -m 3 "$XF_URL/next?flow=$F5&worker=a1" >/dev/null
curl -s -m 3 -X POST "$XF_URL/step?flow=$F5&id=p" -d 'done ready' >/dev/null
curl -s -m 3 "$XF_URL/next?flow=$F5&worker=a1" >/dev/null
kill -9 "$XF_PID" 2>/dev/null
sleep 0.5
setsid nohup "$XF_BIN" --port $XF_PORT --exomind "$XM_URL" \
    --exosched "$XS_URL" >>"$XF_LOG" 2>&1 < /dev/null &
XF_PID=$!
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ "$(curl -s -m 2 "$XF_URL/ping")" = "pong" ] && break
    sleep 0.5
done
r=$(curl -s -m 3 "$XF_URL/flow?id=$F5")
check "SIGKILL restart: flow state intact" \
    "$(printf '%s' "$r" | awk -F '\t' '/^step/ {if ($1=="p" && $3!="done") bad=1; if ($1=="w" && $3!="claimed") bad=1; if ($1=="w" && $4!="a1") bad=1; if ($1=="d" && $3!="pending") bad=1} END {exit bad?1:0}' && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/flows")
check "restart: /flows lists reloaded flows" \
    "$(printf '%s' "$r" | grep -q "$F5" && echo 0 || echo 1)" "$r"

# deadline expiring while the daemon is down -> overdue on reload
DL=$(( $(date +%s) + 1 ))
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'down deadline\nsoon\turgent\t\t'$DL)
F6=$(echo "$r" | awk '{print $2}')
kill -9 "$XF_PID" 2>/dev/null
sleep 2.5
setsid nohup "$XF_BIN" --port $XF_PORT --exomind "$XM_URL" \
    --exosched "$XS_URL" >>"$XF_LOG" 2>&1 < /dev/null &
XF_PID=$!
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ "$(curl -s -m 2 "$XF_URL/ping")" = "pong" ] && break
    sleep 0.5
done
r=$(curl -s -m 3 "$XF_URL/flow?id=$F6")
check "deadline expired while down -> overdue on reload" \
    "$(printf '%s' "$r" | grep '^step' | awk -F '\t' '$2=="soon" && $3=="overdue" {ok=1} END {exit ok?0:1}' && echo 0 || echo 1)" "$r"

# --- loops --------------------------------------------------------------------
# every helper is SCOPED to one loop via its parent link: several loops run
# in this suite simultaneously, so /loops rows must be matched per loop.
# drive the single t1 step of a flow to done (skip if already terminal)
drive_t1() { # flow id
    local fid="$1" st
    st=$(curl -s -m 3 "$XF_URL/flow?id=$fid" | head -1 | awk -F '\t' '{print $4}')
    case "$st" in done|failed|cancelled) return 1;; esac
    curl -s -m 3 "$XF_URL/next?flow=$fid&worker=loopw" >/dev/null
    curl -s -m 3 -X POST "$XF_URL/step?flow=$fid&id=t1" -d 'done iteration' >/dev/null
    return 0
}
# name of a flow record (3rd TSV column of /flow header)
flow_name() { # flow id
    curl -s -m 3 "$XF_URL/flow?id=$1" | head -1 | awk -F '\t' '{print $3}'
}
# newest record of the loop rooted at $1 (/loops lists newest first)
newest_iter() { # root
    loop_iters "$1" | head -1
}
# ids of all records of the loop rooted at $1 (root + iterations)
loop_iters() { # root
    local root="$1" id body
    for id in $(curl -s -m 3 "$XF_URL/loops" | awk -F '\t' '$1=="loop" {print $2}'); do
        [ "$id" = "$root" ] && { echo "$id"; continue; }
        body=$(curl -s -m 3 "$XF_URL/flow?id=$id")
        printf '%s' "$body" | grep -q "parent $root" && echo "$id"
    done
}
# prints the /loops row for one flow id (empty if not listed)
loop_row() { # flow id
    curl -s -m 3 "$XF_URL/loops" | awk -F '\t' -v id="$1" '$1=="loop" && $2==id {print}'
}
# 0 when the lazy finalize of a loop record has fired (next_run zeroed)
finalized() { # flow id
    loop_row "$1" | grep -q 'next 0'
}
# poll the note feed until the pattern appears (lazy finalize notes are
# written by the tick that fires them, which may lag the state change)
# poll the note feed until every grep pattern appears (the lazy finalize
# notes are written by the tick that fires them, which may lag; the /flows
# poke triggers the lazy checks and the query retry removes timing
# sensitivity)
wait_notes() { # q-pattern (%20-encoded), max seconds, patterns...
    local pat="$1" max="$2" i plain
    shift 2
    for i in $(seq 1 $((max * 2))); do
        curl -s -m 3 "$XF_URL/flows" >/dev/null
        RN=$(curl -s -m 3 "$XM_URL/notes?q=$pat&limit=1000")
        ok=1
        for pat2 in "$@"; do
            printf '%s' "$RN" | grep -q "$pat2" || ok=0
        done
        [ "$ok" = "1" ] && return 0
        sleep 0.5
    done
    return 1
}
# one pass: drive every not-yet-driven record of the loop; 0 = progress made
DRIVEN=""
drive_all() { # root flow id
    local root="$1" id changed=0
    for id in $(loop_iters "$root"); do
        case " $DRIVEN " in *" $id "*) continue;; esac
        drive_t1 "$id"
        DRIVEN="$DRIVEN $id"
        changed=1
    done
    [ "$changed" = "1" ]
}
# wait until a shell expression becomes true; arg 1 = max seconds
wait_true() { # seconds, cmd...
    local max="$1" i; shift
    for i in $(seq 1 $((max * 2))); do
        "$@" && return 0
        sleep 0.5
    done
    return 1
}
count_iters() { # root: number of records of this loop listed by /loops
    local n=0 id
    for id in $(loop_iters "$1"); do n=$((n + 1)); done
    echo "$n"
}
iters_ge() { # root, n
    [ "$(count_iters "$1")" -ge "$2" ]
}
latest_done() { # root: 0 when the newest record of the loop is terminal
    local last
    last=$(newest_iter "$1")
    [ -z "$last" ] && return 1
    case "$(curl -s -m 3 "$XF_URL/flow?id=$last" | head -1 | awk -F '\t' '{print $4}')" in
        done|failed|cancelled) return 0;;
    esac
    return 1
}
# drive every iteration of the loop as it spawns until <n> records exist
# and the newest is terminal (bounded ~30s)
run_loop_to() { # root, n
    local root="$1" n="$2" i
    DRIVEN="$root"
    drive_t1 "$root" >/dev/null
    for i in $(seq 1 60); do
        drive_all "$root" && continue
        iters_ge "$root" "$n" && latest_done "$root" && break
        sleep 0.5
    done
    curl -s -m 3 "$XF_URL/flows" >/dev/null
}

r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'bad unit\na\tone\t\nloop\tevery 5x')
check "malformed loop spec (bad unit) -> error: bad loop spec" \
    "$([ "$r" = "error: bad loop spec" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'dangling max\na\tone\t\nloop\tevery 2s\tmax')
check "malformed loop spec (dangling max) -> error: bad loop spec" \
    "$([ "$r" = "error: bad loop spec" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'bad token\na\tone\t\nloop\tevery 2s\tbanana 3')
check "malformed loop spec (bad token) -> error: bad loop spec" \
    "$([ "$r" = "error: bad loop spec" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'no every\na\tone\t\nloop\tmax 3')
check "malformed loop spec (no every) -> error: bad loop spec" \
    "$([ "$r" = "error: bad loop spec" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'no steps\nt1\tstep one\t\nloop\tevery 2s\tmax 3')
L1=$(echo "$r" | awk '{print $2}')
check "loop create replies 'ok <id> 1'" \
    "$(printf '%s' "$r" | grep -qE '^ok [0-9]+:[0-9a-f]+ 1$' && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'one shot\no1\tstep one\t')
O1=$(echo "$r" | awk '{print $2}')
r=$(curl -s -m 3 "$XF_URL/flow?id=$O1")
check "one-shot flow has no loop line" \
    "$(printf '%s' "$r" | grep -q '^loop' && echo 1 || echo 0)" "$r"
r=$(curl -s -m 3 "$XF_URL/loops")
check "one-shot flow not listed by /loops" \
    "$(printf '%s' "$r" | grep -q "$O1" && echo 1 || echo 0)" "$r"
r=$(curl -s -m 3 "$XF_URL/flow?id=$L1")
check "loop record carries the loop spec" \
    "$(printf '%s' "$r" | grep -q 'loop.*every 2s' && printf '%s' "$r" | grep -q 'max 3' && printf '%s' "$r" | grep -q 'iter 1' && echo 0 || echo 1)" "$r"

# every 2s max 3 -> 3 instances, iter labels, parent links, notes
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'basic loop\nt1\tstep one\t\nloop\tevery 2s\tmax 3')
L2=$(echo "$r" | awk '{print $2}')
run_loop_to "$L2" 3
check "max 3 loop spawns exactly 3 iterations" \
    "$([ "$(count_iters "$L2")" = "3" ] && echo 0 || echo 1)" "$(loop_row "$L2")"
LOOPLINES=""
for id in $(loop_iters "$L2"); do
    LOOPLINES="$LOOPLINES
$(loop_row "$id")"
done
check "/loops lists iter 1 2 3 with interval 2s" \
    "$(printf '%s' "$LOOPLINES" | grep -q 'iter 1' && printf '%s' "$LOOPLINES" | grep -q 'iter 2' && printf '%s' "$LOOPLINES" | grep -q 'iter 3' && printf '%s' "$LOOPLINES" | grep -q 'interval 2$' && echo 0 || echo 1)" "$LOOPLINES"
I2=$(for id in $(loop_iters "$L2"); do [ "$(flow_name "$id")" = "iter 2" ] && echo "$id" && break; done)
I3=$(for id in $(loop_iters "$L2"); do [ "$(flow_name "$id")" = "iter 3" ] && echo "$id" && break; done)
wait_true 8 finalized "$I3"
wait_notes "flow%20loop%20$L2" 10 "flow loop $L2 -> iter 2 at " \
    "flow loop $L2 -> iter 3 at " "flow loop $L2 finished (max reached)"
r=$(curl -s -m 3 "$XF_URL/flow?id=$I2")
check "iter 2 named 'iter 2' with parent link" \
    "$(printf '%s' "$r" | head -1 | grep -q 'iter 2' && printf '%s' "$r" | grep -q "parent $L2" && echo 0 || echo 1)" "$r"
RN=$(curl -s -m 8 "$XM_URL/notes?q=flow%20loop%20$L2&limit=1000")
check "loop audit notes: spawns + max reached" \
    "$(printf '%s' "$RN" | grep -q "flow loop $L2 -> iter 2 at " && printf '%s' "$RN" | grep -q "flow loop $L2 -> iter 3 at " && printf '%s' "$RN" | grep -q "flow loop $L2 finished (max reached)" && echo 0 || echo 1)" "$RN"
r=$(curl -s -m 8 "$XF_URL/flow?id=$I3")
check "max reached zeroes next_run" \
    "$(printf '%s' "$r" | grep '^loop' | grep -q 'next 0' && echo 0 || echo 1)" "$r"

# stop-loop halts future iterations
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'stop loop\nt1\tstep one\t\nloop\tevery 1s\tmax 10')
L3=$(echo "$r" | awk '{print $2}')
DRIVEN="$L3"
drive_t1 "$L3" >/dev/null
wait_true 10 iters_ge "$L3" 2
r=$(curl -s -m 3 -X POST "$XF_URL/flow?id=$L3&action=stop-loop")
check "stop-loop -> ok" "$([ "$r" = "ok" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XM_URL/notes?q=flow%20loop%20$L3")
check "stop-loop audit note written" \
    "$(printf '%s' "$r" | grep -q "flow loop $L3 stopped (stop-loop)" && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/flow?id=$L3")
check "stopped loop shows stopped 1" \
    "$(printf '%s' "$r" | grep '^loop' | grep -q 'stopped 1' && echo 0 || echo 1)" "$r"
C=$(count_iters "$L3")
sleep 3.5
check "stop-loop: no further iterations spawned" \
    "$([ "$(count_iters "$L3")" = "$C" ] && echo 0 || echo 1)"
r=$(curl -s -m 3 -X POST "$XF_URL/flow?id=$O1&action=stop-loop")
check "stop-loop on non-loop -> error: not a loop" \
    "$([ "$r" = "error: not a loop" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/flow?id=missing-loop&action=stop-loop" -w '\n%{http_code}')
check "stop-loop on unknown flow -> 404 missing" \
    "$(printf '%s' "$r" | grep -q missing && printf '%s' "$r" | grep -q '^404$' && echo 0 || echo 1)" "$r"

# until bound: every 1s until now+6 -> stops
UNTIL=$(( $(date +%s) + 6 ))
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'until loop\nt1\tstep one\t\nloop\tevery 1s\tuntil '$UNTIL)
L4=$(echo "$r" | awk '{print $2}')
run_loop_to "$L4" 6
C=$(count_iters "$L4")
sleep 3
check "until bound stops the loop" \
    "$([ "$(count_iters "$L4")" = "$C" ] && echo 0 || echo 1)"
r=$(curl -s -m 3 "$XM_URL/notes?q=flow%20loop%20$L4&limit=100")
check "until bound audit note written" \
    "$(printf '%s' "$r" | grep -q "flow loop $L4 finished (until reached)" && echo 0 || echo 1)" "$r"

# SIGKILL mid-loop: loop resumes from persisted state after restart
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'kill loop\nt1\tstep one\t\nloop\tevery 2s\tmax 3')
L5=$(echo "$r" | awk '{print $2}')
DRIVEN="$L5"
drive_t1 "$L5" >/dev/null
wait_true 10 iters_ge "$L5" 2
kill -9 "$XF_PID" 2>/dev/null
sleep 0.5
setsid nohup "$XF_BIN" --port $XF_PORT --exomind "$XM_URL" \
    --exosched "$XS_URL" >>"$XF_LOG" 2>&1 < /dev/null &
XF_PID=$!
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ "$(curl -s -m 2 "$XF_URL/ping")" = "pong" ] && break
    sleep 0.5
done
check "SIGKILL mid-loop: iterations survive restart" \
    "$([ "$(count_iters "$L5")" -ge 2 ] && { for id in $(loop_iters "$L5"); do [ "$(flow_name "$id")" = "iter 2" ] && echo y; done; } | grep -q y && echo 0 || echo 1)"
run_loop_to "$L5" 3
wait_notes "flow%20loop%20$L5" 20 "flow loop $L5 -> iter 3 at " \
    "flow loop $L5 finished (max reached)"
check "loop continued after restart (iter 3 + max reached)" \
    "$(printf '%s' "$RN" | grep -q "flow loop $L5 -> iter 3 at " && printf '%s' "$RN" | grep -q "flow loop $L5 finished (max reached)" && echo 0 || echo 1)" "$RN"

# explicit cancel of one iteration does not count toward max
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'cancel loop\nt1\tstep one\t\nloop\tevery 1s\tmax 2')
L6=$(echo "$r" | awk '{print $2}')
DRIVEN="$L6"
drive_t1 "$L6" >/dev/null
wait_true 10 iters_ge "$L6" 2
I2B=$(for id in $(loop_iters "$L6"); do [ "$(curl -s -m 3 "$XF_URL/flow?id=$id" | head -1 | awk -F '\t' '{print $3}')" = "iter 2" ] && echo "$id" && break; done)
r=$(curl -s -m 3 -X POST "$XF_URL/flow?id=$I2B&action=cancel")
check "cancel an iteration -> ok" "$([ "$r" = "ok" ] && echo 0 || echo 1)" "$r"
run_loop_to "$L6" 3
check "cancelled iteration replaced without consuming max" \
    "$([ "$(count_iters "$L6")" = "3" ] && { for id in $(loop_iters "$L6"); do [ "$(flow_name "$id")" = "iter 3" ] && echo y; done; } | grep -q y && echo 0 || echo 1)"
sleep 3
check "cancelled iteration: loop ends at max 2 counted (no iter 4)" \
    "$([ "$(count_iters "$L6")" = "3" ] && echo 0 || echo 1)"
r=$(curl -s -m 3 "$XM_URL/notes?q=flow%20loop%20$L6&limit=100")
check "cancelled-iteration loop reached max" \
    "$(printf '%s' "$r" | grep -q "flow loop $L6 finished (max reached)" && echo 0 || echo 1)" "$r"

# v1-format flow (raw legacy key) loads without looping
kill -9 "$XF_PID" 2>/dev/null
sleep 0.5
V1PAYLOAD=$(python3 -c "
import json
doc = 'exoflow\t1\tv1 legacy flow\nstep\tv1s\tlegacy step\t\n'
print(json.dumps({'key': 'exoflow:flow:v1legacy1', 'value': doc}))")
curl -s -m 3 -H "Content-Type: application/json" -X POST "$XM_URL/set" -d "$V1PAYLOAD" >/dev/null
setsid nohup "$XF_BIN" --port $XF_PORT --exomind "$XM_URL" \
    --exosched "$XS_URL" >>"$XF_LOG" 2>&1 < /dev/null &
XF_PID=$!
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ "$(curl -s -m 2 "$XF_URL/ping")" = "pong" ] && break
    sleep 0.5
done
r=$(curl -s -m 3 "$XF_URL/flow?id=v1legacy1")
check "v1-format flow loads without looping" \
    "$(printf '%s' "$r" | head -1 | grep -q 'v1 legacy flow' && printf '%s' "$r" | grep -q '^step' && printf '%s' "$r" | grep -qv '^loop' && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/loops")
check "v1-format flow not listed by /loops" \
    "$(printf '%s' "$r" | grep -q v1legacy1 && echo 1 || echo 0)" "$r"
curl -s -m 3 "$XF_URL/next?flow=v1legacy1&worker=loopw" >/dev/null
r=$(curl -s -m 3 -X POST "$XF_URL/step?flow=v1legacy1&id=v1s" -d 'done legacy')
check "v1-format flow still fully operable" \
    "$([ "$r" = "ok" ] && echo 0 || echo 1)" "$r"

# --- exomind down at startup: /ping serves, reload retries -------------------
kill "$XF_PID" 2>/dev/null
kill "$XM_PID" 2>/dev/null
kill "$XS_PID" 2>/dev/null
sleep 0.5
stop_port $XF_PORT
stop_port $XM_PORT
setsid nohup "$XF_BIN" --port $XF_PORT --exomind "http://127.0.0.1:$DEAD_PORT" \
    --exosched "http://127.0.0.1:$DEAD_PORT" >>"$XF_LOG" 2>&1 < /dev/null &
XF_PID=$!
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ "$(curl -s -m 2 "$XF_URL/ping")" = "pong" ] && break
    sleep 0.5
done
r=$(curl -s -m 3 "$XF_URL/ping")
check "exomind down: /ping still answers pong" \
    "$([ "$r" = "pong" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/flow" -d $'nope\nx\tfirst\t')
check "exomind down: create reports unavailable" \
    "$(printf '%s' "$r" | grep -q 'error: exomind unavailable' && echo 0 || echo 1)" "$r"
setsid nohup "$XM_BIN" --port $DEAD_PORT --data "$XM_DATA" \
    >>"$XM_LOG" 2>&1 < /dev/null &
XM_PID=$!
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ "$(curl -s -m 2 "http://127.0.0.1:$DEAD_PORT/ping")" = "pong" ] && break
    sleep 0.5
done
r=$(curl -s -m 3 -X POST "$XF_URL/flow" -d $'retry ok\nx\tfirst\t')
check "exomind up: create works after background reload" \
    "$(printf '%s' "$r" | grep -qE '^ok ' && echo 0 || echo 1)" "$r"
for i in 1 2 3 4 5 6 7 8 9 10; do
    r=$(curl -s -m 2 "$XF_URL/flow?id=$F5")
    printf '%s' "$r" | grep -q "$F5" && break
    sleep 0.5
done
r=$(curl -s -m 3 "$XF_URL/flow?id=$F5")
check "background reload picked up existing flows" \
    "$(printf '%s' "$r" | grep -q "$F5" && echo 0 || echo 1)" "$r"
kill "$XM_PID" 2>/dev/null
XM_PID=""
stop_port $DEAD_PORT
# the down-test daemon still points at the dead backend: retire it and
# bring the shared daemon back on the real exomind for later sections
kill "$XF_PID" 2>/dev/null
XF_PID=""
stop_port $XF_PORT
setsid nohup "$XF_BIN" --port $XF_PORT --exomind "$XM_URL" \
    --exosched "$XS_URL" >>"$XF_LOG" 2>&1 < /dev/null &
XF_PID=$!
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ "$(curl -s -m 2 "$XF_URL/ping")" = "pong" ] && break
    sleep 0.5
done

# bring exomind back on its own port so the auth instance can reload
setsid nohup "$XM_BIN" --port $XM_PORT --data "$XM_DATA" \
    >>"$XM_LOG" 2>&1 < /dev/null &
XM_PID=$!
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ "$(curl -s -m 2 "$XM_URL/ping")" = "pong" ] && break
    sleep 0.5
done

# --- auth on/off --------------------------------------------------------------
setsid nohup "$XF_BIN" --port $AUTH_PORT --exomind "$XM_URL" \
    --exosched "$XS_URL" --token supersecret >"$AUTH_LOG" 2>&1 < /dev/null &
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ "$(curl -s -m 2 "$AUTH_URL/ping")" = "pong" ] && break
    sleep 0.5
done
r=$(curl -s -m 3 -o /dev/null -w '%{http_code}' "$AUTH_URL/flows")
check "auth: no token -> 401" \
    "$([ "$r" = "401" ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -H "Authorization: Bearer wrong" -o /dev/null -w '%{http_code}' "$AUTH_URL/flows")
check "auth: wrong token -> 401" \
    "$([ "$r" = "401" ] && echo 0 || echo 1)" "$r"
for i in 1 2 3 4 5 6 7 8 9 10; do
    r=$(curl -s -m 2 -H "Authorization: Bearer supersecret" "$AUTH_URL/flows")
    printf '%s' "$r" | grep -q '^flow' && break
    sleep 0.5
done
check "auth: correct token -> 200 + data" \
    "$([ -n "$r" ] && printf '%s' "$r" | grep -q '^flow' && echo 0 || echo 1)" "$r"
stop_port $AUTH_PORT

# --- /flows filter, limit/offset ----------------------------------------------
r=$(curl -s -m 3 "$XF_URL/flows?status=active")
check "/flows?status=active only active" \
    "$(printf '%s' "$r" | grep '^flow' | awk -F '\t' '$4!="active" {bad=1} END {exit bad?1:0}' && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/flows?limit=1")
check "/flows?limit=1 caps results" \
    "$([ "$(printf '%s' "$r" | grep -c '^flow')" = "1" ] && echo 0 || echo 1)" "$r"

# --- done with note audit detail already covered; final summary --------------
# --- claim timeout steps -----------------------------------------------------
r=$(curl -s -m 3 -X POST "$XF_URL/flow" --data-binary $'timeout flow\nquick\tfinish fast\t\t\t2')
check "create timeout flow" \
    "$(printf '%s' "$r" | grep -q '^ok ' && echo 0 || echo 1)" "$r"
TF=$(printf '%s' "$r" | awk '{print $2}')
r=$(curl -s -m 3 "$XF_URL/next?flow=$TF&worker=w1")
check "claim timeout step" \
    "$(printf '%s' "$r" | grep -q '^ok quick$' && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/flow?id=$TF")
check "claim set deadline from timeout" \
    "$(printf '%s' "$r" | grep '^step' | awk -F '\t' '$2=="quick" && $3=="claimed" && $5>0 {ok=1} END {exit ok?0:1}' && echo 0 || echo 1)" "$r"
check "/flow shows timeout column" \
    "$(printf '%s' "$r" | grep '^step' | awk -F '\t' '$2=="quick" && $6=="2" {ok=1} END {exit ok?0:1}' && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/step?flow=$TF&id=quick" -d 'unclaim')
check "unclaim timeout step" \
    "$(printf '%s' "$r" | grep -q '^ok$' && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/flow?id=$TF")
check "unclaim reset the deadline clock" \
    "$(printf '%s' "$r" | grep '^step' | awk -F '\t' '$2=="quick" && $3=="pending" && $5=="0" {ok=1} END {exit ok?0:1}' && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/next?flow=$TF&worker=w1")
check "reclaim after unclaim" \
    "$(printf '%s' "$r" | grep -q '^ok quick$' && echo 0 || echo 1)" "$r"
sleep 3
r=$(curl -s -m 3 "$XF_URL/flow?id=$TF")
check "expired claim timeout -> overdue" \
    "$(printf '%s' "$r" | grep '^step' | awk -F '\t' '$2=="quick" && $3=="overdue" {ok=1} END {exit ok?0:1}' && echo 0 || echo 1)" "$r"

say ""
say "=== results: $PASS passed, $FAIL failed ==="
[ "$FAIL" = "0" ]
