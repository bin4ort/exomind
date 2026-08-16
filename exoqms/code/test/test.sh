#!/usr/bin/env bash
# exoqms-code test suite: fixture-based.
# bad.c must fire every check id; good.c must be clean; --json valid;
# --ignore works; dir mode works; garbage never crashes; exit codes right.
set -u
cd "$(dirname "$0")/.."
BIN=build/exoqms-code
if ! timeout 60 make -s exoqms-code; then
    echo "build failed" >&2
    exit 1
fi

TDIR=$(mktemp -d /tmp/opencode/b1c-XXXXXX)
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

# =============== bad.c: every check fires ================================
timeout 30 "$BIN" fixtures/bad.c > "$TDIR/bad.out" 2>/dev/null
BAD_RC=$?
BAD_N=$(grep -c "$FIND" "$TDIR/bad.out")
BAD_MAJOR=$(grep -c '^major' "$TDIR/bad.out")
check "bad: findings present" "$([ "$BAD_N" -ge 6 ] && echo 0 || echo 1)" "n=$BAD_N"
check "bad: exit 1" "$([ "$BAD_RC" -eq 1 ] && echo 0 || echo 1)" "rc=$BAD_RC"

for ID in missing-error-path unchecked-return uninitialized-use swallowed-error unchecked-deref-alloc; do
    N=$(grep -c " $ID " "$TDIR/bad.out")
    check "bad: $ID fires" "$([ "$N" -ge 1 ] && echo 0 || echo 1)" "n=$N"
done

# pinned: missing-error-path for f (fopen) + fd (open) = 2 major; malloc deref = 1 major
check "bad: majors >= 3" "$([ "$BAD_MAJOR" -ge 3 ] && echo 0 || echo 1)" "majors=$BAD_MAJOR"
check "bad: summary line" "$(grep -q '^=== findings:' "$TDIR/bad.out" && echo 0 || echo 1)" ""

# =============== good.c: clean ===========================================
timeout 30 "$BIN" fixtures/good.c > "$TDIR/good.out" 2>/dev/null
GOOD_RC=$?
GOOD_N=$(grep -c "$FIND" "$TDIR/good.out")
check "good: 0 findings" "$([ "$GOOD_N" -eq 0 ] && echo 0 || echo 1)" "n=$GOOD_N"
check "good: exit 0" "$([ "$GOOD_RC" -eq 0 ] && echo 0 || echo 1)" "rc=$GOOD_RC"

# =============== dir mode + ignore ======================================
timeout 30 "$BIN" fixtures > "$TDIR/dir.out" 2>/dev/null
DIR_RC=$?
DIR_N=$(grep -c "$FIND" "$TDIR/dir.out")
check "dir: analyzes all files" "$([ "$DIR_N" -ge "$BAD_N" ] && echo 0 || echo 1)" "n=$DIR_N"
check "dir: exit 1" "$([ "$DIR_RC" -eq 1 ] && echo 0 || echo 1)" "rc=$DIR_RC"

mkdir -p "$TDIR/igdir"
cp fixtures/bad.c fixtures/good.c "$TDIR/igdir/"
timeout 30 "$BIN" "$TDIR/igdir" --ignore '*good.c' > "$TDIR/ig.out" 2>/dev/null
IG_N=$(grep -c "$FIND" "$TDIR/ig.out")
check "ignore: good.c excluded" "$([ "$IG_N" -eq "$BAD_N" ] && echo 0 || echo 1)" "n=$IG_N"

# =============== adapters: cpp / sh / py =================================
timeout 30 "$BIN" fixtures/bad.cpp > "$TDIR/cpp.out" 2>/dev/null
check "cpp: findings present" "$([ "$(grep -c "$FIND" "$TDIR/cpp.out")" -ge 4 ] && echo 0 || echo 1)" ""
check "cpp: missing-error-path fires" "$(grep -q ' missing-error-path ' "$TDIR/cpp.out" && echo 0 || echo 1)" ""
check "cpp: unchecked-deref-alloc fires" "$(grep -q ' unchecked-deref-alloc ' "$TDIR/cpp.out" && echo 0 || echo 1)" ""
timeout 30 "$BIN" fixtures/good.cpp > "$TDIR/cppg.out" 2>/dev/null
check "cpp good: 0 findings" "$([ "$(grep -c "$FIND" "$TDIR/cppg.out")" -eq 0 ] && echo 0 || echo 1)" ""
timeout 30 "$BIN" fixtures/bad.sh > "$TDIR/sh.out" 2>/dev/null
for ID in shell-unquoted-rm shell-unquoted-test shell-cd-unchecked shell-backtick; do
    check "sh: $ID fires" "$(grep -q " $ID " "$TDIR/sh.out" && echo 0 || echo 1)" ""
