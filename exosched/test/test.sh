#!/usr/bin/env bash
# exosched test suite: private exomind (port 7660) + exosched (port 7661)
# instances, temp data in /tmp. Covers schedule/fire, cancel, restart
# persistence, WebSocket push, close handling, auth, garbage, console
# one-shot ops, cleanup.
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

# --- console surface: one-shot ops (no server, no socket) -----------------
g=$("$XS_BIN" 2>/dev/null)
check "console no-args prints the guide and exits" \
    "$(printf '%s' "$g" | grep -q exosched && printf '%s' "$g" | grep -q /remind && echo 0 || echo 1)" \
    "$(printf '%s' "$g" | head -1)"

r=$("$XS_BIN" /remind --body 'in 30m "console-remind"' --exomind "$XM_URL" 2>/dev/null)
cid=$(echo "$r" | awk '{print $2}')
check "console op /remind (POST, --body) replies 'ok <id> <epoch>'" \
    "$(printf '%s' "$r" | grep -qE '^ok [0-9]+:[0-9a-f]+ [0-9]+$' && echo 0 || echo 1)" "$r"

r=$("$XS_BIN" /timers --exomind "$XM_URL" 2>/dev/null)
line=$(printf '%s\n' "$r" | grep "$cid")
check "console op /timers lists the console-scheduled timer" \
    "$(printf '%s' "$line" | awk -F '\t' -v id="$cid" 'NF==4 && $1==id && $4=="console-remind" {ok=1} END {exit ok?0:1}' && echo 0 || echo 1)" "$line"

r=$(printf 'in 30m "pipe-remind"' | "$XS_BIN" /remind --exomind "$XM_URL" 2>/dev/null)
check "console op /remind reads the body from stdin (pipe)" \
    "$(printf '%s' "$r" | grep -qE '^ok [0-9]+:[0-9a-f]+ [0-9]+$' && echo 0 || echo 1)" "$r"
pid=$(echo "$r" | awk '{print $2}')

r=$("$XS_BIN" /timer?id=bogus123 --exomind "$XM_URL" 2>/dev/null)
rc=$?
check "console op /timer?id=missing exits 1" "$([ "$rc" = 1 ] && echo 0 || echo 1)" "rc=$rc body='$r'"

r=$("$XS_BIN" /nonsense --exomind "$XM_URL" 2>/dev/null)
rc=$?
check "console op unknown path exits 1" "$([ "$rc" = 1 ] && echo 0 || echo 1)" "rc=$rc body='$r'"

r=$("$XS_BIN" /timer?id=$cid --exomind "$XM_URL" 2>/dev/null)
check "console op /timer?id= cancels (cleanup)" \
    "$([ "$r" = "ok" ] && echo 0 || echo 1)" "$r"
r=$("$XS_BIN" /timer?id=$pid --exomind "$XM_URL" 2>/dev/null)
check "console op /timer?id= cancels the piped timer (cleanup)" \
    "$([ "$r" = "ok" ] && echo 0 || echo 1)" "$r"

XS2_PORT=$((20000 + $$ % 10000))
setsid nohup "$XS_BIN" --serve --port "$XS2_PORT" --exomind "$XM_URL" \
    >"$XS_LOG" 2>&1 < /dev/null &
XS2_PID=$!
r=""
for _ in $(seq 1 40); do
    r=$(curl -s -m 2 "http://127.0.0.1:$XS2_PORT/ping" 2>/dev/null)
    [ "$r" = "pong" ] && break
    r=""
    sleep 0.1
done
check "--serve with --port binds and answers /ping" \
    "$([ "$r" = "pong" ] && echo 0 || echo 1)" "r='$r'"
kill "$XS2_PID" 2>/dev/null
stop_port $XS2_PORT

v=$("$XS_BIN" --version)
check "console --version still works" \
    "$(printf '%s' "$v" | grep -qE '^exosched v[0-9]' && echo 0 || echo 1)" "$v"

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
sleep 0.7 # the ack window closes before the fire note lands
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

# --- recurring timers (0.2.0): every / until / cancel / restart -------------
r=$(curl -s -m 3 -X POST "$XS_URL/remind" -d 'every 2s "cadence-check"')
idr=$(echo "$r" | awk '{print $2}')
epr=$(echo "$r" | awk '{print $3}')
now=$(date +%s)
check "every 2s replies 'ok <id> <epoch ~now+2>'" \
    "$(printf '%s' "$r" | grep -qE '^ok [0-9]+:[0-9a-f]+ [0-9]+$' && [ $((epr - now)) -ge 1 ] && [ $((epr - now)) -le 3 ] && echo 0 || echo 1)" "$r"
