#!/usr/bin/env bash
# exoqms-ui test suite: fixture-based, own temp dir.
# Asserts: bad fixture fires all 7 checks (counts pinned), good fixture is
# clean (0 findings, exact summary line, exit 0), --json is valid JSON with
# all 7 check ids, --no-emoji and --emoji-allowlist suppress emoji findings,
# garbage input (NUL bytes, unclosed tags, 10MB) never crashes, exit codes
# 0/1/2 are correct, summary line format is exact.
set -u
cd "$(dirname "$0")/.."
BIN=build/exoqms-ui
if ! timeout 60 make -s exoqms-ui; then
    echo "build failed" >&2
    exit 1
fi

TDIR=$(mktemp -d /tmp/opencode/b2t-XXXXXX)
trap 'rm -rf "$TDIR"' EXIT

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

FIND='^\(major\|minor\) '

# =============== run 1: bad fixture = all 7 checks =======================
timeout 30 "$BIN" fixtures/bad.html > "$TDIR/bad.out" 2>/dev/null
BAD_RC=$?
BAD_N=$(grep -c "$FIND" "$TDIR/bad.out")
for c in emoji-icon overlap misalign corner-mismatch background sdk-default contrast; do
    n=$(grep -c " $c " "$TDIR/bad.out")
    check "bad: $c fires" "$([ "$n" -ge 1 ] && echo 0 || echo 1)" "n=$n"
done
check "bad: 12 findings (10 major)" \
    "$([ "$BAD_N" -eq 12 ] && [ "$(grep -c '^major ' "$TDIR/bad.out")" -eq 10 ] && echo 0 || echo 1)" \
    "n=$BAD_N"
check "bad: summary line exact" \
    "$(grep -qx '=== findings: 12 (10 major) ===' "$TDIR/bad.out" && echo 0 || echo 1)" \
    "$(grep '===' "$TDIR/bad.out" 2>/dev/null)"
check "bad: exit code 1" "$([ "$BAD_RC" -eq 1 ] && echo 0 || echo 1)" "rc=$BAD_RC"

# =============== run 2: good fixture = clean ==============================
timeout 30 "$BIN" fixtures/good.html > "$TDIR/good.out" 2>/dev/null
GOOD_RC=$?
GOOD_N=$(grep -c "$FIND" "$TDIR/good.out")
check "good: 0 findings" "$([ "$GOOD_N" -eq 0 ] && echo 0 || echo 1)" "n=$GOOD_N"
check "good: summary line exact" \
    "$(grep -qx '=== findings: 0 (0 major) ===' "$TDIR/good.out" && echo 0 || echo 1)" \
    "$(grep '===' "$TDIR/good.out" 2>/dev/null)"
check "good: exit code 0" "$([ "$GOOD_RC" -eq 0 ] && echo 0 || echo 1)" "rc=$GOOD_RC"

# =============== run 3: --json ===========================================
timeout 30 "$BIN" fixtures/bad.html --json > "$TDIR/bad.json" 2>/dev/null
check "json: parses" "$(python3 -m json.tool < "$TDIR/bad.json" >/dev/null 2>&1 && echo 0 || echo 1)" ""
check "json: 7 check ids present" \
    "$(python3 -c "
import json
d = json.load(open('$TDIR/bad.json'))
ids = set(x['check'] for x in d)
want = {'emoji-icon','overlap','misalign','corner-mismatch','background','sdk-default','contrast'}
print(0 if ids == want else 1)
" 2>/dev/null)" ""

# =============== run 4: --no-emoji / allowlist ===========================
timeout 30 "$BIN" fixtures/bad.html --no-emoji > "$TDIR/noemoji.out" 2>/dev/null
check "--no-emoji: no emoji-icon findings" \
    "$(grep -q 'emoji-icon' "$TDIR/noemoji.out" && echo 1 || echo 0)" ""
timeout 30 "$BIN" fixtures/bad.html --emoji-allowlist "🛒⚙️" > "$TDIR/allow.out" 2>/dev/null
check "--emoji-allowlist: no emoji-icon findings" \
    "$(grep -q 'emoji-icon' "$TDIR/allow.out" && echo 1 || echo 0)" ""

# =============== run 5: garbage = no crash ===============================
printf '<html><body><button>\x00\x00<h1>a\x00b</h1>\xff\xfe garbage</body></html>' \
    > "$TDIR/nul.html"
