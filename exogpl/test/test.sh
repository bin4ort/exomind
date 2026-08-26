#!/bin/sh
# exogpl offline + smoke test suite
set -e
cd "$(dirname "$0")/.."

BIN=./exogpl
ok=0
fail=0

check() {
    if printf '%s' "$3" | grep -q -- "$2"; then
        echo "ok   - $1"
        ok=$((ok+1))
    else
        echo "FAIL - $1 (expected: $2)"
        fail=$((fail+1))
    fi
}

# --- console ops (single invocation, no state needed) ---

ver=$($BIN --version 2>&1)
check "version flag" "exogpl v" "$ver"

spec=$($BIN 2>&1)
check "no-arg spec" "GPL v3.0" "$spec"

ping=$($BIN /ping 2>&1)
check "ping op" "pong" "$ping"

help=$($BIN --help 2>&1)
check "help shows usage" "usage:" "$help"

# --- server-mode tests (multi-step with state) ---
if command -v curl >/dev/null 2>&1; then
    PORT=17661
    # Start server in background
    $BIN --serve --port $PORT </dev/null >/dev/null 2>&1 &
    SERVER_PID=$!
    
    # Wait for server to be ready
    for i in 1 2 3 4 5 6 7 8 9 10; do
        curl -s "http://127.0.0.1:$PORT/ping" >/dev/null 2>&1 && break
        sleep 0.5
    done
    
    # Test ping over HTTP
    ping_http=$(curl -s "http://127.0.0.1:$PORT/ping" 2>&1)
    check "ping over HTTP" "pong" "$ping_http"
    
    # Test audit
    audit=$(curl -s "http://127.0.0.1:$PORT/audit?path=src" 2>&1) || true
    check "audit src" "Files scanned:" "$audit"
    
    # Test report after audit (same process, state persists)
    report=$(curl -s "http://127.0.0.1:$PORT/report" 2>&1) || true
    check "report after audit" "Compliance Report" "$report"
    
    # Test file audit
    file_audit=$(curl -s "http://127.0.0.1:$PORT/file?path=src/main.c" 2>&1) || true
    check "file audit" "Has GPL header:" "$file_audit"
    
    # Test deps
    deps=$(curl -s "http://127.0.0.1:$PORT/deps?path=." 2>&1) || true
    check "deps scan" "Dependency scan" "$deps"
    
    # Test missing param error
    err=$($BIN /audit 2>&1) || true
    check "missing param error" "error:" "$err"
    
    # Kill server
    kill $SERVER_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true
else
    echo "skip - curl not present; cannot server-test"
fi

echo "----"
echo "passed=$ok failed=$fail"