sleep 5.5
r=$(curl -s -m 3 "$XM_URL/notes?q=cadence-check")
nf=$(printf '%s' "$r" | grep -c "fired timer $idr")
check "every 2s fires at least twice in 5.5s" \
    "$([ "$nf" -ge 2 ] && echo 0 || echo 1)" "fires=$nf"
ep1=$(printf '%s' "$r" | grep "fired timer $idr" | grep -oP 'at \K[0-9]+(?= \()' | head -1)
ep2=$(printf '%s' "$r" | grep "fired timer $idr" | grep -oP 'at \K[0-9]+(?= \()' | tail -1)
gap=$((ep1 - ep2))
check "recurring fires keep ~2s cadence (measured from note epochs)" \
    "$([ "$gap" -ge 1 ] && [ "$gap" -le 3 ] && echo 0 || echo 1)" "gap=$gap (ep1=$ep1 ep2=$ep2)"

r=$(curl -s -m 3 -X POST "$XS_URL/remind" -d 'in 60s "tsv-oneshot"')
ido=$(echo "$r" | awk '{print $2}')
r=$(curl -s -m 3 "$XS_URL/timers")
l1=$(printf '%s\n' "$r" | grep "$idr")
l2=$(printf '%s\n' "$r" | grep "$ido")
check "recurring /timers tsv: repeat_s+until appended (6 fields)" \
    "$(printf '%s' "$l1" | awk -F '\t' -v id="$idr" 'NF==6 && $1==id && $5==2 && $6==0 {ok=1} END {exit ok?0:1}' && echo 0 || echo 1)" "$l1"
check "one-shot /timers tsv stays 4 fields" \
    "$(printf '%s' "$l2" | awk -F '\t' -v id="$ido" 'NF==4 && $1==id && $4=="tsv-oneshot" {ok=1} END {exit ok?0:1}' && echo 0 || echo 1)" "$l2"
r=$(curl -s -m 3 "$XS_URL/timers?json=1")
check "/timers?json=1 carries repeat_s/until (2/0 recurring, 0/0 one-shot)" \
    "$(printf '%s' "$r" | python3 -c "
import json,sys
d=json.load(sys.stdin)
rec=[t for t in d if t['id']=='$idr']
one=[t for t in d if t['id']=='$ido']
sys.exit(0 if rec and one and rec[0]['repeat_s']==2 and rec[0]['until']==0 and one[0]['repeat_s']==0 and one[0]['until']==0 else 1)" && echo 0 || echo 1)" "$r"

now=$(date +%s)
u=$((now + 5))
r=$(curl -s -m 3 -X POST "$XS_URL/remind" -d "every 2s \"until-check\" until $u")
idu=$(echo "$r" | awk '{print $2}')
sleep 8
r=$(curl -s -m 3 "$XM_URL/notes?q=until-check")
nu=$(printf '%s' "$r" | grep -c "fired timer $idu")
check "every 2s until now+5 fires exactly twice, then stops" \
    "$([ "$nu" -eq 2 ] && echo 0 || echo 1)" "fires=$nu"
r=$(curl -s -m 3 "$XS_URL/timers")
check "until-reached timer leaves /timers" \
    "$(printf '%s' "$r" | grep -q "$idu" && echo 1 || echo 0)"
r=$(curl -s -m 3 "$XM_URL/list?prefix=exosched:timer:")
check "until-reached timer's key dropped" \
    "$(printf '%s' "$r" | grep -q "$idu" && echo 1 || echo 0)"

h1=$(curl -s -m 3 -o /dev/null -w '%{http_code}' -X POST "$XS_URL/remind" -d "every 2s \"x\" until $((now - 10))")
check "every ... until <past> -> 400" "$([ "$h1" = "400" ] && echo 0 || echo 1)" "h1=$h1"
h2=$(curl -s -m 3 -o /dev/null -w '%{http_code}' -X POST "$XS_URL/remind" -d "at $((now - 10)) \"x\"")
check "at <past> -> 400 (pinned)" "$([ "$h2" = "400" ] && echo 0 || echo 1)" "h2=$h2"

r=$(curl -s -m 3 -X POST "$XS_URL/remind" -d 'every 2s "cancel-recur"')
idcr=$(echo "$r" | awk '{print $2}')
sleep 2.5
d=$(curl -s -m 3 -X DELETE "$XS_URL/timer?id=$idcr")
check "DELETE /timer?id= cancels a recurring timer" \
    "$([ "$d" = "ok" ] && echo 0 || echo 1)" "$d"
