#!/usr/bin/env bash
# exosched test suite: private exomind (port 7660) + exosched (port 7661)
# instances, temp data in /tmp. Covers schedule/fire, cancel, restart
# persistence, WebSocket push, close handling, auth, garbage, cleanup.
set -u
cd "$(dirname "$0")"

ROOT="$(cd ../.. && pwd)"
XM_BIN=$ROOT/build/exomind
XS_BIN=$ROOT/exosched/build/exosched
XM_PORT=7660
XS_PORT=7661
XM_URL="http://127.0.0.1:$XM_PORT"
XS_URL="http://127.0.0.1:$XS_PORT"
WORK=/tmp/exosched_test
XM_DATA=$WORK/exomind.dat
XM_LOG=$WORK/exomind.log
XS_LOG=$WORK/exosched.log
WS_LOG=$WORK/ws.log

PASS=0
FAIL=0
XM_PID=""
XS_PID=""

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
    [ -n "$XS_PID" ] && kill "$XS_PID" 2>/dev/null
    [ -n "$XM_PID" ] && kill "$XM_PID" 2>/dev/null
    stop_port $XS_PORT
    stop_port $XM_PORT
}
trap cleanup EXIT

for bin in "$XM_BIN" "$XS_BIN"; do
    if [ ! -x "$bin" ]; then
        say "missing $bin - build first (make -C .. exosched / make)"
        exit 1
    fi
