#!/usr/bin/env bash
# exoflow integration test: proves the orchestration loop end-to-end on a
# private, disposable stack (exomind 7674 + exosched 7675 + exoflow 7676,
# temp data in /tmp/b2-exoflow-int) driven by the contrib worker harness.
#
# Covers: diamond flow with deadline, 2-worker claim exclusivity + ordering
# (verified via note timestamps in the private exomind), step-deadline
# overdue + audit note, SIGKILL restart durability, Bearer-token auth,
# cleanup. Mirrors exosched/test/test.sh style; ends with the same
# `=== results: N passed, N failed ===` summary.
#
# The exoflow binary is taken from EXOFLOW_BIN (env) or
# <repo>/exoflow/build/exoflow. If exoflow/ is missing from the tree the
# script fetches the `feat/exoflow` branch from origin and merges it, then
# builds via `make exoflow` (root). Extra daemon flags can be injected
# with EXOFLOW_ARGS (defaults: --port 7676 --exomind ... --exosched ...).
set -u
cd "$(dirname "$0")"

ROOT="$(cd ../.. && pwd)"
XM_BIN=$ROOT/build/exomind
XS_BIN=$ROOT/exosched/build/exosched
XF_BIN=${EXOFLOW_BIN:-$ROOT/exoflow/build/exoflow}
WORKER=$ROOT/exoflow/contrib/worker.sh

XM_PORT=7674
XS_PORT=7675
XF_PORT=7676
XM_URL="http://127.0.0.1:$XM_PORT"
XS_URL="http://127.0.0.1:$XS_PORT"
XF_URL="http://127.0.0.1:$XF_PORT"
XF_ARGS=${EXOFLOW_ARGS:---port $XF_PORT --exomind "$XM_URL" --exosched "$XS_URL"}

WORK=/tmp/b2-exoflow-int
XM_DATA=$WORK/exomind.dat
XM_LOG=$WORK/exomind.log
XS_LOG=$WORK/exosched.log
XF_LOG=$WORK/exoflow.log
W1_LOG=$WORK/worker1.log
W2_LOG=$WORK/worker2.log

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
        say "FAIL  $1$([ -n "${3:-}" ] && printf '\n      %s' "$3")"
    fi
}

port_pid() { # returns pid listening on a port, or empty
    ss -tlnp 2>/dev/null | grep ":$1 " | grep -oP 'pid=\K[0-9]+' | head -1
}

stop_port() { # port - SIGTERM, wait up to ~2s
    local pid
    pid=$(port_pid "$1")
    [ -n "$pid" ] && kill "$pid" 2>/dev/null
    for _ in $(seq 1 10); do
        [ -z "$(port_pid "$1")" ] && break
        sleep 0.2
    done
}