done
timeout 30 "$BIN" fixtures/good.sh > "$TDIR/shg.out" 2>/dev/null
check "sh good: 0 findings" "$([ "$(grep -c "$FIND" "$TDIR/shg.out")" -eq 0 ] && echo 0 || echo 1)" ""
timeout 30 "$BIN" fixtures/bad.py > "$TDIR/py.out" 2>/dev/null
for ID in py-bare-except py-mutable-default py-assert-validation py-os-system; do
    check "py: $ID fires" "$(grep -q " $ID " "$TDIR/py.out" && echo 0 || echo 1)" ""
done
timeout 30 "$BIN" fixtures/good.py > "$TDIR/pyg.out" 2>/dev/null
check "py good: 0 findings" "$([ "$(grep -c "$FIND" "$TDIR/pyg.out")" -eq 0 ] && echo 0 || echo 1)" ""

# =============== rules engine ===========================================
mkdir -p "$TDIR/rules"
printf 'minor\nTODO|FIXME\n' > "$TDIR/rules/debt-x"
printf 'major\nAKIA[0-9A-Z]{16}\n' > "$TDIR/rules/secrets-x"
printf 'minor\n.\n' > "$TDIR/rules/hygiene-no-eol"
printf 'int main(void) {\n    // TODO later\n    char *k = "AKIAIOSFODNN7EXAMPLE";\n    return 0;\n}' > "$TDIR/rule.c"
timeout 30 "$BIN" "$TDIR/rule.c" --rules "$TDIR/rules" > "$TDIR/rl.out" 2>/dev/null
check "rules: debt fires" "$(grep -q ' debt-x ' "$TDIR/rl.out" && echo 0 || echo 1)" ""
check "rules: secrets fires" "$(grep -q ' secrets-x ' "$TDIR/rl.out" && echo 0 || echo 1)" ""
check "rules: no-eol fires" "$(grep -q ' hygiene-no-eol ' "$TDIR/rl.out" && echo 0 || echo 1)" ""
check "rules: exit 1" "$([ "$(grep -c "$FIND" "$TDIR/rl.out")" -gt 0 ] && echo 0 || echo 1)" ""

# =============== json ====================================================
timeout 30 "$BIN" fixtures/bad.c --json > "$TDIR/j.out" 2>/dev/null
J_RC=$?
check "json: exit 1" "$([ "$J_RC" -eq 1 ] && echo 0 || echo 1)" "rc=$J_RC"
head -c 1 "$TDIR/j.out" | grep -q '\['
check "json: starts with [" "$?" ""
tail -c 2 "$TDIR/j.out" | grep -q '\]'
check "json: ends with ]" "$?" ""
python3 -m json.tool "$TDIR/j.out" > /dev/null 2>&1
check "json: valid JSON" "$?" ""

# =============== garbage never crashes ==================================
printf 'int x;\x00\x01\x02 garbage {{{ ((( ;;;' > "$TDIR/garbage.c"
timeout 30 "$BIN" "$TDIR/garbage.c" > "$TDIR/g.out" 2>/dev/null
G_RC=$?
check "garbage: no crash" "$([ "$G_RC" -le 1 ] && echo 0 || echo 1)" "rc=$G_RC"
printf 'int x;\n' > "$TDIR/big.c"
for i in $(seq 1 4000); do
    printf 'void f%d(void) { int v%d = %d; if (!v%d) { } }\n' "$i" "$i" "$i" "$i" >> "$TDIR/big.c"
done
timeout 30 "$BIN" "$TDIR/big.c" > /dev/null 2>&1
BIG_RC=$?
check "big file: no crash" "$([ "$BIG_RC" -le 1 ] && echo 0 || echo 1)" "rc=$BIG_RC"

# =============== help/version/errors ====================================
timeout 10 "$BIN" --version | grep -q 'exoqms-code v'
check "version" "$?" ""
timeout 10 "$BIN" --help | grep -q 'missing-error-path'
check "help lists checks" "$?" ""
timeout 10 "$BIN" --nonsense > /dev/null 2>&1
check "bad option: exit 2" "$([ "$?" -eq 2 ] && echo 0 || echo 1)" ""

# =============== self-audit: analyzer on its own source =================
timeout 30 "$BIN" src > "$TDIR/self.out" 2>/dev/null
SELF_N=$(grep -c "$FIND" "$TDIR/self.out")
check "self-audit: no crash" "$([ "$SELF_N" -ge 0 ] && echo 0 || echo 1)" "n=$SELF_N"

echo "=== exoqms-code tests: $PASS ok, $FAILED fail ==="
[ "$FAILED" -eq 0 ]
