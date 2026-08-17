#!/usr/bin/env bash
# exokit test suite: hermetic. Builds the fixture calculator, scaffolds a
# kit, extracts the inventory, then exercises verify (incl. drift
# detection), audit (completeness + fidelity findings), and diff.
set -u
cd "$(dirname "$0")/.."

BIN=$PWD/build/exokit
FIX=$PWD/test/fixtures
WORK=$(mktemp -d /tmp/opencode/exokit-XXXXXX)
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
check() { # name, result, detail
    if [ "$2" = "0" ]; then
        PASS=$((PASS + 1))
        printf 'PASS  %-46s\n' "$1"
    else
        FAIL=$((FAIL + 1))
        printf 'FAIL  %-46s [%s]\n' "$1" "${3:-}"
    fi
}

r=$("$BIN" --version)
check "version" "$([ "$r" = "exokit v0.4.0-alpha.1" ] && echo 0 || echo 1)" "$r"
r=$("$BIN" bogus 2>&1 | head -1)
check "unknown command exit 2" "$([ "$r" = "exokit: unknown command bogus" ] && echo 0 || echo 1)" "$r"

say() { printf '%s\n' "$*"; }
say ""
say "=== init ==="
r=$("$BIN" init "$WORK/proj")
check "init reports success" "$(printf '%s' "$r" | grep -q 'kit initialized' && echo 0 || echo 1)" "$r"
for f in config contract.tsv examples.tsv Makefile README.md; do
    check "init creates $f" "$([ -f "$WORK/proj/kit/$f" ] && echo 0 || echo 1)"
done
check "init creates runner dirs" "$([ -d "$WORK/proj/kit/runners/c" ] && [ -d "$WORK/proj/kit/runners/js" ] && [ -d "$WORK/proj/kit/runners/rust" ] && [ -d "$WORK/proj/kit/runners/python" ] && echo 0 || echo 1)"
check "init config has a runner line" "$(grep -q '^runner' "$WORK/proj/kit/config" && echo 0 || echo 1)"
check "init config has max_examples" "$(grep -q '^max_examples' "$WORK/proj/kit/config" && echo 0 || echo 1)"

say ""
say "=== extract ==="
cp -r "$FIX/calc.c" "$WORK/proj/"
r=$("$BIN" extract "$WORK/proj" --out "$WORK/proj/kit/contract.tsv")
check "extract reports count" "$(printf '%s' "$r" | grep -q 'extracted 4 function' && echo 0 || echo 1)" "$r"
check "extract finds add" "$(grep -q '^add	' "$WORK/proj/kit/contract.tsv" && echo 0 || echo 1)"
check "extract finds static sub" "$(grep -q '^sub	' "$WORK/proj/kit/contract.tsv" && echo 0 || echo 1)"
check "extract finds div_safe" "$(grep -q '^div_safe	' "$WORK/proj/kit/contract.tsv" && echo 0 || echo 1)"
check "extract records sigs" "$(grep -q 'int add(int a, int b)' "$WORK/proj/kit/contract.tsv" && echo 0 || echo 1)"
r=$("$BIN" extract /nonexistent --out "$WORK/na.tsv" 2>&1)
check "extract bad dir tolerated" "$(printf '%s' "$r" | grep -q 'extracted 0' && echo 0 || echo 1)" "$r"

say ""
say "=== verify: conforming runner ==="
cp "$FIX/run.c" "$WORK/proj/kit/runners/c/run.c"
( cd "$WORK/proj/kit/runners/c" && cc -O2 -o run run.c )
cat > "$WORK/proj/kit/examples.tsv" <<'EOF'
# fn	args	expected	desc	err
add	2 3	5	positive ints	0
add	-1 1	0	negative mix	0
div_safe	7 2	3	integer division	0
div_safe	5 0	error: division by zero	zero divisor	1
calc_name		calc v1	no args	0
sub	5 3	2	internal helper	0
EOF
r=$(cd "$WORK/proj" && "$BIN" verify)
check "verify all pass" "$([ "$(printf '%s' "$r" | grep -c '^pass')" = 6 ] && echo 0 || echo 1)" "$r"
check "verify summary ok" "$(printf '%s' "$r" | grep -q '6 ok, 0 fail' && echo 0 || echo 1)" "$r"
check "verify exit 0" "$(cd "$WORK/proj" && "$BIN" verify >/dev/null 2>&1; echo $?)" "exit=$?"

