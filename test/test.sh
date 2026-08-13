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

start_server() {
    "$BIN" --host 127.0.0.1 --port "$PORT" --data "$DATA/exomind.dat" \
        "$@" 2>"$DATA/server.log" &
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

rm -rf "$DATA"

if [ "$FAILS" -eq 0 ]; then
    echo "ALL TESTS PASSED"
    exit 0
fi
echo "$FAILS test(s) FAILED"
exit 1
