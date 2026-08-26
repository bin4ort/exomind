#!/bin/sh
# exogit offline + smoke test suite
set -e
cd "$(dirname "$0")/.."

BIN=./exogit
ok=0
fail=0

check() {
    # $1 = description, $2 = expected substring, $3 = actual output
    if printf '%s' "$3" | grep -q -- "$2"; then
        echo "ok   - $1"
        ok=$((ok+1))
    else
        echo "FAIL - $1 (expected: $2)"
        fail=$((fail+1))
    fi
}

ver=$($BIN --version 2>&1)
check "version flag" "exogit v" "$ver"

spec=$($BIN 2>&1)
check "no-arg spec" "exogit v" "$spec"

ping=$($BIN /ping 2>&1)
check "ping op" "pong" "$ping"

st=$($BIN /selftest 2>&1)
check "parser selftest" "selftest: ok" "$st"

help=$($BIN --help 2>&1)
check "help shows usage" "usage:" "$help"

# Network-smoke (best-effort; skip if no connectivity)
if command -v curl >/dev/null 2>&1; then
    netout=$($BIN /file?url=https://raw.githubusercontent.com/octocat/Hello-World/master/README 2>&1) || true
    if printf '%s' "$netout" | grep -q .; then
        echo "ok   - network /file returned data"
        ok=$((ok+1))
    else
        echo "skip - network /file (no connectivity / rate-limited)"
    fi
else
    echo "skip - curl not present; cannot network-test"
fi

echo "----"
echo "passed=$ok failed=$fail"
if [ "$fail" -gt 0 ]; then
    exit 1
fi