sleep 3.5
r=$(curl -s -m 3 "$XM_URL/notes?q=cancel-recur")
ncr=$(printf '%s' "$r" | grep -c "fired timer $idcr")
check "cancelled recurring fires exactly once before cancel, never after" \
    "$([ "$ncr" -eq 1 ] && echo 0 || echo 1)" "fires=$ncr"
r=$(curl -s -m 3 "$XM_URL/list?prefix=exosched:timer:")
check "cancelled recurring timer's exomind key gone" \
    "$(printf '%s' "$r" | grep -q "$idcr" && echo 1 || echo 0)"

r=$(curl -s -m 3 -X POST "$XS_URL/remind" -d 'every 2s "recur-restart"')
idp2=$(echo "$r" | awk '{print $2}')
kill -9 "$XS_PID" 2>/dev/null
stop_port $XS_PORT
sleep 0.3
setsid nohup "$XS_BIN" --port $XS_PORT --exomind "$XM_URL" \
    >"$XS_LOG" 2>&1 < /dev/null &
XS_PID=$!
sleep 1
r=$(curl -s -m 3 "$XS_URL/timers")
check "recurring timer survives SIGKILL restart, repeat col intact" \
    "$(printf '%s' "$r" | awk -F '\t' -v id="$idp2" '$1==id && NF==6 && $5==2 {ok=1} END {exit ok?0:1}' && echo 0 || echo 1)" "$r"
python3 wsclient.py $XS_PORT recur-restart 8 >"$WS_LOG" 2>&1 &
WS_PID=$!
wait "$WS_PID" 2>/dev/null
check "restarted recurring timer still fires + ws push" \
    "$( [ $? -eq 0 ] && grep -q "timer $idp2 .* recur-restart" "$WS_LOG" && echo 0 || echo 1)" "$(cat "$WS_LOG")"

r=$(curl -s -m 3 -X POST "$XS_URL/remind" -d 'every 2s "catchup-check"')
idq=$(echo "$r" | awk '{print $2}')
kill -9 "$XS_PID" 2>/dev/null
stop_port $XS_PORT
sleep 5.5
setsid nohup "$XS_BIN" --port $XS_PORT --exomind "$XM_URL" \
    >"$XS_LOG" 2>&1 < /dev/null &
XS_PID=$!
sleep 1.2
check "overdue recurring timer logs catch-up on reload (not 'missed')" \
    "$(grep -q "caught up" "$XS_LOG" && echo 0 || echo 1)" "$(grep -E 'caught up|missed' "$XS_LOG" | tail -2)"
sleep 3
r=$(curl -s -m 3 "$XM_URL/notes?q=catchup-check")
check "overdue recurring catches up and fires after restart, no missed note" \
    "$(printf '%s' "$r" | grep -q "fired timer $idq" && printf '%s' "$r" | grep -q "missed timer $idq" && echo 1 || echo 0)" "$r"

kill -9 "$XS_PID" 2>/dev/null
stop_port $XS_PORT
now=$(date +%s)
curl -s -m 3 -X POST "$XM_URL/set" -H 'Content-Type: application/json' \
    -d "{\"key\":\"exosched:timer:legacy-wire\",\"value\":\"$((now + 6))\tlegacy-wire-msg\",\"ttl\":600}" \
    >/dev/null
setsid nohup "$XS_BIN" --port $XS_PORT --exomind "$XM_URL" \
    >"$XS_LOG" 2>&1 < /dev/null &
XS_PID=$!
sleep 1.2
r=$(curl -s -m 3 "$XS_URL/timers")
check "0.1.0 wire value (fire\\tmsg, no repeat cols) loads as one-shot" \
    "$(printf '%s' "$r" | awk -F '\t' -v id="legacy-wire" '$1==id && NF==4 && $4=="legacy-wire-msg" {ok=1} END {exit ok?0:1}' && echo 0 || echo 1)" "$r"
python3 wsclient.py $XS_PORT legacy-wire 10 >"$WS_LOG" 2>&1 &
WS_PID=$!
wait "$WS_PID" 2>/dev/null
check "legacy one-shot fires with ws push" \
    "$( [ $? -eq 0 ] && grep -q "timer legacy-wire" "$WS_LOG" && echo 0 || echo 1)" "$(cat "$WS_LOG")"

