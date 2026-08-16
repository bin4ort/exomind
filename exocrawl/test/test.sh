#!/usr/bin/env bash
# exocrawl test suite: hermetic (local mock web server).
# Covers: HTML->text extraction (boilerplate removal, headings, links,
# images, entities, pre verbatim), /fetch, /scrape concurrency, retry
# on 403, search via mock searxng, stats, auth, error paths.
set -u
cd "$(dirname "$0")/.."
BIN=build/exocrawl
if ! timeout 60 make -s exocrawl; then
    echo "build failed" >&2
    exit 1
fi

TDIR=$(mktemp -d /tmp/opencode/exocrawl-XXXXXX)
trap 'pkill -f "mock_web.py" 2>/dev/null; rm -rf "$TDIR"' EXIT


PORT=$((18500 + RANDOM % 500))
CRL=7777
BASE="http://127.0.0.1:$CRL"
WEB="http://127.0.0.1:$PORT"

# kill any stale daemon squatting on the test port
OLDPID=$(ss -tlnp 2>/dev/null | grep ":$CRL " | grep -oP 'pid=\K[0-9]+' | head -1)
[ -n "$OLDPID" ] && kill $OLDPID 2>/dev/null

python3 test/mock_web.py $PORT > /dev/null 2>&1 &
MOCK_PID=$!
sleep 1

setsid nohup "$BIN" --port $CRL --concurrency 8 --pace-ms 0 --engine-base "http://127.0.0.1:$PORT" \
    > "$TDIR/crawl.log" 2>&1 < /dev/null &
CRL_PID=$!
sleep 1

PASS=0
FAILED=0
check() { # desc cond detail
    if [ "$2" = "0" ]; then
        PASS=$((PASS + 1))
        printf 'ok   %-58s\n' "$1"
    else
        FAILED=$((FAILED + 1))
        printf 'FAIL %-58s [%s]\n' "$1" "$3"
    fi
}

# =============== ping + spec ============================================
check "ping" "$(timeout 5 curl -s $BASE/ping | grep -q pong && echo 0 || echo 1)" ""
SPEC=$(timeout 5 curl -s $BASE/)
check "self-describing spec" "$(echo "$SPEC" | grep -q '/search' && echo 0 || echo 1)" ""

# =============== extraction: /fetch =====================================
OUT=$(timeout 30 curl -s "$BASE/fetch?url=$WEB/html/")
check "fetch: title as heading" "$(echo "$OUT" | grep -q '# Test Article Title' && echo 0 || echo 1)" "$OUT"
check "fetch: h1 kept" "$(echo "$OUT" | grep -q 'Main Heading Here' && echo 0 || echo 1)" ""
check "fetch: paragraph kept" "$(echo "$OUT" | grep -q 'First paragraph with an & entity' && echo 0 || echo 1)" ""
check "fetch: nav stripped" "$(echo "$OUT" | grep -q 'Navigation' && echo 1 || echo 0)" ""
check "fetch: ad stripped" "$(echo "$OUT" | grep -q 'BUY NOW' && echo 1 || echo 0)" ""
check "fetch: footer stripped" "$(echo "$OUT" | grep -q 'Copyright footer' && echo 1 || echo 0)" ""
check "fetch: pre verbatim" "$(echo "$OUT" | grep -q 'code = "verbatim <not escaped>"' && echo 0 || echo 1)" ""
check "fetch: list items" "$(echo "$OUT" | grep -q -- '- List item one' && echo 0 || echo 1)" ""
check "fetch: entity decoded" "$(echo "$OUT" | grep -qF '© 2026' && echo 0 || echo 1)" ""
OUT2=$(timeout 30 curl -s "$BASE/fetch?url=$WEB/html/&links=1&images=1")
check "fetch: links section" "$(echo "$OUT2" | grep -q 'link to the thing.*http://127.0.0.1:'$PORT'/wiki/thing' && echo 0 || echo 1)" ""
check "fetch: image section" "$(echo "$OUT2" | grep -q 'The Logo.*img/logo.png' && echo 0 || echo 1)" ""
check "fetch: max cap" "$([ "$(timeout 30 curl -s "$BASE/fetch?url=$WEB/html/&max=40" | wc -c)" -le 60 ] && echo 0 || echo 1)" ""