say ""
say "=== verify: drift detection ==="
cp "$FIX/run.c" "$WORK/proj/kit/runners/c/run_drift.c"
cat > "$WORK/proj/kit/runners/c/run.c" <<'EOF'
#include <stdio.h>
#include <string.h>
static int add(int a, int b) { return a + b; }
static int div_safe(int a, int b) { return b == 0 ? -999 : a / b; }
static int sub(int a, int b) { return a - b; }
static const char *calc_name(void) { return "calc v1"; }
int main(void)
{
    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char *nl = strchr(tab + 1, '\n');
        if (nl) *nl = 0;
        const char *fn = line, *args = tab + 1;
        int a = 0, b = 0;
        if (!strcmp(fn, "add")) {
            sscanf(args, "%d %d", &a, &b);
            printf("%d\n", add(a, b));
        } else if (!strcmp(fn, "div_safe")) {
            sscanf(args, "%d %d", &a, &b);
            printf("%d\n", div_safe(a, b));
        } else if (!strcmp(fn, "sub")) {
            sscanf(args, "%d %d", &a, &b);
            printf("%d\n", sub(a, b));
        } else if (!strcmp(fn, "calc_name")) {
            printf("%s\n", calc_name());
        } else {
            printf("error: no such fn %s\n", fn);
        }
        fflush(stdout);
    }
    return 0;
}
EOF
( cd "$WORK/proj/kit/runners/c" && cc -O2 -o run run.c )
r=$(cd "$WORK/proj" && "$BIN" verify; echo "exit=$?")
check "drift: div_safe fails" "$(printf '%s' "$r" | grep -q 'fail div_safe' && echo 0 || echo 1)" "$r"
check "drift: other fns still pass" "$(printf '%s' "$r" | grep -c '^pass' | grep -q '^5$' && echo 0 || echo 1)" "$r"
check "drift: exit nonzero" "$(printf '%s' "$r" | grep -q 'exit=1' && echo 0 || echo 1)" "$r"
# restore the conforming runner
cp "$FIX/run.c" "$WORK/proj/kit/runners/c/run.c"
( cd "$WORK/proj/kit/runners/c" && cc -O2 -o run run.c )

say ""
say "=== verify: --fn filter and --runner ==="
r=$(cd "$WORK/proj" && "$BIN" verify --fn add)
check "filter runs only add" "$(printf '%s' "$r" | grep -c '^pass' | grep -q '^2$' && echo 0 || echo 1)" "$r"
r=$(cd "$WORK/proj" && "$BIN" verify --runner ./kit/runners/c/run)
check "explicit runner works" "$(printf '%s' "$r" | grep -q '6 ok, 0 fail' && echo 0 || echo 1)" "$r"

say ""
say "=== audit: clean kit ==="
r=$(cd "$WORK/proj" && "$BIN" audit)
check "clean audit passes" "$([ "$r" = "pass" ] && echo 0 || echo 1)" "$r"
check "clean audit exit 0" "$(cd "$WORK/proj" && "$BIN" audit >/dev/null 2>&1; echo $?)" "exit=$?"

say ""
say "=== audit: completeness (R1/R2) ==="
printf 'extra_fn\tsig\t1\t\t\n' >> "$WORK/proj/kit/contract.tsv"
r=$(cd "$WORK/proj" && "$BIN" audit; echo "exit=$?")
check "untracked fn is a finding" "$(printf '%s' "$r" | grep -q 'missing-example' && echo 0 || echo 1)" "$r"
check "finding is major" "$(printf '%s' "$r" | grep -q '"severity":"major"' && echo 0 || echo 1)" "$r"
check "finding names the fn" "$(printf '%s' "$r" | grep -q 'extra_fn' && echo 0 || echo 1)" "$r"
check "audit exit 1 with findings" "$(printf '%s' "$r" | grep -q 'exit=1' && echo 0 || echo 1)" "$r"
# remove the offending line
grep -v '^extra_fn' "$WORK/proj/kit/contract.tsv" > "$WORK/proj/kit/contract.tsv.tmp" && mv "$WORK/proj/kit/contract.tsv.tmp" "$WORK/proj/kit/contract.tsv"