r=$(curl -s -m 3 -X POST "$XS_URL/remind" -d 'in 2s "one-shot-still-ok"')
id2s=$(echo "$r" | awk '{print $2}')
sleep 3
r=$(curl -s -m 3 "$XM_URL/notes?q=one-shot-still-ok")
check "in 2s one-shot still fires (no regression)" \
    "$(printf '%s' "$r" | grep -q "fired timer $id2s" && echo 0 || echo 1)" "$r"

# --- reload/cancel race: a stale reload snapshot must not resurrect a timer
#     cancelled while the reload was in flight (Agent E report) -------------
# Degraded startup (exomind down) puts reloads in a background thread that
# runs concurrently with the HTTP API; 6000 overdue keys stretch the reload's
# apply phase so the window is deterministic.
kill -9 "$XS_PID" 2>/dev/null
stop_port $XS_PORT
kill -9 "$XM_PID" 2>/dev/null
stop_port $XM_PORT
setsid nohup "$XM_BIN" --port $XM_PORT --data "$XM_DATA" \
    >"$XM_LOG" 2>&1 < /dev/null &
XM_PID=$!
sleep 0.6
XM_PORT=$XM_PORT python3 - <<'EOF'
import json, os, urllib.request
port = os.environ["XM_PORT"]
ops = [["set", "exosched:timer:0000-%04d" % i, "1700000000\tstale-%04d" % i, "3600"] for i in range(6000)]
req = urllib.request.Request("http://127.0.0.1:%s/batch" % port,
                             data=json.dumps(ops).encode(),
                             headers={"Content-Type": "application/json"})
urllib.request.urlopen(req, timeout=30).read()
EOF
kill -9 "$XM_PID" 2>/dev/null
stop_port $XM_PORT
setsid nohup "$XS_BIN" --port $XS_PORT --exomind "$XM_URL" \
    >"$XS_LOG" 2>&1 < /dev/null &
XS_PID=$!
fails=0
for _ in $(seq 1 80); do
    fails=$(grep -c "reload failed" "$XS_LOG" 2>/dev/null || true)
    [ "${fails:-0}" -ge 11 ] && break
    sleep 0.25
done
check "degraded startup: reload thread running (>=11 failed attempts)" \
    "$([ "${fails:-0}" -ge 11 ] && echo 0 || echo 1)" "fails=${fails:-0}"
target=$((fails + 1))
for _ in $(seq 1 30); do
    fails=$(grep -c "reload failed" "$XS_LOG" 2>/dev/null || true)
    [ "${fails:-0}" -ge "$target" ] && break
    sleep 0.2
done
setsid nohup "$XM_BIN" --port $XM_PORT --data "$XM_DATA" \
    >"$XM_LOG" 2>&1 < /dev/null &
XM_PID=$!
sleep 0.3
r=$(curl -s -m 3 -X POST "$XS_URL/remind" -d 'every 2s "race-victim"')
idrv=$(echo "$r" | awk '{print $2}')
sleep 0.8
dok=0
t0=$(date +%s)
while [ $(( $(date +%s) - t0 )) -lt 20 ]; do
    grep -qE "reload complete|no timers to reload" "$XS_LOG" 2>/dev/null && break
    d=$(curl -s -m 3 -X DELETE "$XS_URL/timer?id=$idrv")
    [ "$d" = "ok" ] && dok=$((dok + 1))
    sleep 0.05
done
check "cancelled mid-reload (DELETE answered ok at least once)" \
    "$([ "$dok" -ge 1 ] && echo 0 || echo 1)" "ok-deletes=$dok"
sleep 5
r=$(curl -s -m 3 "$XM_URL/notes?q=race-victim")
check "cancelled mid-reload timer does NOT fire again (no resurrection)" \
    "$(printf '%s' "$r" | grep -q "fired timer $idrv" && echo 1 || echo 0)" "$r"
r=$(curl -s -m 3 "$XS_URL/timers")
check "cancelled mid-reload timer not re-added to /timers" \
    "$(printf '%s' "$r" | grep -q "$idrv" && echo 1 || echo 0)"
r=$(curl -s -m 3 "$XM_URL/list?prefix=exosched:timer:")
check "cancelled mid-reload timer's exomind key not re-created" \
    "$(printf '%s' "$r" | grep -q "$idrv" && echo 1 || echo 0)"
r=$(curl -s -m 3 "$XS_URL/ping")
check "daemon alive after reload race" "$([ "$r" = "pong" ] && echo 0 || echo 1)"

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

