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
sleep 0.8
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
sleep 0.8
r=$(curl -s -m 3 "$XF_URL/flow?id=$F6")
check "deadline expired while down -> overdue on reload" \
    "$(printf '%s' "$r" | grep '^step' | awk -F '\t' '$2=="soon" && $3=="overdue" {ok=1} END {exit ok?0:1}' && echo 0 || echo 1)" "$r"

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
sleep 1.5
r=$(curl -s -m 3 -X POST "$XF_URL/flow" -d $'retry ok\nx\tfirst\t')
check "exomind up: create works after background reload" \
    "$(printf '%s' "$r" | grep -qE '^ok ' && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XF_URL/flow?id=$F5")
check "background reload picked up existing flows" \
    "$(printf '%s' "$r" | grep -q "$F5" && echo 0 || echo 1)" "$r"
kill "$XM_PID" 2>/dev/null
XM_PID=""
stop_port $DEAD_PORT

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
r=$(curl -s -m 3 -H "Authorization: Bearer supersecret" "$AUTH_URL/flows")
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
say ""
say "=== results: $PASS passed, $FAIL failed ==="
[ "$FAIL" = "0" ]
