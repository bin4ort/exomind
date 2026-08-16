#!/usr/bin/env bash
# exocontext test suite: private exomind (7660) + exocontext (7661)
# instances, temp data in /tmp. Covers digest composition (notes + state
# keys), budget capping, POST body, unknown agent, auth, and the
# self-describing spec.
set -u
cd "$(dirname "$0")"

ROOT="$(cd ../.. && pwd)"
XM_BIN=$ROOT/build/exomind
XC_BIN=$ROOT/exocontext/build/exocontext
XM_PORT=7660
XC_PORT=7661
XM_URL="http://127.0.0.1:$XM_PORT"
XC_URL="http://127.0.0.1:$XC_PORT"
WORK=/tmp/exocontext_test
XM_DATA=$WORK/exomind.dat
XM_LOG=$WORK/exomind.log
XC_LOG=$WORK/exocontext.log

PASS=0
FAIL=0
XM_PID=""
XC_PID=""

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

port_pid() {
    ss -tlnp 2>/dev/null | grep ":$1 " | grep -oP 'pid=\K[0-9]+' | head -1
}

kill_port() { # name, port
    local p
    p=$(port_pid "$2")
    [ -n "$p" ] && kill "$p" 2>/dev/null
}

mkdir -p "$WORK"
rm -f "$XM_DATA"

say ""
say "=== exocontext: setup ==="
kill_port exomind $XM_PORT
kill_port exocontext $XC_PORT
sleep 0.3
"$XM_BIN" --port $XM_PORT --data "$XM_DATA" > "$XM_LOG" 2>&1 &
XM_PID=$!
sleep 0.5
"$XC_BIN" --port $XC_PORT --exomind "$XM_URL" > "$XC_LOG" 2>&1 &
XC_PID=$!
sleep 0.5

check "exomind up" "$([ "$(curl -s -m 3 "$XM_URL/ping")" = pong ] && echo 0 || echo 1)"
check "exocontext up" "$([ "$(curl -s -m 3 "$XC_URL/ping")" = pong ] && echo 0 || echo 1)"

say ""
say "=== exocontext: spec ==="
r=$(curl -s -m 3 "$XC_URL/")
check "self-describing spec" "$(printf '%s' "$r" | grep -q 'GET /context?agent' && echo 0 || echo 1)" "$r"
check "spec mentions budget" "$(printf '%s' "$r" | grep -q 'budget' && echo 0 || echo 1)"

say ""
say "=== exocontext: digest composition ==="
r=$(curl -s -m 3 -X POST "$XM_URL/set?key=agent:alice:state" -d 'mid-sprint: docs green')
check "seed state key" "$([ "$r" = ok ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XM_URL/set?key=agent:alice:plan" -d '1) ship 2) party')
check "seed plan key" "$([ "$r" = ok ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XM_URL/set?key=agent:bob:state" -d 'bob is unrelated')
check "seed other-agent key" "$([ "$r" = ok ] && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 -X POST "$XM_URL/note" -d 'agent:alice checkpoint: tests green')
check "seed note mentioning alice" "$([ "${r#ok }" != "$r" ] && echo 0 || echo 1)" "$r"

r=$(curl -s -m 3 "$XC_URL/context?agent=alice&budget=2000")
check "digest header" "$(printf '%s' "$r" | grep -q 'context for alice' && echo 0 || echo 1)" "$r"
check "digest has note" "$(printf '%s' "$r" | grep -q 'agent:alice checkpoint: tests green' && echo 0 || echo 1)" "$r"
check "digest has state key" "$(printf '%s' "$r" | grep -q 'agent:alice:state' && echo 0 || echo 1)" "$r"
check "digest has plan key" "$(printf '%s' "$r" | grep -q 'agent:alice:plan' && echo 0 || echo 1)" "$r"
check "digest excludes other agent" "$(printf '%s' "$r" | grep -qv 'agent:bob:state' && echo 0 || echo 1)" "$r"
check "digest excludes bob's note" "$(printf '%s' "$r" | grep -qv 'bob is unrelated' && echo 0 || echo 1)" "$r"

say ""
say "=== exocontext: budget capping ==="
r=$(curl -s -m 3 "$XC_URL/context?agent=alice&budget=120")
B=$(printf '%s' "$r" | wc -c)
check "budget caps the digest" "$([ "$B" -le 400 ] && echo 0 || echo 1)" "len=$B"
check "tiny budget still emits header" "$(printf '%s' "$r" | grep -q 'context for alice' && echo 0 || echo 1)"

say ""
say "=== exocontext: POST body ==="
r=$(curl -s -m 3 -X POST "$XC_URL/context" -d 'agent=alice&budget=2000')
check "POST digest works" "$(printf '%s' "$r" | grep -q 'agent:alice:plan' && echo 0 || echo 1)" "$r"

say ""
say "=== exocontext: error paths ==="
check "missing agent rejected" "$([ "$(curl -s -m 3 "$XC_URL/context")" = 'error: missing agent' ] && echo 0 || echo 1)" \
    "$(curl -s -m 3 "$XC_URL/context")"
check "unknown agent yields empty digest" "$(printf '%s' "$(curl -s -m 3 "$XC_URL/context?agent=nobody")" | grep -q 'context for nobody' && echo 0 || echo 1)"
check "unknown path" "$([ "$(curl -s -m 3 "$XC_URL/nope")" = 'error: unknown path' ] && echo 0 || echo 1)"

say ""
say "=== exocontext: auth ==="
kill_port exocontext $XC_PORT
sleep 0.3
"$XC_BIN" --port $XC_PORT --exomind "$XM_URL" --token sekrit > "$XC_LOG" 2>&1 &
XC_PID=$!
sleep 0.5
check "no token rejected" "$([ "$(curl -s -m 3 -o /dev/null -w '%{http_code}' "$XC_URL/context?agent=alice")" = 401 ] && echo 0 || echo 1)"
check "wrong token rejected" "$([ "$(curl -s -m 3 -o /dev/null -w '%{http_code}' -H 'Authorization: Bearer nope' "$XC_URL/context?agent=alice")" = 401 ] && echo 0 || echo 1)"
r=$(curl -s -m 3 -H 'Authorization: Bearer sekrit' "$XC_URL/context?agent=alice&budget=2000")
check "right token works" "$(printf '%s' "$r" | grep -q 'agent:alice:state' && echo 0 || echo 1)" "$r"

say ""
say "=== exocontext: cleanup ==="
kill "$XC_PID" 2>/dev/null
kill "$XM_PID" 2>/dev/null
kill_port exocontext $XC_PORT
kill_port exomind $XM_PORT
sleep 0.3
check "ports released" "$([ -z "$(port_pid $XC_PORT)" ] && [ -z "$(port_pid $XM_PORT)" ] && echo 0 || echo 1)"
rm -rf "$WORK"

say ""
say "=== results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
