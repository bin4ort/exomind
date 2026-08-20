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

setsid nohup "$BIN" --serve --port $CRL --concurrency 8 --pace-ms 0 --engine-base "http://127.0.0.1:$PORT" \
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

# =============== extraction regression corpus (issue:008) =================
# HTML fixtures in test/corpus/*.html pin the extractor's output: any
# change to html.c must reproduce the expected *.txt byte-for-byte, or the
# corpus is updated consciously.
CORPUS_DIR="$(pwd)/test/corpus"
CFAILS=0
for f in "$CORPUS_DIR"/*.html; do
    base=${f%.html}
    exp="$base.txt"
    [ -f "$exp" ] || continue
    if timeout 5 "$BIN" --extract "$f" > "$TDIR/corpus-out.txt" 2>/dev/null; then
        if cmp -s "$TDIR/corpus-out.txt" "$exp"; then
            PASS=$((PASS + 1))
            printf 'PASS corpus %s\n' "$(basename "$base")"
        else
            printf 'FAIL corpus %s\n' "$(basename "$base")"
            CFAILS=$((CFAILS + 1))
        fi
    else
        printf 'FAIL corpus %s (extract error)\n' "$(basename "$base")"
        CFAILS=$((CFAILS + 1))
    fi
done
[ "$CFAILS" -eq 0 ] || FAILED=$((FAILED + CFAILS))

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

# =============== console operations (no daemon) =========================
G=$(timeout 5 "$BIN")
check "console: no args prints guide" "$(echo "$G" | grep -q 'exocrawl' && echo 0 || echo 1)" ""
check "console: guide lists /search" "$(echo "$G" | grep -q '/search' && echo 0 || echo 1)" ""
check "console: guide exit 0" "$(timeout 5 "$BIN" >/dev/null 2>&1 && echo 0 || echo 1)" ""
C=$(timeout 5 "$BIN" /stats)
check "console: /stats offline" "$(echo "$C" | grep -q 'fetches:' && echo 0 || echo 1)" "$C"
check "console: /stats exit 0" "$(timeout 5 "$BIN" /stats >/dev/null 2>&1 && echo 0 || echo 1)" ""
CF=$(timeout 30 "$BIN" "/fetch?url=$WEB/html/")
check "console: /fetch against mock" "$(echo "$CF" | grep -q 'Main Heading Here' && echo 0 || echo 1)" "$CF"
check "console: /fetch exit 0" "$(timeout 30 "$BIN" "/fetch?url=$WEB/html/" >/dev/null 2>&1 && echo 0 || echo 1)" ""
check "console: /fetch missing url exit 1" "$(timeout 5 "$BIN" /fetch >/dev/null 2>&1; [ $? -eq 1 ] && echo 0 || echo 1)" ""
"$BIN" /nope >/dev/null 2>&1
check "console: unknown op exit 2" "$([ $? -eq 2 ] && echo 0 || echo 1)" ""
CS=$(printf "$WEB/html/\n$WEB/deny\n" | timeout 60 "$BIN" /scrape)
check "console: /scrape stdin body" "$(echo "$CS" | head -1 | grep -q '^ok 2$' && echo 0 || echo 1)" "$CS"

# =============== robots.txt politeness (--robots [dir]) =================
# mock serves robots.txt with "Disallow: /private" + "Crawl-delay: 2"
RDF=$(timeout 30 "$BIN" "/fetch?url=$WEB/private/x")
check "robots: default mode ignores robots.txt" "$(echo "$RDF" | grep -q 'PRIVATE PAGE CONTENT' && echo 0 || echo 1)" "$RDF"
RDIR="$TDIR/robots-cache"
mkdir -p "$RDIR"
RB=$(timeout 30 "$BIN" --robots "$RDIR" "/fetch?url=$WEB/private/x" 2>&1)
check "robots: disallowed path skipped" "$(echo "$RB" | grep -q 'robots.txt disallows' && echo 0 || echo 1)" "$RB"
check "robots: skip is an error exit 1" "$(timeout 30 "$BIN" --robots "$RDIR" "/fetch?url=$WEB/private/x" >/dev/null 2>&1; [ $? -eq 1 ] && echo 0 || echo 1)" ""
check "robots: robots.txt cached in dir" "$([ -s "$RDIR/127.0.0.1:$PORT.txt" ] && echo 0 || echo 1)" ""
check "robots: allowed path fetched" "$(timeout 30 "$BIN" "/fetch?url=$WEB/html/" --robots "$RDIR" | grep -q 'Main Heading Here' && echo 0 || echo 1)" ""
RBE=$(EXO_CRAWL_ROBOTS="$RDIR" timeout 30 "$BIN" "/fetch?url=$WEB/private/x" 2>&1)
check "robots: env EXO_CRAWL_ROBOTS enables mode" "$(echo "$RBE" | grep -q 'robots.txt disallows' && echo 0 || echo 1)" "$RBE"
RDIR2="$TDIR/robots-cache2"
mkdir -p "$RDIR2"
T0=$(date +%s.%N)
RSC=$(printf "$WEB/html/\n$WEB/html/&x=1\n" | timeout 60 "$BIN" --robots "$RDIR2" /scrape)
T1=$(date +%s.%N)
check "robots: crawl-delay spaces same-host fetches" "$(awk -v s="$T0" -v e="$T1" 'BEGIN { exit (e - s < 3.5) }' && echo 0 || echo 1)" "$T1 - $T0"
check "robots: crawl-delay scrape both fetched" "$([ "$(echo "$RSC" | grep -c ' ok$')" -eq 2 ] && echo 0 || echo 1)" "$RSC"
RDIR3="$TDIR/robots-cache3"
mkdir -p "$RDIR3"
printf 'User-agent: *\nDisallow: /private\n' > "$RDIR3/127.0.0.1:$PORT.txt"
printf '1500\n' > "$RDIR3/127.0.0.1:$PORT.pace"
T2=$(date +%s.%N)
RSP=$(printf "$WEB/html/\n$WEB/html/&x=1\n" | timeout 60 "$BIN" --robots "$RDIR3" /scrape)
T3=$(date +%s.%N)
check "robots: per-host pace override spaced fetches" "$(awk -v s="$T2" -v e="$T3" 'BEGIN { exit (e - s < 1.1) }' && echo 0 || echo 1)" "$T3 - $T2"

# =============== extraction quality (/extract-quality) ==================
EQDIR="$(pwd)/test/fixtures/extract"
EQ=$(timeout 30 "$BIN" "/extract-quality?dir=$EQDIR")
check "extract-quality: per-fixture fooled lines" "$([ "$(echo "$EQ" | grep -c '^fixture .*fooled=yes')" -eq 4 ] && echo 0 || echo 1)" "$EQ"
check "extract-quality: overall f1 printed" "$(echo "$EQ" | grep -q '^overall: fixtures=4 .*f1=0\.8' && echo 0 || echo 1)" "$EQ"
check "extract-quality: exit 0" "$(timeout 30 "$BIN" "/extract-quality?dir=$EQDIR" >/dev/null 2>&1 && echo 0 || echo 1)" ""
check "extract-quality: missing dir exit 1" "$(timeout 5 "$BIN" /extract-quality >/dev/null 2>&1; [ $? -eq 1 ] && echo 0 || echo 1)" ""

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