timeout 30 "$BIN" "$TDIR/nul.html" > "$TDIR/nul.out" 2>/dev/null
NUL_RC=$?
check "garbage: NUL bytes no crash" \
    "$([ "$NUL_RC" -lt 128 ] && echo 0 || echo 1)" "rc=$NUL_RC"

printf '<html><body><button class="x"><div><a><input>' > "$TDIR/unclosed.html"
timeout 30 "$BIN" "$TDIR/unclosed.html" > "$TDIR/uncl.out" 2>/dev/null
UNC_RC=$?
check "garbage: unclosed tags no crash" \
    "$([ "$UNC_RC" -lt 128 ] && echo 0 || echo 1)" "rc=$UNC_RC"

head -c 10000000 /dev/zero | tr '\0' 'a' > "$TDIR/big.html"
timeout 30 "$BIN" "$TDIR/big.html" > "$TDIR/big.out" 2>/dev/null
BIG_RC=$?
check "garbage: 10MB single line no crash" \
    "$([ "$BIG_RC" -lt 128 ] && echo 0 || echo 1)" "rc=$BIG_RC"
check "garbage: summary line present" \
    "$(grep -q '^=== findings: .* ===$' "$TDIR/big.out" && echo 0 || echo 1)" ""

# =============== run 6: exit codes =======================================
timeout 30 "$BIN" "$TDIR/missing.html" > /dev/null 2>&1
check "missing target: exit 2" "$([ $? -eq 2 ] && echo 0 || echo 1)" ""
timeout 30 "$BIN" --bogus 2>/dev/null
check "unknown option: exit 2" "$([ $? -eq 2 ] && echo 0 || echo 1)" ""
timeout 30 "$BIN" --version | grep -q "exoqms-ui 0.4.0-alpha.1"
check "--version" "$([ $? -eq 0 ] && echo 0 || echo 1)" ""
timeout 30 "$BIN" --help | grep -q "usage:"
check "--help" "$([ $? -eq 0 ] && echo 0 || echo 1)" ""

# =============== operation form (/audit?k=v) ============================
timeout 30 "$BIN" "/audit?file=fixtures/good.html" > "$TDIR/op.out" 2>/dev/null
check "op /audit good: exit 0" "$([ "$?" -eq 0 ] && echo 0 || echo 1)" ""
check "op /audit good: 0 findings" \
    "$([ "$(grep -c "$FIND" "$TDIR/op.out")" -eq 0 ] && echo 0 || echo 1)" \
    "$(grep "$FIND" "$TDIR/op.out")"
timeout 30 "$BIN" "/audit?file=fixtures/bad.html&json=1&no_emoji=1" \
    > "$TDIR/opbad.json" 2>/dev/null
OP_RC=$?
check "op /audit bad: exit 1" "$([ "$OP_RC" -eq 1 ] && echo 0 || echo 1)" "rc=$OP_RC"
check "op /audit json: parses" "$(python3 -m json.tool < "$TDIR/opbad.json" >/dev/null 2>&1 && echo 0 || echo 1)" ""
check "op /audit no_emoji: no emoji-icon findings" \
    "$(grep -q 'emoji-icon' "$TDIR/opbad.json" && echo 1 || echo 0)" ""
timeout 30 "$BIN" "/audit?nope=1" > /dev/null 2>&1
check "op /audit unknown param: exit 2" "$([ "$?" -eq 2 ] && echo 0 || echo 1)" ""
timeout 30 "$BIN" "/bogus?x=1" > /dev/null 2>&1
check "op unknown: exit 2" "$([ "$?" -eq 2 ] && echo 0 || echo 1)" ""

# =============== run 7: directory mode ===================================
timeout 30 "$BIN" fixtures > "$TDIR/dir.out" 2>/dev/null
DIR_RC=$?
DIR_N=$(grep -c "$FIND" "$TDIR/dir.out")
check "dir: audits both fixtures" "$([ "$DIR_N" -eq 12 ] && echo 0 || echo 1)" "n=$DIR_N"
check "dir: per-file header lines" \
    "$(grep -q 'file: .*bad.html' "$TDIR/dir.out" && grep -q 'file: .*good.html' "$TDIR/dir.out" && echo 0 || echo 1)" ""
check "dir: exit code 1" "$([ "$DIR_RC" -eq 1 ] && echo 0 || echo 1)" "rc=$DIR_RC"

# =============== summary ==================================================
SECS=$SECONDS
echo "=== exoqms-ui tests: $PASS ok, $FAILED fail (${SECS}s) ==="
[ "$FAILED" -eq 0 ]