kill9_port() { # port - SIGKILL whatever listens there (restart test)
    local pid
    pid=$(port_pid "$1")
    [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null
    sleep 0.3
}

step_marked() { # flow_state step lowercase-marker -> 0 when the step line
    # (TSV `step<TAB><sid><TAB>...`) contains the marker (case-insensitive);
    # tolerant of any compliant TSV layout
    printf '%s\n' "$1" | awk -F '\t' -v s="$2" -v m="$3" \
        '$1=="step" && $2==s && index(tolower($0), m) { found=1 } END { exit found ? 0 : 1 }'
}

cleanup() {
    [ -n "$XF_PID" ] && kill "$XF_PID" 2>/dev/null
    [ -n "$XS_PID" ] && kill "$XS_PID" 2>/dev/null
    [ -n "$XM_PID" ] && kill "$XM_PID" 2>/dev/null
    stop_port $XF_PORT
    stop_port $XS_PORT
    stop_port $XM_PORT
    rm -rf "$WORK"
}
trap cleanup EXIT

ensure_exoflow() {
    [ -x "$XF_BIN" ] && return 0
    if [ ! -d "$ROOT/exoflow" ] || [ ! -f "$ROOT/exoflow/Makefile" ]; then
        say "exoflow/ not in tree - fetching feat/exoflow from origin"
        (cd "$ROOT" && git fetch origin feat/exoflow && git merge --no-edit FETCH_HEAD) \
            || return 1
    fi
    (cd "$ROOT" && make exoflow) || return 1
    [ -x "$XF_BIN" ]
}

for bin in "$XM_BIN" "$XS_BIN"; do
    if [ ! -x "$bin" ]; then
        say "missing $bin - building"
        (cd "$ROOT" && make all && make exosched) || { say "build failed"; exit 1; }
    fi
done
ensure_exoflow || { say "missing exoflow binary - build first (make exoflow)"; exit 1; }
[ -x "$WORKER" ] || { say "missing $WORKER"; exit 1; }
command -v ss >/dev/null || { say "ss (iproute2) required"; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK"

# --- start private exomind + exosched + exoflow --------------------------
stop_port $XM_PORT
stop_port $XS_PORT
stop_port $XF_PORT
setsid nohup "$XM_BIN" --port $XM_PORT --data "$XM_DATA" \
    >"$XM_LOG" 2>&1 < /dev/null &
XM_PID=$!
sleep 0.5
setsid nohup "$XS_BIN" --port $XS_PORT --exomind "$XM_URL" \
    >"$XS_LOG" 2>&1 < /dev/null &
XS_PID=$!
sleep 0.8
# shellcheck disable=SC2086
setsid nohup "$XF_BIN" $XF_ARGS \
    >"$XF_LOG" 2>&1 < /dev/null &
XF_PID=$!
sleep 0.8

say "=== exoflow integration test suite ==="

r=$(curl -s -m 3 "$XF_URL/")
check "exoflow is up and self-describing" \
    "$(printf '%s' "$r" | grep -qi exoflow && printf '%s' "$r" | grep -q /next && echo 0 || echo 1)" \
    "first bytes: $(printf '%s' "$r" | head -c 120)"

# --- diamond flow with a deadline on s4 (now+10s) -------------------------
now=$(date +%s)
r=$(printf 'b2dia\ns1\tfirst\t\ns2\tsecond\ts1\ns3\tthird\ts1\ns4\tjoin\ts2,s3\t%s\ns5\tlast\ts4\n' \
    "$((now + 10))" | curl -s -m 3 -X POST "$XF_URL/flow" --data-binary @-)
fid=$(printf '%s' "$r" | awk '{print $2}')
check "POST /flow creates diamond, 'ok <fid> 5'" \
    "$(printf '%s' "$r" | grep -qE '^ok [^ ]+ 5$' && echo 0 || echo 1)" "$r"

r=$(curl -s -m 3 "$XF_URL/flows")
check "flow appears in /flows" \
    "$(printf '%s' "$r" | grep -q "$fid" && echo 0 || echo 1)" "$r"

# --- TWO workers in parallel through contrib/worker.sh ---------------------
# both park on `none` (-r) so they contend for steps at every diamond level
"$WORKER" -u "$XF_URL" -f "$fid" -w w1 -e "$XM_URL" -s 0.5 -r 10 >"$W1_LOG" 2>&1 &
W1_PID=$!
"$WORKER" -u "$XF_URL" -f "$fid" -w w2 -e "$XM_URL" -s 0.5 -r 10 >"$W2_LOG" 2>&1 &
W2_PID=$!
deadline=$((SECONDS + 90))
while kill -0 "$W1_PID" 2>/dev/null || kill -0 "$W2_PID" 2>/dev/null; do
    [ "$SECONDS" -lt "$deadline" ] || break
    sleep 0.5
done
alive=""
kill -0 "$W1_PID" 2>/dev/null && alive="w1"
kill -0 "$W2_PID" 2>/dev/null && alive="$alive w2"
[ -n "$alive" ] && kill "$W1_PID" "$W2_PID" 2>/dev/null
wait "$W1_PID" 2>/dev/null; rc1=$?
wait "$W2_PID" 2>/dev/null; rc2=$?
check "both workers finish the diamond (rc 0)" \
    "$( [ $rc1 -eq 0 ] && [ $rc2 -eq 0 ] && [ -z "$alive" ] && echo 0 || echo 1)" \
    "rc1=$rc1 rc2=$rc2 alive='$alive'"
say "    w1: $(tr '\n' '; ' < "$W1_LOG")"
say "    w2: $(tr '\n' '; ' < "$W2_LOG")"

# --- claim exclusivity: no step claimed by both workers --------------------
awk '/claimed/ {print $NF}' "$W1_LOG" | sort -u >"$WORK/w1c"
awk '/claimed/ {print $NF}' "$W2_LOG" | sort -u >"$WORK/w2c"
shared=$(comm -12 "$WORK/w1c" "$WORK/w2c" | tr '\n' ' ')
check "claim exclusivity: no step claimed by both workers" \
    "$([ -z "$shared" ] && echo 0 || echo 1)" "shared: '$shared'"

# --- all steps done, in /flow state ----------------------------------------
r=$(curl -s -m 3 "$XF_URL/flow?id=$fid")
all_done=0
for s in s1 s2 s3 s4 s5; do
    step_marked "$r" "$s" done || { all_done=1; break; }
done
check "all 5 steps completed in /flow state" \
    "$([ "$all_done" = 0 ] && echo 0 || echo 1)" "$(printf '%s\n' "$r" | head -8)"

# --- ordering: s4 claimed only after s2 AND s3 done (note timestamps) ------
# worker notes are "FID|STEP|ACT|WORKER" (exomind escapes control chars, so
# '|' separators; the note key itself carries the epoch)
order_ok=$(curl -s -m 3 "$XM_URL/notes?q=$fid&limit=200" | awk -F '|' -v f="$fid" '
    $1 ~ /^note:[0-9]+:/ && index($1, f) {
        epoch=$1; sub(/^note:/,"",epoch); sub(/:.*/,"",epoch)
        if ($2=="s2" && $3=="done") e2=epoch
        if ($2=="s3" && $3=="done") e3=epoch
        if ($2=="s4" && $3=="claimed") ec4=epoch
    }
    END { if (e2 && e3 && ec4 && ec4 >= e2 && ec4 >= e3) print 0; else print 1 }')
check "ordering: s4 claimed after s2 and s3 done (note epochs)" \
    "$order_ok" \
    "$(curl -s -m 3 "$XM_URL/notes?q=$fid&limit=200" | grep -E '\|s4\||done' | head -6)"

# --- SIGKILL restart durability ---------------------------------------------
r=$(printf 'b2lin\nl1\tlin-one\t\nl2\tlin-two\tl1\n' \
    | curl -s -m 3 -X POST "$XF_URL/flow" --data-binary @-)
lfid=$(printf '%s' "$r" | awk '{print $2}')
r=$(curl -s -m 3 "$XF_URL/next?flow=$lfid&worker=wR")
check "linear flow: /next hands out l1" \
    "$(printf '%s' "$r" | grep -qE '^ok l1$' && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XF_URL/step?flow=$lfid&id=l1" -d "done wR")
check "linear flow: l1 done" "$([ "$r" = "ok" ] && echo 0 || echo 1)" "$r"

kill -9 "$XF_PID" 2>/dev/null
kill9_port $XF_PORT
setsid nohup "$XF_BIN" $XF_ARGS >"$XF_LOG" 2>&1 < /dev/null &
XF_PID=$!
sleep 1.5
r=$(curl -s -m 3 "$XF_URL/flow?id=$lfid")
check "SIGKILL restart: l1 state intact (persisted in exomind)" \
    "$(step_marked "$r" l1 done && echo 0 || echo 1)" \
    "$(printf '%s\n' "$r" | head -3)"
"$WORKER" -u "$XF_URL" -f "$lfid" -w wR -s 0.2 >"$WORK/wr.log" 2>&1
rcw=$?
check "restart: flow still completable (worker finishes l2)" \
    "$([ $rcw -eq 0 ] && grep -q "done l2" "$WORK/wr.log" && echo 0 || echo 1)" \
    "rc=$rcw log=$(cat "$WORK/wr.log")"

# --- step deadline: now+2s, wait 3s -> overdue + audit note -----------------
now=$(date +%s)
r=$(printf 'b2dl\nd1\tdeadline-check\t\t%s\n' "$((now + 2))" \
    | curl -s -m 3 -X POST "$XF_URL/flow" --data-binary @-)
dfid=$(printf '%s' "$r" | awk '{print $2}')
sleep 3
r=$(curl -s -m 3 "$XF_URL/flow?id=$dfid")
check "step deadline: /flow marks d1 overdue" \
    "$(step_marked "$r" d1 overdue && echo 0 || echo 1)" \
    "$(printf '%s\n' "$r" | head -3)"
r=$(curl -s -m 3 "$XM_URL/notes?q=$dfid")
check "step deadline: audit note exists in exomind" \
    "$(printf '%s' "$r" | grep -qiE 'overdue|missed|deadline|late' && echo 0 || echo 1)" \
    "$(printf '%s' "$r" | head -3)"

# --- auth: --token ------------------------------------------------------------
kill "$XF_PID" 2>/dev/null
stop_port $XF_PORT
setsid nohup "$XF_BIN" $XF_ARGS --token sekrit >"$XF_LOG" 2>&1 < /dev/null &
XF_PID=$!
sleep 0.8
a1=$(curl -s -m 3 -o /dev/null -w '%{http_code}' "$XF_URL/")
a2=$(curl -s -m 3 -o /dev/null -w '%{http_code}' -H "Authorization: Bearer wrong" "$XF_URL/")
a3=$(curl -s -m 3 -o /dev/null -w '%{http_code}' -H "Authorization: Bearer sekrit" "$XF_URL/")
a4=$(curl -s -m 3 -o /dev/null -w '%{http_code}' "$XF_URL/next?flow=$lfid&worker=anon")
a5=$(curl -s -m 3 -o /dev/null -w '%{http_code}' -H "Authorization: Bearer sekrit" \
    "$XF_URL/next?flow=$lfid&worker=wA")
check "auth: no/wrong token -> 401, right token works" \
    "$( [ "$a1" = "401" ] && [ "$a2" = "401" ] && [ "$a3" = "200" ] && [ "$a4" = "401" ] && [ "$a5" = "200" ] && echo 0 || echo 1)" \
    "a1=$a1 a2=$a2 a3=$a3 a4=$a4 a5=$a5"
kill "$XF_PID" 2>/dev/null
stop_port $XF_PORT
setsid nohup "$XF_BIN" $XF_ARGS >"$XF_LOG" 2>&1 < /dev/null &
XF_PID=$!
sleep 0.8
r=$(curl -s -m 3 "$XF_URL/")
check "auth: back to no-token mode after restart" \
    "$(printf '%s' "$r" | grep -qi exoflow && echo 0 || echo 1)" "$(printf '%s' "$r" | head -c 60)"

# --- cleanup ------------------------------------------------------------------
kill "$XF_PID" 2>/dev/null
stop_port $XF_PORT
kill "$XS_PID" 2>/dev/null
stop_port $XS_PORT
kill "$XM_PID" 2>/dev/null
stop_port $XM_PORT
XF_PID=""; XS_PID=""; XM_PID=""
check "cleanup: all three ports released" \
    "$([ -z "$(port_pid $XF_PORT)$(port_pid $XS_PORT)$(port_pid $XM_PORT)" ] && echo 0 || echo 1)" \
    "x=( $(port_pid $XF_PORT) ) s=( $(port_pid $XS_PORT) ) m=( $(port_pid $XM_PORT) )"
rm -rf "$WORK"
check "cleanup: temp data removed" "$([ ! -e "$WORK" ] && echo 0 || echo 1)"

say ""
say "=== results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
