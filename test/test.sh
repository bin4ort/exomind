#!/bin/bash
# exomind functional test suite.
# Exercises every endpoint, persistence across restarts, TTL expiry, auth.

set -u
BIN="$(cd "$(dirname "$0")/.." && pwd)/build/exomind"
PORT=$((20000 + $$ % 20000))
BASE="http://127.0.0.1:$PORT"
DATA=$(mktemp -d)
FAILS=0

assert_eq() { # desc expected actual
    if [ "$2" = "$3" ]; then
        printf 'PASS %s\n' "$1"
    else
        printf 'FAIL %s\n  expected: %s\n  actual:   %s\n' "$1" "$2" "$3"
        FAILS=$((FAILS + 1))
    fi
}

assert_contains() { # desc needle haystack
    case "$3" in
        *"$2"*) printf 'PASS %s\n' "$1" ;;
        *) printf 'FAIL %s\n  expected to contain: %s\n  actual: %s\n' "$1" "$2" "$3"
           FAILS=$((FAILS + 1)) ;;
    esac
}

assert_not_contains() { # desc needle haystack
    case "$3" in
        *"$2"*) printf 'FAIL %s\n  expected not to contain: %s\n  actual: %s\n' "$1" "$2" "$3"
           FAILS=$((FAILS + 1)) ;;
        *) printf 'PASS %s\n' "$1" ;;
    esac
}

start_server() {
    {
        echo "===== SERVER START: $*"
    } >> "$DATA/server.log"
    "$BIN" --host 127.0.0.1 --port "$PORT" --data "$DATA/exomind.dat" \
        --project-root "$DATA" \
        "$@" 2>>"$DATA/server.log" &
    SRV=$!
    for _ in $(seq 1 100); do
        if curl -s -o /dev/null "$BASE/ping" 2>/dev/null; then
            return 0
        fi
        sleep 0.05
    done
    echo "server failed to start"
    cat "$DATA/server.log"
    exit 1
}

stop_server() {
    kill "$SRV" 2>/dev/null
    wait "$SRV" 2>/dev/null
}

echo "=== session 1: core endpoints ==="
start_server

assert_eq "ping" "pong" "$(curl -s "$BASE/ping")"

assert_eq "set raw body" "ok" "$(curl -s -X POST "$BASE/set?key=hello" -d 'world')"
assert_eq "get" "world" "$(curl -s "$BASE/get?key=hello")"
assert_eq "get missing" "missing" "$(curl -s "$BASE/get?key=nope")"
assert_eq "get missing status" "404" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/get?key=nope")"

assert_eq "append" "ok" "$(curl -s -X POST "$BASE/append?key=hello" -d 'again')"
assert_eq "get appended" "world
again" "$(curl -s "$BASE/get?key=hello")"

assert_eq "set json body" "ok" "$(curl -s -X POST "$BASE/set" -H 'Content-Type: application/json' -d '{"key":"jj","value":"{\"a\":1}"}')"
assert_eq "get json value" '{"a":1}' "$(curl -s "$BASE/get?key=jj")"

assert_eq "set form body" "ok" "$(curl -s -X POST "$BASE/set" -d 'key=ff&value=hello%20world')"
assert_eq "get form value" "hello world" "$(curl -s "$BASE/get?key=ff")"

assert_eq "search exact" "ff	hello world" "$(curl -s "$BASE/search?q=hello+world")"
assert_contains "search json" '"score"' "$(curl -s "$BASE/search?q=hello&json=1")"
assert_contains "list" "jj" "$(curl -s "$BASE/list")"
assert_contains "list json" '"keys"' "$(curl -s "$BASE/list?json=1")"
assert_contains "list prefix" "ff" "$(curl -s "$BASE/list?prefix=f")"
assert_eq "list prefix excludes" "" "$(curl -s "$BASE/list?prefix=zz")"

assert_contains "note" "ok note:" "$(curl -s -X POST "$BASE/note" -d 'first note')"
sleep 0.05
assert_contains "note 2" "ok note:" "$(curl -s -X POST "$BASE/note" -d 'second note with hello')"
assert_contains "notes newest first" "second note with hello" "$(curl -s "$BASE/notes?limit=1")"
assert_contains "notes all" "first note" "$(curl -s "$BASE/notes")"
assert_contains "notes filter" "first note" "$(curl -s "$BASE/notes?q=first")"
assert_contains "notes json" '"notes"' "$(curl -s "$BASE/notes?json=1")"

assert_eq "ttl set" "ok" "$(curl -s -X POST "$BASE/set?key=temp&ttl=1" -d 'gone')"
assert_eq "ttl get before expiry" "gone" "$(curl -s "$BASE/get?key=temp")"
sleep 2
assert_eq "ttl expired" "missing" "$(curl -s "$BASE/get?key=temp")"

assert_eq "batch arrays" 'set k1 ok
get k1 alpha
del k1 ok' "$(curl -s -X POST "$BASE/batch" -d '[["set","k1","alpha"],["get","k1"],["del","k1"]]')"

assert_eq "batch objects" 'set k2 ok
get k2 beta
del k2 ok' "$(curl -s -X POST "$BASE/batch" -d '[{"set":"k2","value":"beta"},{"get":"k2"},{"del":"k2"}]')"

assert_eq "del" "ok" "$(curl -s -X DELETE "$BASE/del?key=hello")"
assert_eq "get after del" "missing" "$(curl -s "$BASE/get?key=hello")"
assert_eq "del missing" "missing" "$(curl -s -X DELETE "$BASE/del?key=hello")"

assert_contains "stats" "entries:" "$(curl -s "$BASE/stats")"
assert_contains "stats json" '"uptime_s"' "$(curl -s "$BASE/stats?json=1")"
assert_contains "help" "# exomind" "$(curl -s "$BASE/")"
assert_eq "unknown path" "error: unknown path" "$(curl -s "$BASE/nope")"
assert_eq "bad method" "error: method not allowed" "$(curl -s -X PUT "$BASE/ping")"

stop_server