# =============== error paths ============================================
check "fetch: bad url" "$(timeout 5 curl -s "$BASE/fetch?url=ftp://x" | grep -q 'error: url must be http' && echo 0 || echo 1)" ""
check "fetch: missing url" "$(timeout 5 curl -s "$BASE/fetch" | grep -q 'error: missing url' && echo 0 || echo 1)" ""
check "fetch: 403 retry -> error" "$(timeout 30 curl -s "$BASE/fetch?url=$WEB/deny" | grep -q 'error: http 403' && echo 0 || echo 1)" ""
check "unknown path" "$(timeout 5 curl -s $BASE/nope | grep -q 'unknown path' && echo 0 || echo 1)" ""

# =============== /scrape concurrency ====================================
BODY="$WEB/html/
$WEB/html/&x=1
$WEB/html/&y=2
$WEB/deny"
S=$(timeout 60 curl -s -X POST $BASE/scrape --data-binary "$BODY")
check "scrape: ok count" "$(echo "$S" | head -1 | grep -q '^ok 4$' && echo 0 || echo 1)" "$S"
check "scrape: 3 fetched" "$([ "$(echo "$S" | grep -c ' ok$')" -eq 3 ] && echo 0 || echo 1)" ""
check "scrape: 1 error" "$([ "$(echo "$S" | grep -c 'error: http 403')" -eq 1 ] && echo 0 || echo 1)" "$S"

# =============== independent search engines =============================
SE=$(timeout 30 curl -s "$BASE/search?q=test&n=8&engines=ddg,mojeek,marginalia,bing,wikipedia")
check "search: ddg result" "$(echo "$SE" | grep -q 'DDG Result Title' && echo 0 || echo 1)" "$SE"
check "search: ddg url decoded" "$(echo "$SE" | grep -q 'example.org/page' && echo 0 || echo 1)" ""
check "search: ddg ad filtered" "$(echo "$SE" | grep -q 'Sponsored Ad' && echo 1 || echo 0)" ""
check "search: mojeek result" "$(echo "$SE" | grep -q 'Mojeek One' && echo 0 || echo 1)" ""
check "search: marginalia result" "$(echo "$SE" | grep -q 'Marginalia X' && echo 0 || echo 1)" ""
check "search: bing result" "$(echo "$SE" | grep -q 'Bing B' && echo 0 || echo 1)" ""
check "search: wikipedia result" "$(echo "$SE" | grep -q 'Wiki One' && echo 0 || echo 1)" ""
SE2=$(timeout 30 curl -s "$BASE/search?q=test&n=2&engines=mojeek")
check "search: engine filter" "$(echo "$SE2" | grep -q 'Mojeek One' && ! echo "$SE2" | grep -q 'DDG Result' && echo 0 || echo 1)" "$SE2"

# =============== stats ==================================================
ST=$(timeout 5 curl -s $BASE/stats)
check "stats: fetches counted" "$([ "$(echo "$ST" | grep -oP 'fetches: \K[0-9]+')" -ge 5 ] && echo 0 || echo 1)" "$ST"
check "stats: errors counted" "$([ "$(echo "$ST" | grep -oP 'errors: \K[0-9]+')" -ge 1 ] && echo 0 || echo 1)" ""

# =============== auth ===================================================
kill $CRL_PID 2>/dev/null
sleep 1
setsid nohup "$BIN" --port $CRL --token sekret --concurrency 4 --pace-ms 0 \
    > "$TDIR/crawl2.log" 2>&1 < /dev/null &
CRL_PID=$!
for i in $(seq 1 20); do
    if timeout 2 curl -s $BASE/ping 2>/dev/null | grep -q unauthorized; then
        break
    fi
    sleep 1
done
check "auth: no token rejected" "$(timeout 5 curl -s $BASE/ping | grep -q unauthorized && echo 0 || echo 1)" ""
check "auth: wrong token rejected" "$(timeout 5 curl -s -H 'Authorization: Bearer nope' $BASE/ping | grep -q unauthorized && echo 0 || echo 1)" ""
check "auth: right token ok" "$(timeout 5 curl -s -H 'Authorization: Bearer sekret' $BASE/ping | grep -q pong && echo 0 || echo 1)" ""
kill $CRL_PID 2>/dev/null

echo "=== exocrawl tests: $PASS ok, $FAILED fail ==="
[ "$FAILED" -eq 0 ]