done
command -v python3 >/dev/null || { say "python3 required"; exit 1; }
command -v ss >/dev/null || { say "ss (iproute2) required"; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK"

# --- start private exomind + exosched -----------------------------------
stop_port $XM_PORT
stop_port $XS_PORT
setsid nohup "$XM_BIN" --port $XM_PORT --data "$XM_DATA" \
    >"$XM_LOG" 2>&1 < /dev/null &
XM_PID=$!
sleep 0.5
setsid nohup "$XS_BIN" --port $XS_PORT --exomind "$XM_URL" \
    >"$XS_LOG" 2>&1 < /dev/null &
XS_PID=$!
sleep 0.8

say "=== exosched test suite ==="

r=$(curl -s -m 3 "$XS_URL/ping")
check "ping" "$([ "$r" = "pong" ] && echo 0 || echo 1)" "got: $r"

r=$(curl -s -m 3 "$XS_URL/")
check "GET / is self-describing" \
    "$(printf '%s' "$r" | grep -q exosched && printf '%s' "$r" | grep -q /remind && echo 0 || echo 1)"

# --- schedule + fire on time --------------------------------------------
r=$(curl -s -m 3 -X POST "$XS_URL/remind" -d 'in 1s "fires-on-time"')
id1=$(echo "$r" | awk '{print $2}')
epoch1=$(echo "$r" | awk '{print $3}')
now=$(date +%s)
check "remind replies 'ok <id> <epoch>'" \
    "$(printf '%s' "$r" | grep -qE '^ok [0-9]+:[0-9a-f]+ [0-9]+$' && echo 0 || echo 1)" "$r"
check "epoch is ~now+1s" \
    "$( [ $((epoch1 - now)) -ge 0 ] && [ $((epoch1 - now)) -le 2 ] && echo 0 || echo 1)" "epoch1=$epoch1 now=$now"
sleep 2.5
r=$(curl -s -m 3 "$XS_URL/timers")
check "fired timer leaves /timers" \
    "$(printf '%s' "$r" | grep -q "$id1" && echo 1 || echo 0)"
r=$(curl -s -m 3 "$XM_URL/notes?q=fires-on-time")
check "fire is logged as exomind note" \
    "$(printf '%s' "$r" | grep -q "fired timer $id1" && echo 0 || echo 1)" "$r"

# --- parse variants: in 5m -------------------------------------------------
r=$(curl -s -m 3 -X POST "$XS_URL/remind" -d 'in 5m "parse-check"')
idm=$(echo "$r" | awk '{print $2}')
epochm=$(echo "$r" | awk '{print $3}')
now=$(date +%s)
check "in 5m parses to ~now+300s" \
    "$( [ $((epochm - now)) -ge 295 ] && [ $((epochm - now)) -le 305 ] && echo 0 || echo 1)" "epochm=$epochm now=$now"

# --- timers listing ---------------------------------------------------------
r=$(curl -s -m 3 -X POST "$XS_URL/remind" -d 'in 60s "listed timer"')
idl=$(echo "$r" | awk '{print $2}')
r=$(curl -s -m 3 "$XS_URL/timers")
line=$(printf '%s\n' "$r" | grep "$idl")
check "/timers tsv: id<TAB>epoch<TAB>remaining<TAB>msg" \
    "$(printf '%s' "$line" | awk -F '\t' -v id="$idl" 'NF==4 && $1==id && $4=="listed timer" {ok=1} END {exit ok?0:1}' && echo 0 || echo 1)" "$line"
r=$(curl -s -m 3 "$XS_URL/timers?json=1")
check "/timers?json=1 is valid JSON" \
    "$(printf '%s' "$r" | python3 -c "
import json,sys
d=json.load(sys.stdin)
sys.exit(0 if any(t['message']=='listed timer' for t in d) else 1)" && echo 0 || echo 1)" "$r"

# --- cancel ------------------------------------------------------------------
r=$(curl -s -m 3 -X POST "$XS_URL/remind" -d 'in 60s "cancel me"')
idc=$(echo "$r" | awk '{print $2}')
r1=$(curl -s -m 3 -X DELETE "$XS_URL/timer?id=$idc")
r2=$(curl -s -m 3 -X DELETE "$XS_URL/timer?id=$idc")
check "DELETE /timer answers ok then missing" \
    "$( [ "$r1" = "ok" ] && printf '%s' "$r2" | grep -q missing && echo 0 || echo 1)" "r1='$r1' r2='$r2'"
r=$(curl -s -m 3 "$XM_URL/list?prefix=exosched:timer:")
check "cancelled timer's exomind key is gone" \
    "$(printf '%s' "$r" | grep -q "$idc" && echo 1 || echo 0)"
r=$(curl -s -m 3 "$XS_URL/timers")
check "cancelled timer leaves /timers" \
    "$(printf '%s' "$r" | grep -q "$idc" && echo 1 || echo 0)"

# --- websocket push ----------------------------------------------------------
python3 wsclient.py $XS_PORT ws-push-live 10 >"$WS_LOG" 2>&1 &
WS_PID=$!
sleep 0.5
r=$(curl -s -m 3 -X POST "$XS_URL/remind" -d 'in 2s "ws-push-live"')
idw=$(echo "$r" | awk '{print $2}')
epochw=$(echo "$r" | awk '{print $3}')
wait "$WS_PID" 2>/dev/null
wsrc=$?
check "websocket receives 'timer <id> <epoch> <msg>' on fire" \
    "$( [ $wsrc -eq 0 ] && grep -q "timer $idw $epochw ws-push-live" "$WS_LOG" && echo 0 || echo 1)" \
    "rc=$wsrc log=$(cat "$WS_LOG")"

# --- close frame handling -----------------------------------------------------
python3 wsclient.py $XS_PORT CLOSE 6 >"$WS_LOG" 2>&1 &
WS_PID=$!
wait "$WS_PID" 2>/dev/null
check "server answers close frame" "$([ $? -eq 0 ] && echo 0 || echo 1)"

# --- persistence across restart ------------------------------------------------
r=$(curl -s -m 3 -X POST "$XS_URL/remind" -d 'in 12s "survive-restart"')
idp=$(echo "$r" | awk '{print $2}')
epochp=$(echo "$r" | awk '{print $3}')
kill "$XS_PID" 2>/dev/null
stop_port $XS_PORT
sleep 0.3
setsid nohup "$XS_BIN" --port $XS_PORT --exomind "$XM_URL" \
    >"$XS_LOG" 2>&1 < /dev/null &
XS_PID=$!
sleep 1
r=$(curl -s -m 3 "$XS_URL/timers")
check "timer survives exosched restart (reloaded from exomind)" \
    "$(printf '%s' "$r" | awk -F '\t' -v id="$idp" -v ep="$epochp" '$1==id && $2==ep {ok=1} END {exit ok?0:1}' && echo 0 || echo 1)" "$r"
python3 wsclient.py $XS_PORT survive-restart 15 >"$WS_LOG" 2>&1 &
WS_PID=$!
wait "$WS_PID" 2>/dev/null
check "restarted timer still fires + ws push" \
    "$( [ $? -eq 0 ] && grep -q "timer $idp $epochp survive-restart" "$WS_LOG" && echo 0 || echo 1)" "$(cat "$WS_LOG")"
r=$(curl -s -m 3 "$XM_URL/notes?q=survive-restart")
check "restarted fire logged in exomind" \
    "$(printf '%s' "$r" | grep -q "fired timer $idp" && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XM_URL/list?prefix=exosched:timer:")