echo "=== session 2: persistence across restart ==="
start_server
assert_eq "persist get" '{"a":1}' "$(curl -s "$BASE/get?key=jj")"
assert_eq "persist form" "hello world" "$(curl -s "$BASE/get?key=ff")"
assert_contains "persist notes" "first note" "$(curl -s "$BASE/notes")"
assert_eq "persist ttl gone" "missing" "$(curl -s "$BASE/get?key=temp")"
assert_eq "persist del" "ok" "$(curl -s -X DELETE "$BASE/del?key=ff")"
stop_server

echo "=== session 3: tombstone persists ==="
start_server
assert_eq "tombstone persists" "missing" "$(curl -s "$BASE/get?key=ff")"
assert_contains "tombstone not in list" "jj" "$(curl -s "$BASE/list")"
stop_server

echo "=== session 4: token auth ==="
start_server --token sekret
assert_eq "auth denied" "error: unauthorized" "$(curl -s "$BASE/get?key=jj")"
assert_eq "auth ok" '{"a":1}' "$(curl -s -H 'Authorization: Bearer sekret' "$BASE/get?key=jj")"
assert_eq "auth wrong token" "error: unauthorized" "$(curl -s -H 'Authorization: Bearer wrong' "$BASE/get?key=jj")"
stop_server

echo "=== session 5: crash recovery (SIGKILL, no clean shutdown) ==="
start_server
for i in $(seq 1 200); do
    curl -s -X POST "$BASE/set?key=crash$i" -d "value$i" > /dev/null
done
kill -9 "$SRV" 2>/dev/null
wait "$SRV" 2>/dev/null
start_server
assert_eq "crash recovered" "value42" "$(curl -s "$BASE/get?key=crash42")"
assert_eq "crash recovered last" "value200" "$(curl -s "$BASE/get?key=crash200")"
stop_server

echo "=== session 6: concurrency smoke test ==="
start_server
CPIDS=""
for i in $(seq 1 50); do
    curl -s -X POST "$BASE/set?key=c$i" -d "v$i" > /dev/null &
    CPIDS="$CPIDS $!"
done
for p in $CPIDS; do wait "$p"; done
for i in $(seq 1 50); do
    assert_eq "concurrent key c$i" "v$i" "$(curl -s "$BASE/get?key=c$i")"
done
stop_server


echo "=== session 7: snapshot/restore ==="
start_server

printf 'tab\there\nline2\n\x01\x02\xff' > "$DATA/weird"
assert_eq "set binary value" "ok" "$(curl -s -X POST "$BASE/set?key=w:bin" --data-binary @"$DATA/weird")"
curl -s "$BASE/get?key=w:bin" > "$DATA/back"
cmp -s "$DATA/weird" "$DATA/back"
assert_eq "binary value round trip" "0" "$?"
assert_eq "set s:a" "ok" "$(curl -s -X POST "$BASE/set?key=s:a" -d 'alpha')"
assert_eq "set s:b" "ok" "$(curl -s -X POST "$BASE/set?key=s:b" -d 'beta')"
assert_eq "set s:gone" "ok" "$(curl -s -X POST "$BASE/set?key=s:gone" -d 'x')"
assert_eq "del s:gone" "ok" "$(curl -s -X DELETE "$BASE/del?key=s:gone")"

curl -s "$BASE/snapshot" > "$DATA/snap1"
N=$(curl -s "$BASE/list?limit=10000" | wc -l)
assert_contains "snapshot header" "exomind-snapshot-v1" "$(curl -s "$BASE/snapshot")"
assert_contains "snapshot has key" "w:bin" "$(cat "$DATA/snap1")"
assert_not_contains "snapshot no tombstones" "s:gone" "$(cat "$DATA/snap1")"

assert_eq "restore count" "ok $N" "$(curl -s -X POST "$BASE/restore" --data-binary @"$DATA/snap1")"
curl -s "$BASE/get?key=w:bin" > "$DATA/back2"
cmp -s "$DATA/weird" "$DATA/back2"
assert_eq "binary value after restore" "0" "$?"
assert_eq "restored value" "beta" "$(curl -s "$BASE/get?key=s:b")"

assert_eq "restore bad format" "error: bad snapshot" "$(curl -s -X POST "$BASE/restore" -d 'garbage')"
assert_eq "store intact after bad restore" "alpha" "$(curl -s "$BASE/get?key=s:a")"

printf 'exomind-snapshot-v1\n' > "$DATA/snapempty"
assert_eq "restore to empty" "ok 0" "$(curl -s -X POST "$BASE/restore" --data-binary @"$DATA/snapempty")"
assert_eq "store empty after restore" "missing" "$(curl -s "$BASE/get?key=s:a")"
assert_eq "list empty after restore" "" "$(curl -s "$BASE/list")"

assert_eq "restore again" "ok $N" "$(curl -s -X POST "$BASE/restore" --data-binary @"$DATA/snap1")"
stop_server

echo "=== session 8: snapshot persists, restore respects auth ==="
start_server
curl -s "$BASE/get?key=w:bin" > "$DATA/back3"
cmp -s "$DATA/weird" "$DATA/back3"
assert_eq "binary value persisted" "0" "$?"
assert_eq "restore persisted" "alpha" "$(curl -s "$BASE/get?key=s:a")"
stop_server

start_server --token sekret
assert_eq "snapshot denied without auth" "error: unauthorized" "$(curl -s "$BASE/snapshot")"
curl -s -H 'Authorization: Bearer sekret' "$BASE/snapshot" > "$DATA/snap2"
assert_contains "snapshot ok with auth" "exomind-snapshot-v1" "$(cat "$DATA/snap2")"
assert_eq "restore denied without auth" "error: unauthorized" "$(curl -s -X POST "$BASE/restore" --data-binary @"$DATA/snap2")"
assert_eq "restore ok with auth" "ok $N" "$(curl -s -H 'Authorization: Bearer sekret' -X POST "$BASE/restore" --data-binary @"$DATA/snap2")"
stop_server