# --- delivery receipts ----------------------------------------------------------
rid=$(curl -s -m 3 -X POST "$XS_URL/remind" -d 'in 1s "rcpt-test" receipt=1')
rid=$(printf '%s' "$rid" | grep -oP '^ok \K\S+')
sleep 2.5
r=$(curl -s -m 3 "$XM_URL/get?key=receipt:$rid")
check "delivery receipt key exists after fire" \
    "$(printf '%s' "$r" | grep -q '^fired:' && echo 0 || echo 1)" "key=receipt:$rid val=$r"
r=$(curl -s -m 3 "$XM_URL/get?key=receipt:no-such-timer")
check "no receipt for never-created timer" "$([ "$r" = "missing" ] && echo 0 || echo 1)" "$r"

# --- delivery receipts: ack tracking + /delivery stats ------------------------
# quiet the 3 recurring every-2s timers still running from earlier sections
# (cadence-check, recur-restart, catchup-check) so the cumulative counters
# are stable while we measure deltas. A cancel can miss a timer that is
# mid-fire (popped from the registry, reschedules after bookkeeping), so
# retry until /timers no longer lists it, then drop any durable key that
# was left behind so a later daemon restart cannot resurrect it.
for tq in "$idr" "$idp2" "$idq"; do
    for _ in $(seq 1 30); do
        curl -s -m 3 -X DELETE "$XS_URL/timer?id=$tq" >/dev/null
        sleep 0.15
        n=$(curl -s -m 3 "$XS_URL/timers" | grep -c "^$tq[[:space:]]")
        [ "$n" = 0 ] && break
    done
    curl -s -m 3 -X DELETE "$XM_URL/del?key=exosched:timer:$tq" >/dev/null
done
# wait for a fully quiescent counter baseline (no fire in flight)
for _ in $(seq 1 10); do
    b1=$(curl -s -m 3 "$XS_URL/delivery")
    sleep 2
    b2=$(curl -s -m 3 "$XS_URL/delivery")
    [ "$b1" = "$b2" ] && break
    sleep 1
done
statv() { curl -s -m 3 "$XS_URL/delivery" | awk -v k="$1" '$0 ~ "^"k": " {print $2}'; }
df0=$(statv fired); da0=$(statv acked); du0=$(statv unacked)
check "/delivery prints key: value stats" \
    "$([ -n "$df0" ] && [ -n "$da0" ] && [ -n "$du0" ] && echo 0 || echo 1)" \
    "fired='$df0' acked='$da0' unacked='$du0'"

# (a) fired timer with an ACKed client -> fired/acked move, note carries receipt
python3 wsclient.py $XS_PORT delivery-acked 8 ack >"$WS_LOG" 2>&1 &
WS_PID=$!
sleep 0.4
r=$(curl -s -m 3 -X POST "$XS_URL/remind" -d 'in 1s "delivery-acked" receipt=1')
ida=$(echo "$r" | awk '{print $2}')
epda=$(echo "$r" | awk '{print $3}')
wait "$WS_PID" 2>/dev/null
sleep 0.6
df1=$(statv fired); da1=$(statv acked); du1=$(statv unacked)
check "ack'd fire: fired+1, acked+1, unacked unchanged" \
    "$( [ $((df1 - df0)) -eq 1 ] && [ $((da1 - da0)) -eq 1 ] && [ $((du1 - du0)) -eq 0 ] && echo 0 || echo 1)" \
    "fired $df0->$df1 acked $da0->$da1 unacked $du0->$du1"
r=$(curl -s -m 3 "$XM_URL/notes?q=delivery-acked")
check "ack'd fire: note carries 'delivery: acked 1/1'" \
    "$(printf '%s' "$r" | grep -q "fired timer $ida" && printf '%s' "$r" | grep -q 'delivery: acked 1/1 at ' && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XM_URL/get?key=delivery:$ida:$epda")
check "per-fire delivery key written (acked 1/1)" \
    "$(printf '%s' "$r" | grep -qE '^acked 1/1 at [0-9]+$' && echo 0 || echo 1)" \
    "key=delivery:$ida:$epda val=$r"
r=$(curl -s -m 3 "$XS_URL/delivery?detail=1")
check "/delivery?detail=1 lists the per-timer receipt" \
    "$(printf '%s' "$r" | grep -qE "timer $ida $epda acked 1/1 at [0-9]+$" && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XM_URL/get?key=receipt:$ida")
check "receipt=1 key carries the ack count" \
    "$(printf '%s' "$r" | grep -qE '^fired:.*;acked:1:1@' && echo 0 || echo 1)" "$r"