check "fired/cancelled keys cleaned up in exomind" \
    "$(printf '%s' "$r" | grep -qE "^exosched:timer:($id1|$idc|$idp|$idw)$" && echo 1 || echo 0)" "$r"

# --- missed timer logged on reload -------------------------------------------------
r=$(curl -s -m 3 -X POST "$XS_URL/remind" -d 'in 3s "missed-check"')
idm2=$(echo "$r" | awk '{print $2}')
kill "$XS_PID" 2>/dev/null
stop_port $XS_PORT
sleep 0.2
sleep 4
setsid nohup "$XS_BIN" --port $XS_PORT --exomind "$XM_URL" \
    >"$XS_LOG" 2>&1 < /dev/null &
XS_PID=$!
sleep 1.5
r=$(curl -s -m 3 "$XM_URL/notes?q=missed-check")
check "overdue timer logged as missed note on reload" \
    "$(printf '%s' "$r" | grep -q "missed timer $idm2" && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XM_URL/list?prefix=exosched:timer:")
check "missed timer's key dropped" \
    "$(printf '%s' "$r" | grep -q "$idm2" && echo 1 || echo 0)"

# --- garbage / robustness ------------------------------------------------------
g1=$(curl -s -m 3 -o /dev/null -w '%{http_code}' -X POST "$XS_URL/remind" -d 'garbage')
g2=$(curl -s -m 3 -o /dev/null -w '%{http_code}' -X POST "$XS_URL/remind" -d 'in xyz "bad"')
g3=$(curl -s -m 3 -o /dev/null -w '%{http_code}' -X POST "$XS_URL/remind" -d 'in 5m')
g4=$(curl -s -m 3 -o /dev/null -w '%{http_code}' "$XS_URL/nonsense")
g5=$(curl -s -m 3 -o /dev/null -w '%{http_code}' -X DELETE "$XS_URL/timer")
check "garbage input -> 400/404, no crash" \
    "$( [ "$g1" = "400" ] && [ "$g2" = "400" ] && [ "$g3" = "400" ] && [ "$g4" = "404" ] && [ "$g5" = "400" ] && echo 0 || echo 1)" \
    "g1=$g1 g2=$g2 g3=$g3 g4=$g4 g5=$g5"
head -c 2000000 /dev/zero | tr '\0' 'a' >"$WORK/big.bin"
g6=$(curl -s -m 3 -o /dev/null -w '%{http_code}' -X POST "$XS_URL/remind" --data-binary @"$WORK/big.bin")
check "oversized body -> 413" "$([ "$g6" = "413" ] && echo 0 || echo 1)" "g6=$g6"
g7=$(curl -s -m 3 -o /dev/null -w '%{http_code}' "$XS_URL/ws")
check "non-upgrade GET /ws -> 400" "$([ "$g7" = "400" ] && echo 0 || echo 1)" "g7=$g7"
r=$(curl -s -m 3 "$XS_URL/ping")
check "daemon alive after fuzz" "$([ "$r" = "pong" ] && echo 0 || echo 1)"

# --- auth -----------------------------------------------------------------------
kill "$XS_PID" 2>/dev/null
stop_port $XS_PORT
setsid nohup "$XS_BIN" --port $XS_PORT --exomind "$XM_URL" --token sekrit \
    >"$XS_LOG" 2>&1 < /dev/null &
XS_PID=$!
sleep 0.8
a1=$(curl -s -m 3 -o /dev/null -w '%{http_code}' "$XS_URL/")
a2=$(curl -s -m 3 -o /dev/null -w '%{http_code}' -H "Authorization: Bearer wrong" "$XS_URL/")
a3=$(curl -s -m 3 -o /dev/null -w '%{http_code}' -H "Authorization: Bearer sekrit" "$XS_URL/")
check "auth: no/wrong token -> 401, right token -> 200" \
    "$( [ "$a1" = "401" ] && [ "$a2" = "401" ] && [ "$a3" = "200" ] && echo 0 || echo 1)" \
    "a1=$a1 a2=$a2 a3=$a3"
r=$(curl -s -m 3 -H "Authorization: Bearer sekrit" "$XS_URL/ping")
check "authed ping" "$([ "$r" = "pong" ] && echo 0 || echo 1)" "$r"

# --- shutdown graceful ----------------------------------------------------------
kill "$XS_PID" 2>/dev/null
for _ in $(seq 1 20); do
    port_pid $XS_PORT >/dev/null || break
    sleep 0.2
done
check "graceful SIGTERM shutdown" "$([ -z "$(port_pid $XS_PORT)" ] && echo 0 || echo 1)"

say ""
say "=== results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
