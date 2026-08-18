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
"$XC_BIN" --serve --port $XC_PORT --exomind "$XM_URL" > "$XC_LOG" 2>&1 &
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
say "=== exocontext: console ops (no daemon) ==="
G=$(timeout 5 "$XC_BIN" 2>/dev/null)
check "console guide mentions exocontext" "$(printf '%s' "$G" | grep -q 'exocontext' && echo 0 || echo 1)" "$G"
check "console guide exit 0" "$(timeout 5 "$XC_BIN" >/dev/null 2>&1 && echo 0 || echo 1)" ""
r=$(timeout 5 "$XC_BIN" "/context?agent=alice&budget=2000" --exomind "$XM_URL" 2>/dev/null)
check "console /context works" "$(printf '%s' "$r" | grep -q 'agent:alice:state' && echo 0 || echo 1)" "$r"
check "console /context exit 0" "$(timeout 5 "$XC_BIN" "/context?agent=alice" --exomind "$XM_URL" >/dev/null 2>&1 && echo 0 || echo 1)" ""
r=$(timeout 5 "$XC_BIN" /context --exomind "$XM_URL" --body "agent=alice&budget=2000" 2>/dev/null)
check "console /context --body" "$(printf '%s' "$r" | grep -q 'agent:alice:plan' && echo 0 || echo 1)" "$r"
"$XC_BIN" /context --exomind "$XM_URL" >/dev/null 2>&1
check "console missing agent exit 1" "$([ $? -eq 1 ] && echo 0 || echo 1)" ""
"$XC_BIN" /nope 2>/dev/null
check "console unknown op exit 2" "$([ $? -eq 2 ] && echo 0 || echo 1)" ""

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
say "=== exocontext: auto-compression (long sessions) ==="
kill_port exocontext $XC_PORT
sleep 0.3
EXO_CTX_BUDGET=200 "$XC_BIN" --serve --port $XC_PORT --exomind "$XM_URL" \
    > "$XC_LOG" 2>&1 &
XC_PID=$!
sleep 0.5
check "compression daemon up" "$([ "$(curl -s -m 3 "$XC_URL/ping")" = pong ] && echo 0 || echo 1)"

seed_carol() { # 10 log entries (~366 bytes) vs the 200-byte budget
    local i v
    for i in 0001 0002 0003 0004 0005 0006 0007 0008 0009 0010; do
        case $i in
        0001) v="state: sprint 1 shipped";; 0002) v="decided: ship v1";;
        0003) v="state: docs green";; 0004) v="noise: plain log line";;
        0005) v="state: budget spent";; 0006) v="decided: cut scope";;
        0007) v="state: ui frozen";; 0008) v="decided: keep rpc";;
        0009) v="state: tests passing";; 0010) v="decided: resume now";;
        esac
        curl -s -m 3 -X POST "$XM_URL/set?key=agent:carol:$i" -d "$v" > /dev/null
    done
}
seed_carol
r=$(curl -s -m 3 "$XC_URL/context?agent=carol") # GET triggers auto-compress
S=$(curl -s -m 3 "$XM_URL/get?key=agent:carol:summary")
check "big session compressed (summary exists)" "$(printf '%s' "$S" | grep -q '# summary 5 entries' && echo 0 || echo 1)" "$S"
check "summary records compression timestamp" "$(printf '%s' "$S" | grep -qE 'compressed at [0-9]+' && echo 0 || echo 1)" "$S"
check "summary counts folded entries" "$(printf '%s' "$S" | grep -q '(+5 entries compressed)' && echo 0 || echo 1)" "$S"
check "summary carries decisions forward" "$(printf '%s' "$S" | grep -q 'decided: ship v1' && echo 0 || echo 1)" "$S"
check "summary carries state forward" "$(printf '%s' "$S" | grep -q 'state: docs green' && echo 0 || echo 1)" "$S"
check "summary drops plain log lines" "$(printf '%s' "$S" | grep -qv 'noise' && echo 0 || echo 1)" "$S"
check "live tail trimmed (oldest gone)" "$([ "$(curl -s -m 3 "$XM_URL/get?key=agent:carol:0001")" = missing ] && echo 0 || echo 1)"
check "live tail kept (newest stays)" "$(curl -s -m 3 "$XM_URL/get?key=agent:carol:0009" | grep -q 'state: tests passing' && echo 0 || echo 1)"
check "compression marker recorded" "$(curl -s -m 3 "$XM_URL/get?key=ctx:summary:carol" | grep -qE '^[0-9]+$' && echo 0 || echo 1)"

say ""
say "=== exocontext: re-compression merges ==="
for i in 0011 0012 0013; do
    case $i in
    0011) v="decided: ship v2";; 0012) v="state: smoke green";; 0013) v="decided: tag rc";;
    esac
    curl -s -m 3 -X POST "$XM_URL/set?key=agent:carol:$i" -d "$v" > /dev/null
done
curl -s -m 3 "$XC_URL/context?agent=carol" > /dev/null
S=$(curl -s -m 3 "$XM_URL/get?key=agent:carol:summary")
check "re-compression accumulates counts" "$(printf '%s' "$S" | grep -q '# summary 8 entries' && echo 0 || echo 1)" "$S"
check "re-compression records increment" "$(printf '%s' "$S" | grep -q '(+3 entries compressed)' && echo 0 || echo 1)" "$S"

say ""
say "=== exocontext: re-expansion on resume ==="
r=$(curl -s -m 3 "$XC_URL/context?agent=carol&budget=2000")
check "resume re-expands summary" "$(printf '%s' "$r" | grep -q '# summary 8 entries' && echo 0 || echo 1)" "$r"
check "resume shows folded decisions" "$(printf '%s' "$r" | grep -q 'decided: ship v1' && echo 0 || echo 1)" "$r"
check "resume shows folded state" "$(printf '%s' "$r" | grep -q 'state: sprint 1 shipped' && echo 0 || echo 1)" "$r"
check "resume shows live tail" "$(printf '%s' "$r" | grep -q 'decided: tag rc' && echo 0 || echo 1)" "$r"
SN=$(printf '%s\n' "$r" | grep -n '## summary' | head -1 | cut -d: -f1)
LN=$(printf '%s\n' "$r" | grep -n 'agent:carol:0013' | head -1 | cut -d: -f1)
check "summary precedes live tail" "$([ -n "$SN" ] && [ -n "$LN" ] && [ "$SN" -lt "$LN" ] && echo 0 || echo 1)" "summary@$SN live@$LN"

say ""
say "=== exocontext: small sessions untouched ==="
curl -s -m 3 -X POST "$XM_URL/set?key=agent:dave:plan" -d 'state: keep it small' > /dev/null
r=$(curl -s -m 3 "$XC_URL/context?agent=dave")
check "small session digest works" "$(printf '%s' "$r" | grep -q 'agent:dave:plan' && echo 0 || echo 1)" "$r"
check "small session has no summary key" "$([ "$(curl -s -m 3 "$XM_URL/get?key=agent:dave:summary")" = missing ] && echo 0 || echo 1)"
check "small session has no marker" "$([ "$(curl -s -m 3 "$XM_URL/get?key=ctx:summary:dave")" = missing ] && echo 0 || echo 1)"

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