# (b) fired timer with NO client connected -> unacked counters move
r=$(curl -s -m 3 -X POST "$XS_URL/remind" -d 'in 1s "delivery-unacked"')
idu=$(echo "$r" | awk '{print $2}')
epdu=$(echo "$r" | awk '{print $3}')
sleep 2.5
df2=$(statv fired); da2=$(statv acked); du2=$(statv unacked)
check "unacked fire: fired+1, unacked+1, acked unchanged" \
    "$( [ $((df2 - df1)) -eq 1 ] && [ $((du2 - du1)) -eq 1 ] && [ $((da2 - da1)) -eq 0 ] && echo 0 || echo 1)" \
    "fired $df1->$df2 acked $da1->$da2 unacked $du1->$du2"
r=$(curl -s -m 3 "$XM_URL/notes?q=delivery-unacked")
check "unacked fire: note carries 'delivery: acked 0/0'" \
    "$(printf '%s' "$r" | grep -q "fired timer $idu" && printf '%s' "$r" | grep -q 'delivery: acked 0/0 at ' && echo 0 || echo 1)" "$r"
r=$(curl -s -m 3 "$XM_URL/get?key=delivery:$idu:$epdu")
check "per-fire delivery key written (acked 0/0)" \
    "$(printf '%s' "$r" | grep -qE '^acked 0/0 at [0-9]+$' && echo 0 || echo 1)" \
    "key=delivery:$idu:$epdu val=$r"
r=$(curl -s -m 3 "$XS_URL/delivery")
check "/delivery shows last_acked_ts and last_unacked_ts" \
    "$(printf '%s' "$r" | grep -qE '^last_acked_ts: [0-9]+$' && printf '%s' "$r" | grep -qE '^last_unacked_ts: [0-9]+$' && echo 0 || echo 1)" "$r"

# (c) restart -> stats survive
dl1=$(curl -s -m 3 "$XS_URL/delivery")
kill "$XS_PID" 2>/dev/null
stop_port $XS_PORT
sleep 0.3
setsid nohup "$XS_BIN" --port $XS_PORT --exomind "$XM_URL" \
    >"$XS_LOG" 2>&1 < /dev/null &
XS_PID=$!
sleep 1
dl2=$(curl -s -m 3 "$XS_URL/delivery")
check "delivery stats survive daemon restart" \
    "$( [ "$(printf '%s\n' "$dl1" | awk '/^fired:/{print $2}')" = "$(printf '%s\n' "$dl2" | awk '/^fired:/{print $2}')" ] && [ "$(printf '%s\n' "$dl1" | awk '/^acked:/{print $2}')" = "$(printf '%s\n' "$dl2" | awk '/^acked:/{print $2}')" ] && [ "$(printf '%s\n' "$dl1" | awk '/^unacked:/{print $2}')" = "$(printf '%s\n' "$dl2" | awk '/^unacked:/{print $2}')" ] && echo 0 || echo 1)" \
    "$(printf 'before:\n%s\nafter:\n%s' "$dl1" "$dl2")"
r=$(curl -s -m 3 "$XS_URL/delivery?detail=1")
check "per-timer receipts survive daemon restart (still listed)" \
    "$(printf '%s' "$r" | grep -qE "timer $ida $epda acked 1/1" && echo 0 || echo 1)" "$r"

# bad params
"$XS_BIN" /delivery?foo=1 --exomind "$XM_URL" >/dev/null 2>&1
rc=$?
check "console /delivery?foo=1 exits 2 (bad params)" "$([ "$rc" = 2 ] && echo 0 || echo 1)" "rc=$rc"
"$XS_BIN" /delivery?detail=2 --exomind "$XM_URL" >/dev/null 2>&1
rc=$?
check "console /delivery?detail=2 exits 2" "$([ "$rc" = 2 ] && echo 0 || echo 1)" "rc=$rc"
r=$("$XS_BIN" /delivery --exomind "$XM_URL" 2>/dev/null)
check "console /delivery prints stats and exits 0" \
    "$([ "$?" = 0 ] && printf '%s' "$r" | grep -qE '^fired: [0-9]+$' && echo 0 || echo 1)" "$r"
h=$(curl -s -m 3 -o /dev/null -w '%{http_code}' "$XS_URL/delivery?foo=1")
check "daemon /delivery?foo=1 -> 400" "$([ "$h" = "400" ] && echo 0 || echo 1)" "h=$h"

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