say ""
say "=== audit: fidelity (R6) ==="
cp "$FIX/run.c" "$WORK/proj/kit/runners/c/run_bad.c"
printf 'add\t0 0\t1\tintentional drift\t0\n' >> "$WORK/proj/kit/examples.tsv"
r=$(cd "$WORK/proj" && "$BIN" audit; echo "exit=$?")
check "failing example is a finding" "$(printf '%s' "$r" | grep -q 'example-fail' && echo 0 || echo 1)" "$r"
check "finding quotes the drift" "$(printf '%s' "$r" | grep -q "expected '1' got '0'" && echo 0 || echo 1)" "$r"
check "audit exit 1 on drift" "$(printf '%s' "$r" | grep -q 'exit=1' && echo 0 || echo 1)" "$r"
grep -v 'intentional drift' "$WORK/proj/kit/examples.tsv" > "$WORK/proj/kit/examples.tsv.tmp" && mv "$WORK/proj/kit/examples.tsv.tmp" "$WORK/proj/kit/examples.tsv"

say ""
say "=== diff ==="
cat > "$WORK/a.tsv" <<'EOF'
alpha	sig A
beta	sig B
EOF
cat > "$WORK/b.tsv" <<'EOF'
alpha	sig A
EOF
r=$("$BIN" diff "$WORK/a.tsv" "$WORK/b.tsv"; echo "exit=$?")
check "diff reports missing" "$(printf '%s' "$r" | grep -q 'missing beta' && echo 0 || echo 1)" "$r"
check "diff exit 1 on missing" "$(printf '%s' "$r" | grep -q 'exit=1' && echo 0 || echo 1)" "$r"
cat > "$WORK/c.tsv" <<'EOF'
alpha	sig A
gamma	sig G
EOF
r=$("$BIN" diff "$WORK/b.tsv" "$WORK/c.tsv"; echo "exit=$?")
check "diff warns extra" "$(printf '%s' "$r" | grep -q 'extra gamma' && echo 0 || echo 1)" "$r"
check "diff exit 0 on extra (non-exact)" "$(printf '%s' "$r" | grep -q 'exit=0' && echo 0 || echo 1)" "$r"
r=$("$BIN" diff "$WORK/b.tsv" "$WORK/c.tsv" --exact; echo "exit=$?")
check "diff --exact rejects extra" "$(printf '%s' "$r" | grep -q 'exit=1' && echo 0 || echo 1)" "$r"

say ""
say "=== console operation form (/op?k=v) ==="
r=$("$BIN" "/init?dir=$WORK/opkit")
check "op /init scaffolds a kit" "$([ -f "$WORK/opkit/kit/config" ] && echo 0 || echo 1)" "$r"
r=$("$BIN" "/extract?src=$WORK/proj&out=$WORK/op.tsv")
check "op /extract writes inventory" "$(printf '%s' "$r" | grep -q 'extracted 4 function' && echo 0 || echo 1)" "$r"
r=$(cd "$WORK/proj" && "$BIN" "/verify?kit=kit")
check "op /verify ledger all pass" "$(printf '%s' "$r" | grep -q '6 ok, 0 fail' && echo 0 || echo 1)" "$r"
check "op /verify exit 0" "$(cd "$WORK/proj" && "$BIN" "/verify?kit=kit" >/dev/null 2>&1; echo $?)" ""
r=$(cd "$WORK/proj" && "$BIN" "/audit?kit=kit")
check "op /audit clean kit passes" "$([ "$r" = "pass" ] && echo 0 || echo 1)" "$r"
check "op /audit exit 0" "$(cd "$WORK/proj" && "$BIN" "/audit?kit=kit" >/dev/null 2>&1; echo $?)" ""
r=$("$BIN" "/diff?a=$WORK/a.tsv&b=$WORK/b.tsv"; echo "exit=$?")
check "op /diff reports missing" "$(printf '%s' "$r" | grep -q 'missing beta' && echo 0 || echo 1)" "$r"
check "op /diff exit 1 on missing" "$(printf '%s' "$r" | grep -q 'exit=1' && echo 0 || echo 1)" "$r"
r=$("$BIN" "/diff?a=$WORK/b.tsv&b=$WORK/c.tsv&exact=1"; echo "exit=$?")
check "op /diff exact rejects extra" "$(printf '%s' "$r" | grep -q 'exit=1' && echo 0 || echo 1)" "$r"
check "op unknown exits 2" "$([ "$("$BIN" "/bogus?x=1" >/dev/null 2>&1; echo $?)" = 2 ] && echo 0 || echo 1)" ""
check "op unknown param exits 2" "$([ "$("$BIN" "/verify?nope=1" >/dev/null 2>&1; echo $?)" = 2 ] && echo 0 || echo 1)" ""

say ""
say "=== results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