echo "=== session 9: scoped tokens ==="
cat > "$DATA/tokens" <<'EOF'
# scoped token test file
reader:ro
logs:scope=logs/*
logro:ro:scope=logs/*
EOF
start_server --token master --tokens "$DATA/tokens"
MASTER="Authorization: Bearer master"
LOGS="Authorization: Bearer logs"
READER="Authorization: Bearer reader"
LOGRO="Authorization: Bearer logro"

assert_eq "master set logs/a" "ok" "$(curl -s -H "$MASTER" -X POST "$BASE/set?key=logs/a" -d 'la')"
assert_eq "master set logs/b" "ok" "$(curl -s -H "$MASTER" -X POST "$BASE/set?key=logs/b" -d 'lb')"
assert_eq "master set other/c" "ok" "$(curl -s -H "$MASTER" -X POST "$BASE/set?key=other/c" -d 'oc')"

assert_eq "scoped get allowed" "la" "$(curl -s -H "$LOGS" "$BASE/get?key=logs/a")"
assert_eq "scoped get denied" "error: denied" "$(curl -s -H "$LOGS" "$BASE/get?key=other/c")"
assert_eq "scoped get denied status" "403" "$(curl -s -o /dev/null -w '%{http_code}' -H "$LOGS" "$BASE/get?key=other/c")"
assert_eq "scoped set allowed" "ok" "$(curl -s -H "$LOGS" -X POST "$BASE/set?key=logs/c" -d 'lc')"
assert_eq "scoped set denied" "error: denied" "$(curl -s -H "$LOGS" -X POST "$BASE/set?key=other/d" -d 'x')"
assert_eq "scoped append denied" "error: denied" "$(curl -s -H "$LOGS" -X POST "$BASE/append?key=other/e" -d 'x')"
assert_eq "scoped del denied" "error: denied" "$(curl -s -H "$LOGS" -X DELETE "$BASE/del?key=other/f")"
assert_eq "scoped note denied" "error: denied" "$(curl -s -H "$LOGS" -X POST "$BASE/note" -d 'nope')"
assert_contains "scoped list shows own" "logs/a" "$(curl -s -H "$LOGS" "$BASE/list")"
assert_not_contains "scoped list hides outside" "other/c" "$(curl -s -H "$LOGS" "$BASE/list")"
assert_contains "scoped list prefix" "logs/b" "$(curl -s -H "$LOGS" "$BASE/list?prefix=logs")"
assert_eq "scoped list prefix outside" "" "$(curl -s -H "$LOGS" "$BASE/list?prefix=other")"
assert_eq "scoped search denied key" "" "$(curl -s -H "$LOGS" "$BASE/search?q=oc")"
assert_eq "scoped search own key" "logs/c	lc" "$(curl -s -H "$LOGS" "$BASE/search?q=lc")"

assert_eq "ro get allowed" "alpha" "$(curl -s -H "$READER" "$BASE/get?key=s:a")"
assert_eq "ro set denied" "error: denied" "$(curl -s -H "$READER" -X POST "$BASE/set?key=nk" -d 'x')"
assert_eq "ro append denied" "error: denied" "$(curl -s -H "$READER" -X POST "$BASE/append?key=nk" -d 'x')"
assert_eq "ro del denied" "error: denied" "$(curl -s -H "$READER" -X DELETE "$BASE/del?key=s:a")"
assert_eq "ro note denied" "error: denied" "$(curl -s -H "$READER" -X POST "$BASE/note" -d 'nope')"
assert_eq "ro restore denied" "error: denied" "$(curl -s -H "$READER" -X POST "$BASE/restore" --data-binary @"$DATA/snap2")"
assert_eq "ro snapshot allowed" "exomind-snapshot-v1" "$(curl -s -H "$READER" "$BASE/snapshot" | head -1)"

assert_eq "logro write denied" "error: denied" "$(curl -s -H "$LOGRO" -X POST "$BASE/set?key=logs/z" -d 'x')"
assert_eq "logro read allowed" "la" "$(curl -s -H "$LOGRO" "$BASE/get?key=logs/a")"
assert_eq "logro restore denied" "error: denied" "$(curl -s -H "$LOGRO" -X POST "$BASE/restore" --data-binary @"$DATA/snap2")"

assert_eq "batch scoped" 'set logs/x ok
set other/y denied
get logs/x v
get other/z denied' "$(curl -s -H "$LOGS" -X POST "$BASE/batch" -d '[["set","logs/x","v"],["set","other/y","v"],["get","logs/x"],["get","other/z"]]')"
assert_eq "batch ro" 'set nk denied
del s:a denied
get s:a alpha' "$(curl -s -H "$READER" -X POST "$BASE/batch" -d '[["set","nk","v"],["del","s:a"],["get","s:a"]]')"
assert_eq "batch objects scoped" 'set logs/o ok
get other/q denied' "$(curl -s -H "$LOGS" -X POST "$BASE/batch" -d '[{"set":"logs/o","value":"z"},{"get":"other/q"}]')"

assert_contains "scoped snapshot shows own" "logs/a" "$(curl -s -H "$LOGS" "$BASE/snapshot")"
assert_not_contains "scoped snapshot hides outside" "other/c" "$(curl -s -H "$LOGS" "$BASE/snapshot")"
assert_eq "scoped restore denied" "error: denied" "$(curl -s -H "$LOGS" -X POST "$BASE/restore" --data-binary @"$DATA/snap2")"

assert_eq "master still full access" "oc" "$(curl -s -H "$MASTER" "$BASE/get?key=other/c")"
assert_eq "wrong token" "error: unauthorized" "$(curl -s -H 'Authorization: Bearer nope' "$BASE/get?key=logs/a")"
stop_server

echo "=== session 10: malformed HTTP hardening ==="
start_server

# LF-only line endings (no CR bytes) must be accepted
exec 9<>/dev/tcp/127.0.0.1/$PORT
printf 'GET /ping HTTP/1.1\nHost: x\n\n' >&9
resp=$(cat <&9)
exec 9<&- 9>&-
assert_contains "lf-only ping" "pong" "$resp"

# LF-only headers with a JSON body; the Content-Type header must not be lost
body='{"key":"lfj","value":"jj"}'
cl=$(printf '%s' "$body" | wc -c)
exec 8<>/dev/tcp/127.0.0.1/$PORT
printf 'POST /set HTTP/1.1\nContent-Type: application/json\nContent-Length: %s\n\n%s' "$cl" "$body" >&8
head -c 128 <&8 > /dev/null
exec 8<&- 8>&-
assert_eq "lf-only json stored" "jj" "$(curl -s "$BASE/get?key=lfj")"

# LF-only request with a body on /append
exec 7<>/dev/tcp/127.0.0.1/$PORT
printf 'POST /append?key=lfa HTTP/1.1\nContent-Length: 3\n\nxyz' >&7
cat <&7 > /dev/null
exec 7<&- 7>&-
assert_eq "lf-only append stored" "xyz" "$(curl -s "$BASE/get?key=lfa")"

# garbage request line must be answered with 400, not a crash
exec 6<>/dev/tcp/127.0.0.1/$PORT
printf 'GARBAGE\x00\xff\r\n\r\n' >&6
resp=$(head -1 <&6)
exec 6<&- 6>&-
assert_contains "garbage request 400" "400" "$resp"

stop_server

echo "=== session 11: body shape heuristics ==="
start_server

assert_eq "set raw with =" "ok" "$(curl -s -X POST "$BASE/set?key=rq" -d 'a=b')"
assert_eq "get raw with =" "a=b" "$(curl -s "$BASE/get?key=rq")"
assert_eq "set raw with &" "ok" "$(curl -s -X POST "$BASE/set?key=ra" -d 'a&b=c')"
assert_eq "get raw with &" "a&b=c" "$(curl -s "$BASE/get?key=ra")"
assert_eq "set raw with {" "ok" "$(curl -s -X POST "$BASE/set?key=rb" -d '{"a":1}')"
assert_eq "get raw with {" '{"a":1}' "$(curl -s "$BASE/get?key=rb")"
assert_eq "set raw plus" "ok" "$(curl -s -X POST "$BASE/set?key=rp" -d 'a+b')"
assert_eq "get raw plus" "a+b" "$(curl -s "$BASE/get?key=rp")"
assert_eq "json ct needs key field" "error: missing key" "$(curl -s -X POST "$BASE/set?key=rb" -H 'Content-Type: application/json' -d '{"a":1}')"

# percent-encoded NUL byte in a form value must survive
curl -s -X POST "$BASE/set" -d 'key=rn&value=ab%00cd' > /dev/null
curl -s "$BASE/get?key=rn" > "$DATA/nulv"
printf 'ab\x00cd' > "$DATA/nulv_exp"
cmp -s "$DATA/nulv" "$DATA/nulv_exp"
assert_eq "form nul byte preserved" "0" "$?"

stop_server

echo "=== session 12: empty keys, oversize keys, ttl edges ==="
start_server

assert_eq "set empty key" "error: empty key" "$(curl -s -X POST "$BASE/set?key=" -d 'x')"
assert_eq "set empty key status" "400" "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/set?key=" -d 'x')"
assert_eq "append empty key" "error: empty key" "$(curl -s -X POST "$BASE/append?key=" -d 'x')"
assert_eq "del empty key" "error: empty key" "$(curl -s -X DELETE "$BASE/del?key=")"
assert_eq "get empty key" "missing" "$(curl -s "$BASE/get?key=")"

LONGK=$(printf 'k%.0s' $(seq 1 5000))
assert_eq "set 5k key rejected" "error: key too long" "$(curl -s -X POST "$BASE/set?key=$LONGK" -d 'x')"
assert_eq "append 5k key rejected" "error: key too long" "$(curl -s -X POST "$BASE/append?key=$LONGK" -d 'x')"
assert_eq "del 5k key rejected" "error: key too long" "$(curl -s -X DELETE "$BASE/del?key=$LONGK")"
assert_eq "get 5k key rejected" "error: key too long" "$(curl -s "$BASE/get?key=$LONGK")"
assert_eq "form 5k key rejected" "error: key too long" "$(curl -s -X POST "$BASE/set" -d "key=$LONGK&value=x")"
assert_eq "json 5k key rejected" "error: key too long" "$(curl -s -X POST "$BASE/set" -H 'Content-Type: application/json' -d "{\"key\":\"$LONGK\",\"value\":\"x\"}")"

assert_eq "ttl huge set" "ok" "$(curl -s -X POST "$BASE/set?key=tth&ttl=9223372036854775" -d 'big')"
assert_eq "ttl huge readable" "big" "$(curl -s "$BASE/get?key=tth")"
curl -s -X POST "$BASE/set?key=ttn&ttl=-7" -d 'neg' > /dev/null
assert_eq "ttl negative forever" "neg" "$(curl -s "$BASE/get?key=ttn")"
curl -s -X POST "$BASE/set?key=ttz&ttl=0" -d 'z' > /dev/null
assert_eq "ttl 0 forever" "z" "$(curl -s "$BASE/get?key=ttz")"

stop_server

echo "=== session 13: auth hardening ==="
start_server --token sekret
assert_eq "auth trailing ws" "pong" "$(curl -s -H 'Authorization: Bearer sekret ' "$BASE/ping")"
assert_eq "auth leading ws" "pong" "$(curl -s -H 'Authorization:    Bearer sekret' "$BASE/ping")"
exec 5<>/dev/tcp/127.0.0.1/$PORT
printf 'GET /ping HTTP/1.1\nAuthorization: Bearer sekret\n\n' >&5
resp=$(cat <&5)
exec 5<&- 5>&-
assert_eq "auth lf-only" "pong" "$(printf '%s' "$resp" | tr -d '\r' | tail -1)"
stop_server
LONGT=$(printf 't%.0s' $(seq 1 200))
start_server --token "$LONGT"
assert_eq "long token ok" "pong" "$(curl -s -H "Authorization: Bearer $LONGT" "$BASE/ping")"
assert_eq "long token wrong" "error: unauthorized" "$(curl -s -H "Authorization: Bearer ${LONGT}x" "$BASE/ping")"
stop_server

echo "=== session 14: batch note limitation ==="
start_server
assert_eq "batch note array rejected" "error: bad batch op" "$(curl -s -X POST "$BASE/batch" -d '[["note","hi"]]')"
assert_eq "batch note object rejected" "error: bad batch element" "$(curl -s -X POST "$BASE/batch" -d '[{"note":"hi"}]')"
assert_contains "spec documents note limit" "not a batch op" "$(curl -s "$BASE/")"

stop_server

echo "=== session 15: vectors (exovec) core ==="
start_server

assert_eq "sim with no vectors" "" "$(curl -s -X POST "$BASE/sim" -d 'nothing here yet')"

assert_eq "embed post" "ok doc:alpha 256" "$(curl -s -X POST "$BASE/embed?key=doc:alpha" -d 'the quick brown fox jumps over the lazy dog')"
assert_contains "embed get dim" "dim 256" "$(curl -s "$BASE/embed?key=doc:alpha")"
assert_contains "embed get pairs" ":" "$(curl -s "$BASE/embed?key=doc:alpha")"
assert_eq "embed get missing" "missing" "$(curl -s "$BASE/embed?key=doc:nope")"
assert_eq "embed get missing status" "404" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/embed?key=doc:nope")"
assert_eq "embed missing key" "error: missing key" "$(curl -s -X POST "$BASE/embed" -d 'hi')"
assert_eq "embed empty key" "error: empty key" "$(curl -s -X POST "$BASE/embed?key=" -d 'hi')"
assert_eq "embed get status" "200" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/embed?key=doc:alpha")"

v1=$(curl -s "$BASE/embed?key=doc:alpha")
curl -s -X POST "$BASE/embed?key=doc:alpha" -d 'the quick brown fox jumps over the lazy dog' > /dev/null
v2=$(curl -s "$BASE/embed?key=doc:alpha")
assert_eq "embed deterministic" "$v1" "$v2"

curl -s -X POST "$BASE/embed?key=doc:cat1" -d 'the cat sat on the mat' > /dev/null
curl -s -X POST "$BASE/embed?key=doc:cat2" -d 'a cat was sitting on the mat' > /dev/null
curl -s -X POST "$BASE/embed?key=doc:dog" -d 'the dog barked at the mailman' > /dev/null
sim=$(curl -s -X POST "$BASE/sim" -d 'cats sit on mats')
assert_contains "sim has cat2" "doc:cat2" "$sim"
assert_contains "sim has cat1" "doc:cat1" "$sim"
assert_not_contains "sim excludes dog" "doc:dog" "$sim"
sim_first=$(printf '%s\n' "$sim" | head -1)
assert_eq "sim best first" "doc:cat2" "$(printf '%s' "$sim_first" | cut -f1)"
assert_contains "sim line format" "doc:cat2	0." "$sim"

assert_eq "sim k=1 count" "1" "$(curl -s -X POST "$BASE/sim?k=1" -d 'cats sit on mats' | wc -l)"
assert_eq "sim k=1 top" "doc:cat2" "$(curl -s -X POST "$BASE/sim?k=1" -d 'cats sit on mats' | cut -f1)"
assert_contains "sim json" '"results"' "$(curl -s -X POST "$BASE/sim?k=1&json=1" -d 'cats sit on mats')"
assert_contains "sim json key" '"key":"doc:cat2"' "$(curl -s -X POST "$BASE/sim?k=1&json=1" -d 'cats sit on mats')"
assert_contains "sim json sim" '"sim":0.426401' "$(curl -s -X POST "$BASE/sim?k=2&json=1" -d 'cats sit on mats')"

assert_eq "sim empty body" "" "$(curl -s -X POST "$BASE/sim" -d '')"
assert_eq "sim unrelated" "" "$(curl -s -X POST "$BASE/sim" -d 'zzz qqq vvv')"

assert_eq "embed ttl set" "ok doc:ttl 256" "$(curl -s -X POST "$BASE/embed?key=doc:ttl&ttl=1" -d 'temporary thought')"
assert_contains "embed ttl alive" "dim 256" "$(curl -s "$BASE/embed?key=doc:ttl")"
sleep 2
assert_eq "embed ttl expired" "missing" "$(curl -s "$BASE/embed?key=doc:ttl")"
assert_not_contains "sim skips expired" "doc:ttl" "$(curl -s -X POST "$BASE/sim" -d 'temporary thought')"

assert_eq "embed del" "ok" "$(curl -s -X DELETE "$BASE/embed?key=doc:cat1")"
assert_eq "embed get after del" "missing" "$(curl -s "$BASE/embed?key=doc:cat1")"
assert_eq "embed del missing" "missing" "$(curl -s -X DELETE "$BASE/embed?key=doc:cat1")"
assert_eq "embed del status" "404" "$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$BASE/embed?key=doc:cat1")"
assert_not_contains "sim after del" "doc:cat1" "$(curl -s -X POST "$BASE/sim" -d 'cats sit on mats')"

curl -s -X POST "$BASE/set?key=doc:dog" -d 'the dog barked at the mailman' > /dev/null
curl -s -X POST "$BASE/embed?key=doc:dog" -d 'the dog barked at the mailman' > /dev/null
assert_eq "del main key" "ok" "$(curl -s -X DELETE "$BASE/del?key=doc:dog")"
assert_eq "del drops vector" "missing" "$(curl -s "$BASE/embed?key=doc:dog")"
assert_eq "del keeps other vectors" "dim 256" "$(curl -s "$BASE/embed?key=doc:cat2" | cut -d' ' -f1-2)"
curl -s -X POST "$BASE/embed?key=doc:ghost" -d 'vector without a main key' > /dev/null
assert_eq "del vector-only key" "missing" "$(curl -s -X DELETE "$BASE/del?key=doc:ghost")"
assert_eq "del vector-only drops vec" "missing" "$(curl -s "$BASE/embed?key=doc:ghost")"

assert_eq "batch embed array" "embed b:1 ok 256" "$(curl -s -X POST "$BASE/batch" -d '[["embed","b:1","hello vector world"]]')"
assert_eq "batch embed object" "embed b:2 ok 256" "$(curl -s -X POST "$BASE/batch" -d '[{"embed":"b:2","value":"second vector here"}]')"
assert_eq "batch embed stored" "dim 256" "$(curl -s "$BASE/embed?key=b:1" | cut -d' ' -f1-2)"
assert_eq "batch embed bad" "error: bad batch op" "$(curl -s -X POST "$BASE/batch" -d '[["embed","b:3"]]')"
assert_eq "batch del cascades" "del b:1 missing" "$(curl -s -X POST "$BASE/batch" -d '[["del","b:1"]]')"
assert_eq "batch del dropped vector" "missing" "$(curl -s "$BASE/embed?key=b:1")"
assert_eq "sim finds batch vector" "b:2" "$(curl -s -X POST "$BASE/sim" -d 'second vector here' | cut -f1)"

assert_contains "spec documents vectors" "exovec" "$(curl -s "$BASE/")"
assert_contains "spec embeds endpoints" "/embed" "$(curl -s "$BASE/")"
assert_contains "spec documents batch embed" "embed k ok" "$(curl -s "$BASE/")"

stop_server

echo "=== session 16: vector persistence across restart ==="
start_server
assert_contains "persist embed get" "dim 256" "$(curl -s "$BASE/embed?key=doc:alpha")"
assert_eq "persist del cascade" "missing" "$(curl -s "$BASE/embed?key=doc:dog")"
assert_eq "persist batch vector" "dim 256" "$(curl -s "$BASE/embed?key=b:2" | cut -d' ' -f1-2)"
sim=$(curl -s -X POST "$BASE/sim" -d 'a cat on a mat')
assert_contains "persist sim ranking" "doc:cat2" "$sim"
assert_not_contains "persist sim excludes deleted" "doc:dog" "$sim"
stop_server

echo "=== session 17: scoped tokens and vectors ==="
cat > "$DATA/tokens2" <<'EOF'
# vector scope test tokens
reader:ro
vecs:scope=vec:*
vlogs:scope=vec:logs/*
logs:scope=logs/*
EOF
start_server --token master --tokens "$DATA/tokens2"
MASTER="Authorization: Bearer master"
READER="Authorization: Bearer reader"
VECS="Authorization: Bearer vecs"
VLOGS="Authorization: Bearer vlogs"
LOGS="Authorization: Bearer logs"

assert_eq "master embed" "ok scoped 256" "$(curl -s -H "$MASTER" -X POST "$BASE/embed?key=scoped" -d 'scope test text')"
assert_eq "vecs embed allowed" "ok scoped 256" "$(curl -s -H "$VECS" -X POST "$BASE/embed?key=scoped" -d 'scope test text')"
assert_eq "vecs get allowed" "dim 256" "$(curl -s -H "$VECS" "$BASE/embed?key=scoped" | cut -d' ' -f1-2)"
assert_eq "vecs del allowed" "ok" "$(curl -s -H "$VECS" -X DELETE "$BASE/embed?key=scoped")"
assert_eq "vecs get after del" "missing" "$(curl -s -H "$VECS" "$BASE/embed?key=scoped")"

assert_eq "ro embed denied" "error: denied" "$(curl -s -H "$READER" -X POST "$BASE/embed?key=x" -d 'nope')"
assert_eq "ro embed denied status" "403" "$(curl -s -o /dev/null -w '%{http_code}' -H "$READER" -X POST "$BASE/embed?key=x" -d 'nope')"
assert_eq "ro embed del denied" "error: denied" "$(curl -s -H "$READER" -X DELETE "$BASE/embed?key=doc:alpha")"
assert_eq "ro embed get allowed" "dim 256" "$(curl -s -H "$READER" "$BASE/embed?key=doc:alpha" | cut -d' ' -f1-2)"
assert_contains "ro sim allowed" "doc:alpha" "$(curl -s -H "$READER" -X POST "$BASE/sim" -d 'quick brown fox')"

assert_eq "logs embed denied" "error: denied" "$(curl -s -H "$LOGS" -X POST "$BASE/embed?key=logs/a" -d 'x')"
assert_eq "logs sim sees nothing" "" "$(curl -s -H "$LOGS" -X POST "$BASE/sim" -d 'quick brown fox')"
assert_eq "vlogs embed allowed" "ok logs/a 256" "$(curl -s -H "$VLOGS" -X POST "$BASE/embed?key=logs/a" -d 'log entry one')"
assert_eq "vlogs get own" "dim 256" "$(curl -s -H "$VLOGS" "$BASE/embed?key=logs/a" | cut -d' ' -f1-2)"
assert_contains "vlogs sim sees own" "logs/a" "$(curl -s -H "$VLOGS" -X POST "$BASE/sim" -d 'log entry one')"
assert_not_contains "vlogs sim hides other" "doc:alpha" "$(curl -s -H "$VLOGS" -X POST "$BASE/sim" -d 'log entry one')"

assert_eq "batch embed ro denied" "embed nope denied" "$(curl -s -H "$READER" -X POST "$BASE/batch" -d '[["embed","nope","v"]]')"
assert_eq "batch embed scoped denied" "embed other/x denied" "$(curl -s -H "$LOGS" -X POST "$BASE/batch" -d '[["embed","other/x","v"]]')"
assert_eq "batch embed logs denied" "embed logs/b denied" "$(curl -s -H "$LOGS" -X POST "$BASE/batch" -d '[["embed","logs/b","v"]]')"
assert_eq "batch embed vlogs allowed" "embed logs/b ok 256" "$(curl -s -H "$VLOGS" -X POST "$BASE/batch" -d '[["embed","logs/b","log entry two"]]')"
stop_server

echo "=== session 18: snapshot and restore round-trip vectors ==="
start_server
vexp=$(curl -s "$BASE/embed?key=doc:alpha")
curl -s "$BASE/snapshot" > "$DATA/snap18"
assert_contains "snapshot has vec key" "vec:doc:alpha" "$(cat "$DATA/snap18")"
assert_contains "snapshot has vec prefix" "vec:" "$(cat "$DATA/snap18")"
N=$(curl -s "$BASE/list?limit=10000" | wc -l)
assert_eq "restore with vectors" "ok $N" "$(curl -s -X POST "$BASE/restore" --data-binary @"$DATA/snap18")"
assert_eq "restored vec byte-exact" "$vexp" "$(curl -s "$BASE/embed?key=doc:alpha")"
assert_contains "restored sim works" "doc:cat2" "$(curl -s -X POST "$BASE/sim" -d 'a cat on a mat')"
assert_contains "restored batch vec" "dim 256" "$(curl -s "$BASE/embed?key=b:2" | cut -d' ' -f1-2)"
stop_server

echo "=== session 19: vector crash safety (SIGKILL mid-embed-batch) ==="
start_server
for i in $(seq 1 40); do
    curl -s -X POST "$BASE/embed?key=vc:$i" -d "the cat sat on the mat number $i" > /dev/null
done
curl -s "$BASE/embed?key=vc:7" > "$DATA/vec7"
{
    printf '['
    for i in $(seq 1 400); do
        [ "$i" -gt 1 ] && printf ','
        printf '["embed","vb:%d","batch vector number %d"]' "$i" "$i"
    done
    printf ']'
} > "$DATA/bigembed.json"
curl -s -X POST "$BASE/batch" --data-binary @"$DATA/bigembed.json" > /dev/null &
BGP=$!
sleep 0.05
kill -9 "$SRV" 2>/dev/null
wait "$SRV" 2>/dev/null
wait "$BGP" 2>/dev/null

start_server
assert_eq "crash store loads" "pong" "$(curl -s "$BASE/ping")"
assert_eq "crash vector preserved" "$(cat "$DATA/vec7")" "$(curl -s "$BASE/embed?key=vc:7")"
assert_eq "crash sim ranks unique" "vc:7" "$(curl -s -X POST "$BASE/sim" -d 'cat sat on the mat number 7' | head -1 | cut -f1)"
BROKEN=0
for k in $(curl -s "$BASE/list?prefix=vec:"); do
    case "$(curl -s "$BASE/embed?key=${k#vec:}")" in
        dim*) ;;
        *) BROKEN=1 ;;
    esac
done
assert_eq "crash no corrupt vectors" "0" "$BROKEN"
stop_server

# ---- console surface: keys subcommands --------------------------------------
KEYS="$DATA/keys.txt"
r=$("$BIN" keys add one:ro --keys "$KEYS")
assert_eq "keys add" "ok key added" "$r"
r=$("$BIN" keys add two:scope=logs/* --keys "$KEYS")
assert_eq "keys add second" "ok key added" "$r"
r=$("$BIN" keys add one:ro --keys "$KEYS" 2>&1)
assert_contains "keys add duplicate rejected" "already present" "$r"
r=$("$BIN" keys list --keys "$KEYS")
assert_contains "keys list shows entries" "two:scope=logs/*" "$r"
r=$("$BIN" keys remove one --keys "$KEYS")
assert_eq "keys remove" "ok key removed" "$r"
r=$("$BIN" keys remove one --keys "$KEYS")
assert_eq "keys remove missing" "missing key" "$r"
r=$("$BIN" keys list --keys "$KEYS")
assert_not_contains "keys list after remove" "one:ro" "$r"

# ---- console surface: --help modules ----------------------------------------
r=$("$BIN" --help modules)
assert_contains "help modules lists exomind" "## exomind" "$r"
assert_contains "help modules lists exokit" "## exokit" "$r"
r=$("$BIN" --help exosched)
assert_contains "help for one module" "# exosched" "$r"

# ---- console surface: MCP stdio mode ----------------------------------------
MCPOUT=$(printf '%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
  '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"set","arguments":{"key":"mcp:a","value":"mcpv"}}}' \
  '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"get","arguments":{"key":"mcp:a"}}}' \
  | "$BIN" --mcp --data "$DATA/mcp.dat")
assert_contains "mcp initialize" '"serverInfo":{"name":"exomind"' "$MCPOUT"
assert_contains "mcp tools/list" '"name":"search"' "$MCPOUT"
assert_contains "mcp set ok" '"text":"ok"' "$MCPOUT"
assert_contains "mcp get value" '"text":"mcpv"' "$MCPOUT"

# ---- console surface: one-shot operations (no server) ------------------------
C="$BIN --data $DATA/console.dat"
rm -f "$DATA/console.dat"

guide=$("$BIN" --data "$DATA/console.dat" 2>/dev/null | head -1)
assert_contains "console no-args guide" "# exomind" "$guide"

r=$("$BIN" /set?key=con:a --body alpha --data "$DATA/console.dat" 2>/dev/null)
assert_eq "console op /set" "ok" "$r"

r=$("$BIN" /get?key=con:a --data "$DATA/console.dat" 2>/dev/null)
assert_eq "console op /get" "alpha" "$r"

r=$(printf 'beta' | "$BIN" /append?key=con:a --data "$DATA/console.dat" 2>/dev/null)
assert_eq "console op /append stdin body" "ok" "$r"

r=$("$BIN" /get?key=con:a --data "$DATA/console.dat" 2>/dev/null)
assert_eq "console append persisted" "alpha
beta" "$r"

r=$("$BIN" /list?prefix=con: --data "$DATA/console.dat" 2>/dev/null)
assert_contains "console op /list prefix" "con:a" "$r"

r=$("$BIN" /search?q=beta --data "$DATA/console.dat" 2>/dev/null)
assert_contains "console op /search" "con:a" "$r"

r=$("$BIN" /note --body 'console note test' --data "$DATA/console.dat" 2>/dev/null)
assert_contains "console op /note" "note:" "$r"

r=$("$BIN" /get?key=missing --data "$DATA/console.dat" 2>/dev/null)
assert_eq "console op missing key exits 1" "1" "$?"

r=$("$BIN" /set?key=con:b%20x --body spacers --data "$DATA/console.dat" 2>/dev/null)
r=$("$BIN" /get?key=con:b%20x --data "$DATA/console.dat" 2>/dev/null)
assert_eq "console op decodes %20 in query" "spacers" "$r"

PORT2=$((PORT + 1))
"$BIN" --serve --port "$PORT2" --data "$DATA/serve.dat" \
    --project-root "$DATA" 2>/dev/null &
SV=$!
for _ in $(seq 1 100); do
    curl -s -o /dev/null "http://127.0.0.1:$PORT2/ping" 2>/dev/null && break
    sleep 0.05
done
r=$(curl -s "http://127.0.0.1:$PORT2/ping")
assert_eq "serve flag binds the daemon" "pong" "$r"
kill "$SV" 2>/dev/null
wait "$SV" 2>/dev/null

r=$("$BIN" /set?key=snap:x --body snapval --data "$DATA/console.dat" \
    --project-root "$DATA" >/dev/null 2>&1; echo $?)
assert_eq "console op with --project-root ok" "0" "$r"

r=$("$BIN" /snapshot --data "$DATA/console.dat" 2>/dev/null)
assert_contains "console op /snapshot" "snap:x" "$r"

r=$("$BIN" /batch --body '[["set","b1","v1"]]' \
    --data "$DATA/console.dat" 2>/dev/null)
assert_contains "console op /batch" "set b1 ok" "$r"
r=$("$BIN" /get?key=b1 --data "$DATA/console.dat" 2>/dev/null)
assert_eq "console op /batch wrote" "v1" "$r"

# ---- server surface: /exo<module> prefix + rate limit -----------------------
start_server
r=$(curl -s -m 3 "$BASE/exoexomind/ping")
assert_eq "module prefix ping" "pong" "$r"
r=$(curl -s -m 3 "$BASE/exoexomind/")
assert_contains "module prefix root = usage" "# exomind" "$r"
stop_server

start_server --rate-limit 2
PIDS=""
for i in 1 2 3 4 5 6; do
    curl -s -m 3 -o /dev/null -w '%{http_code} ' "$BASE/ping" &
    PIDS="$PIDS $!"
done
wait $PIDS
# sequential requests let the bucket refill: only a burst trips the limit
R429=$(for i in 1 2 3 4 5 6; do curl -s -m 3 -o /dev/null -w '%{http_code}\n' "$BASE/ping" & done | grep -c 429)
assert_eq "rate limit yields 429s on burst" "6" "$R429"
stop_server

# ---- memory model: project store, backups, associations, mandate ----------
PROJ="$DATA/proj"
mkdir -p "$PROJ/.exo"
"$BIN" --project-root "$PROJ" --backup "$DATA/backups" \
    --mandate "MANDATE: always read agent:<id> keys first." \
    --port $PORT --data "$DATA/exomind.dat" >"$DATA/mem.log" 2>&1 &
MEM_PID=$!
sleep 0.8
r=$(curl -s -m 3 "$BASE/ping")
assert_eq "memory daemon up" "pong" "$r"
r=$(curl -s -m 3 "$BASE/project")
assert_contains "project store located" "$PROJ/.exo/project.dat" "$r"
r=$(curl -s -m 3 -X POST "$BASE/set?key=p:design:dec" -d 'contract first')
assert_eq "project key set" "ok" "$r"
r=$(curl -s -m 3 "$BASE/get?key=p:design:dec")
assert_eq "project key get" "contract first" "$r"
r=$(curl -s -m 3 -X POST "$BASE/set?key=general:fact" -d 'general memory ok')
r=$(curl -s -m 3 "$BASE/get?key=general:fact")
assert_eq "main memory unaffected" "general memory ok" "$r"
assert_contains "project file exists on disk" "project.dat" \
    "$(ls "$PROJ/.exo/")"
assert_contains "backup written at startup" "exomind-" \
    "$(ls "$DATA/backups/")"
r=$(curl -s -m 3 -X POST "$BASE/backup")
assert_eq "manual backup" "ok" "$r"
BCOUNT=$(ls "$DATA/backups" | grep -c '^exomind-')
assert_eq "two backups after manual" "2" "$BCOUNT"

# associations
r=$(curl -s -m 3 -X POST "$BASE/outdate?key=p:design:dec&reason=superseded")
assert_eq "outdate ok" "ok" "$r"
r=$(curl -s -m 3 "$BASE/outdated?key=p:design:dec")
assert_contains "outdated shows marker" "superseded" "$r"
assert_contains "outdated keeps value" "contract first" \
    "$(curl -s -m 3 "$BASE/get?key=p:design:dec")"
r=$(curl -s -m 3 -X POST "$BASE/link?from=p:design:dec&to=p:new:plan&rel=leads-to")
assert_eq "link ok" "ok" "$r"
r=$(curl -s -m 3 -X POST "$BASE/set?key=p:new:plan" -d 'exokit port')
r=$(curl -s -m 3 "$BASE/assoc?key=p:design:dec")
assert_contains "assoc outgoing" "p:new:plan" "$r"
r=$(curl -s -m 3 "$BASE/recall?q=contract")
assert_contains "recall shows the key" "p:design:dec" "$r"
assert_contains "recall shows history" "superseded" "$r"
r=$(curl -s -m 3 "$BASE/mandate")
assert_contains "mandate served" "MANDATE" "$r"
assert_contains "mandate ack instruction" "agent:<your-id>:ready" "$r"

if [ "$FAILS" -ne 0 ]; then
    cp "$DATA/server.log" /tmp/opencode/fail-server.log 2>/dev/null
    echo "DATA=$DATA"
fi
rm -rf "$DATA"

if [ "$FAILS" -eq 0 ]; then
    echo "ALL TESTS PASSED"
    exit 0
fi
echo "$FAILS test(s) FAILED"
exit 1
