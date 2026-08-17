#!/usr/bin/env bash
# exoqms-svg test suite: fixture-based, own temp dir.
# Asserts: each fixture fires exactly its pinned check-ids (counts and
# severities), tree-good is clean (0 findings, exact summary, exit 0),
# --json is valid JSON with the right fields, --shape auto skips
# house.svg with a 'skip' line and exit 0, --shape tree on house fires
# the tree rules (3 pinned findings), data-shape attribute detection,
# transforms are applied, out-of-bounds / fragmented / empty-shape
# fire, garbage input never crashes, exit codes 0/1/2, directory mode
# with per-file headers and the exact summary.
set -u
cd "$(dirname "$0")/.."
BIN=build/exoqms-svg
if ! timeout 60 make -s exoqms-svg; then
    echo "build failed" >&2
    exit 1
fi

TDIR=$(mktemp -d /tmp/opencode/b2s-XXXXXX)
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

# =============== run 1: good fixture = clean ==============================
timeout 30 "$BIN" fixtures/tree-good.svg > "$TDIR/good.out" 2>/dev/null
GOOD_RC=$?
GOOD_N=$(grep -c "$FIND" "$TDIR/good.out")
check "good: 0 findings" "$([ "$GOOD_N" -eq 0 ] && echo 0 || echo 1)" "n=$GOOD_N"
check "good: summary line exact" \
    "$(grep -qx '=== findings: 0 (0 major) ===' "$TDIR/good.out" && echo 0 || echo 1)" \
    "$(grep '===' "$TDIR/good.out" 2>/dev/null)"
check "good: exit code 0" "$([ "$GOOD_RC" -eq 0 ] && echo 0 || echo 1)" "rc=$GOOD_RC"

# =============== run 2: each bad fixture fires its pinned checks ==========
timeout 30 "$BIN" fixtures/tree-stick.svg > "$TDIR/stick.out" 2>/dev/null
STICK_RC=$?
check "stick: empty-shape major" \
    "$(grep -q '^major empty-shape ' "$TDIR/stick.out" && echo 0 || echo 1)" ""
check "stick: stem-missing major" \
    "$(grep -q '^major stem-missing ' "$TDIR/stick.out" && echo 0 || echo 1)" ""
check "stick: 2 findings (2 major)" \
    "$([ "$(grep -c "$FIND" "$TDIR/stick.out")" -eq 2 ] && [ "$(grep -c '^major ' "$TDIR/stick.out")" -eq 2 ] && echo 0 || echo 1)" ""
check "stick: exit code 1" "$([ "$STICK_RC" -eq 1 ] && echo 0 || echo 1)" "rc=$STICK_RC"

timeout 30 "$BIN" fixtures/tree-square-crown.svg > "$TDIR/sq.out" 2>/dev/null
check "square-crown: crown-roundness major" \
    "$(grep -q '^major crown-roundness ' "$TDIR/sq.out" && echo 0 || echo 1)" ""
check "square-crown: exactly 1 finding" \
    "$([ "$(grep -c "$FIND" "$TDIR/sq.out")" -eq 1 ] && echo 0 || echo 1)" \
    "$(grep "$FIND" "$TDIR/sq.out")"

timeout 30 "$BIN" fixtures/tree-box-stem.svg > "$TDIR/box.out" 2>/dev/null
check "box-stem: stem-taper major (parallel-sided)" \
    "$(grep -q '^major stem-taper .*parallel-sided' "$TDIR/box.out" && echo 0 || echo 1)" ""
check "box-stem: exactly 1 finding" \
    "$([ "$(grep -c "$FIND" "$TDIR/box.out")" -eq 1 ] && echo 0 || echo 1)" \
    "$(grep "$FIND" "$TDIR/box.out")"

timeout 30 "$BIN" fixtures/tree-too-lean.svg > "$TDIR/lean.out" 2>/dev/null
check "too-lean: proportions minor" \
    "$(grep -q '^minor proportions ' "$TDIR/lean.out" && echo 0 || echo 1)" ""
check "too-lean: exactly 1 finding" \
    "$([ "$(grep -c "$FIND" "$TDIR/lean.out")" -eq 1 ] && echo 0 || echo 1)" \
    "$(grep "$FIND" "$TDIR/lean.out")"

timeout 30 "$BIN" fixtures/tree-asym.svg > "$TDIR/asym.out" 2>/dev/null
check "asym: symmetry minor" \
    "$(grep -q '^minor symmetry ' "$TDIR/asym.out" && echo 0 || echo 1)" ""
check "asym: exactly 1 finding" \
    "$([ "$(grep -c "$FIND" "$TDIR/asym.out")" -eq 1 ] && echo 0 || echo 1)" \
    "$(grep "$FIND" "$TDIR/asym.out")"

# =============== run 3: house.svg — auto skip vs --shape tree ============
timeout 30 "$BIN" fixtures/house.svg > "$TDIR/house.out" 2>/dev/null
HOUSE_RC=$?
check "house auto: skip line" \
    "$(grep -q '^skip unknown-shape fixtures/house.svg' "$TDIR/house.out" && echo 0 || echo 1)" ""
check "house auto: 0 findings" \
    "$([ "$(grep -c "$FIND" "$TDIR/house.out")" -eq 0 ] && echo 0 || echo 1)" ""
check "house auto: exit code 0" "$([ "$HOUSE_RC" -eq 0 ] && echo 0 || echo 1)" "rc=$HOUSE_RC"

timeout 30 "$BIN" fixtures/house.svg --shape tree > "$TDIR/houseT.out" 2>/dev/null
HOUSE_T_RC=$?
check "house --shape tree: stem-taper fires" \
    "$(grep -q 'stem-taper' "$TDIR/houseT.out" && echo 0 || echo 1)" ""
check "house --shape tree: crown-roundness fires" \
    "$(grep -q 'crown-roundness' "$TDIR/houseT.out" && echo 0 || echo 1)" ""
check "house --shape tree: proportions fires" \
    "$(grep -q 'proportions' "$TDIR/houseT.out" && echo 0 || echo 1)" ""
check "house --shape tree: 3 findings (2 major, 1 minor)" \
    "$([ "$(grep -c "$FIND" "$TDIR/houseT.out")" -eq 3 ] && [ "$(grep -c '^major ' "$TDIR/houseT.out")" -eq 2 ] && [ "$(grep -c '^minor ' "$TDIR/houseT.out")" -eq 1 ] && echo 0 || echo 1)" \
    "$(grep "$FIND" "$TDIR/houseT.out")"
check "house --shape tree: exit code 1" \
    "$([ "$HOUSE_T_RC" -eq 1 ] && echo 0 || echo 1)" "rc=$HOUSE_T_RC"

# =============== run 4: --json ===========================================
timeout 30 "$BIN" fixtures/tree-asym.svg --json > "$TDIR/asym.json" 2>/dev/null
check "json: parses" "$(python3 -m json.tool < "$TDIR/asym.json" >/dev/null 2>&1 && echo 0 || echo 1)" ""
check "json: 1 finding, check=symmetry severity=minor shape=tree" \
    "$(python3 -c "
import json
d = json.load(open('$TDIR/asym.json'))
ok = len(d) == 1 and d[0]['check'] == 'symmetry' and d[0]['severity'] == 'minor' \
     and d[0]['shape'] == 'tree' and d[0]['file'].endswith('tree-asym.svg')
print(0 if ok else 1)
" 2>/dev/null)" ""
timeout 30 "$BIN" fixtures/tree-good.svg --json > "$TDIR/good.json" 2>/dev/null
check "json: good = empty array" \
    "$(grep -q '^\[\]$' "$TDIR/good.json" && echo 0 || echo 1)" "$(cat "$TDIR/good.json")"

# =============== run 5: detection (data-shape, filename, unknown) ========
sed 's/<svg /<svg data-shape="tree" /' fixtures/tree-good.svg \
    > "$TDIR/whatever.svg"
timeout 30 "$BIN" "$TDIR/whatever.svg" > "$TDIR/ds.out" 2>/dev/null
check "detect: data-shape attr on root svg (name has no tree) -> tree checks" \
    "$([ "$(grep -c "$FIND" "$TDIR/ds.out")" -eq 0 ] && echo 0 || echo 1)" \
    "$(grep "$FIND" "$TDIR/ds.out")"
timeout 30 "$BIN" "$TDIR/whatever.svg" --shape auto > "$TDIR/ds2.out" 2>/dev/null
check "detect: auto + data-shape -> 0 findings, no skip line" \
    "$([ "$(grep -c '^skip ' "$TDIR/ds2.out")" -eq 0 ] && echo 0 || echo 1)" ""
sed 's/data-shape="tree" //' fixtures/tree-good.svg > "$TDIR/noattr.svg"
timeout 30 "$BIN" "$TDIR/noattr.svg" --shape auto > "$TDIR/na.out" 2>/dev/null
check "detect: no hint -> skip line + exit 0" \
    "$(grep -q '^skip unknown-shape' "$TDIR/na.out" && echo 0 || echo 1)" ""
check "detect: unknown data-shape hint -> skip" \
    "$(sed 's/<svg /<svg data-shape="robot" /' fixtures/tree-good.svg > "$TDIR/rob.svg" && \
      timeout 30 "$BIN" "$TDIR/rob.svg" 2>/dev/null | grep -q '^skip unknown-shape' && echo 0 || echo 1)" ""

# =============== run 6: transforms =======================================
sed 's/<title>/<g transform="translate(20,30)"><title>/' fixtures/tree-good.svg \
    | sed 's|</svg>|</g></svg>|' > "$TDIR/trans.svg"
timeout 30 "$BIN" "$TDIR/trans.svg" --shape tree > "$TDIR/trans.out" 2>/dev/null
check "transform: translate(20,30) on whole tree -> still 0 findings" \
    "$([ "$(grep -c "$FIND" "$TDIR/trans.out")" -eq 0 ] && echo 0 || echo 1)" \
    "$(grep "$FIND" "$TDIR/trans.out")"
sed 's/<title>/<g transform="scale(0.5,0.5)"><title>/' fixtures/tree-good.svg \
    | sed 's|</svg>|</g></svg>|' > "$TDIR/sc.svg"
timeout 30 "$BIN" "$TDIR/sc.svg" --shape tree > "$TDIR/sc.out" 2>/dev/null
check "transform: scale(0.5) -> ratios unchanged, still 0 findings" \
    "$([ "$(grep -c "$FIND" "$TDIR/sc.out")" -eq 0 ] && echo 0 || echo 1)" \
    "$(grep "$FIND" "$TDIR/sc.out")"
printf '<svg viewBox="0 0 300 400"><g transform="translate(20,30) rotate(90)"><circle cx="150" cy="110" r="85"/><path d="M 144 195 L 156 195 L 166 290 L 134 290 Z"/></g></svg>' \
    > "$TDIR/rot.svg"
timeout 30 "$BIN" "$TDIR/rot.svg" --shape tree > "$TDIR/rot.out" 2>/dev/null
check "transform: translate+rotate applied without crash" \
    "$([ $? -lt 128 ] && echo 0 || echo 1)" ""

# =============== run 7: strange-geometry extras ===========================
printf '<svg viewBox="0 0 300 400"><circle cx="150" cy="110" r="85"/><circle cx="400" cy="50" r="10"/><path d="M 144 195 L 156 195 L 166 290 L 134 290 Z"/></svg>' \
    > "$TDIR/oob.svg"
timeout 30 "$BIN" "$TDIR/oob.svg" --shape tree > "$TDIR/oob.out" 2>/dev/null
check "out-of-bounds: element outside viewBox fires minor" \
    "$(grep -q '^minor out-of-bounds ' "$TDIR/oob.out" && echo 0 || echo 1)" \
    "$(grep "$FIND" "$TDIR/oob.out")"
{
    printf '<svg viewBox="0 0 400 400"><circle cx="150" cy="100" r="70"/><path d="M 140 170 L 160 170 L 170 270 L 130 270 Z"/>'
    for i in $(seq 1 10); do
        printf '<line x1="%d" y1="%d" x2="%d" y2="%d"/>' $((10 + i * 20)) 20 $((30 + i * 20)) 40
    done
    printf '</svg>'
} > "$TDIR/frag.svg"
timeout 30 "$BIN" "$TDIR/frag.svg" --shape tree > "$TDIR/frag.out" 2>/dev/null
check "fragmented: 11 disconnected strokes > 8 -> minor" \
    "$(grep -q '^minor fragmented ' "$TDIR/frag.out" && echo 0 || echo 1)" \
    "$(grep "$FIND" "$TDIR/frag.out")"
printf '<svg viewBox="0 0 300 400"></svg>' > "$TDIR/empty.svg"
timeout 30 "$BIN" "$TDIR/empty.svg" --shape tree > "$TDIR/empty.out" 2>/dev/null
check "empty-shape: svg with no elements fires major" \
    "$(grep -q '^major empty-shape ' "$TDIR/empty.out" && echo 0 || echo 1)" \
    "$(grep "$FIND" "$TDIR/empty.out")"

# =============== run 8: garbage = no crash ================================
printf '<svg><path d="garbage"/><circle r="-5"/><rect x="a"/></svg>\x00\xff garbage' \
    > "$TDIR/nul.svg"
timeout 30 "$BIN" "$TDIR/nul.svg" > "$TDIR/nul.out" 2>/dev/null
NUL_RC=$?
check "garbage: NUL bytes + garbage d + no crash" \
    "$([ "$NUL_RC" -lt 128 ] && echo 0 || echo 1)" "rc=$NUL_RC"
check "garbage: summary line present" \
    "$(grep -q '^=== findings: .* ===$' "$TDIR/nul.out" && echo 0 || echo 1)" ""
printf '<svg viewBox="0 0 10 10"><path d="M 5 5"><circle' > "$TDIR/unclosed.svg"
timeout 30 "$BIN" "$TDIR/unclosed.svg" > "$TDIR/uncl.out" 2>/dev/null
check "garbage: unclosed tags no crash" \
    "$([ $? -lt 128 ] && echo 0 || echo 1)" ""
head -c 10000000 /dev/zero | tr '\0' 'a' > "$TDIR/big.svg"
timeout 30 "$BIN" "$TDIR/big.svg" > "$TDIR/big.out" 2>/dev/null
check "garbage: 10MB single line no crash" \
    "$([ $? -lt 128 ] && echo 0 || echo 1)" ""
check "garbage: 10MB summary line present" \
    "$(grep -q '^=== findings: .* ===$' "$TDIR/big.out" && echo 0 || echo 1)" ""
printf '<svg viewBox="0 0 100 100"><path d="M 10 10 L 90 10 L 90 90 L 10 90 Z"/><circle/></svg>' \
    > "$TDIR/nod.svg"
timeout 30 "$BIN" "$TDIR/nod.svg" > "$TDIR/nod.out" 2>/dev/null
check "garbage: circle without r skipped, no crash" \
    "$([ $? -lt 128 ] && echo 0 || echo 1)" ""

# =============== run 9: exit codes ========================================
timeout 30 "$BIN" "$TDIR/missing.svg" > /dev/null 2>&1
check "missing target: exit 2" "$([ $? -eq 2 ] && echo 0 || echo 1)" ""
timeout 30 "$BIN" --bogus 2>/dev/null
check "unknown option: exit 2" "$([ $? -eq 2 ] && echo 0 || echo 1)" ""
timeout 30 "$BIN" --shape robot fixtures/tree-good.svg 2>/dev/null
check "unknown shape: exit 2" "$([ $? -eq 2 ] && echo 0 || echo 1)" ""
timeout 30 "$BIN" --version | grep -q "exoqms-svg 0.4.0-alpha.1"
check "--version" "$([ $? -eq 0 ] && echo 0 || echo 1)" ""
timeout 30 "$BIN" --help | grep -q "usage:"
check "--help" "$([ $? -eq 0 ] && echo 0 || echo 1)" ""

# =============== operation form (/check?k=v) ============================
timeout 30 "$BIN" "/check?file=fixtures/tree-good.svg" > "$TDIR/op.out" 2>/dev/null
check "op /check good: exit 0" "$([ "$?" -eq 0 ] && echo 0 || echo 1)" ""
check "op /check good: 0 findings" \
    "$([ "$(grep -c "$FIND" "$TDIR/op.out")" -eq 0 ] && echo 0 || echo 1)" \
    "$(grep "$FIND" "$TDIR/op.out")"
timeout 30 "$BIN" "/check?file=fixtures/house.svg&shape=tree" > "$TDIR/oph.out" 2>/dev/null
OP_RC=$?
check "op /check shape=tree: 3 findings (2 major, 1 minor)" \
    "$([ "$(grep -c "$FIND" "$TDIR/oph.out")" -eq 3 ] && [ "$(grep -c '^major ' "$TDIR/oph.out")" -eq 2 ] && [ "$(grep -c '^minor ' "$TDIR/oph.out")" -eq 1 ] && echo 0 || echo 1)" \
    "$(grep "$FIND" "$TDIR/oph.out")"
check "op /check shape=tree: exit 1" "$([ "$OP_RC" -eq 1 ] && echo 0 || echo 1)" "rc=$OP_RC"
timeout 30 "$BIN" "/check?file=fixtures/tree-asym.svg&json=1" > "$TDIR/opj.json" 2>/dev/null
check "op /check json: parses" "$(python3 -m json.tool < "$TDIR/opj.json" >/dev/null 2>&1 && echo 0 || echo 1)" ""
timeout 30 "$BIN" "/check?nope=1" > /dev/null 2>&1
check "op /check unknown param: exit 2" "$([ "$?" -eq 2 ] && echo 0 || echo 1)" ""
timeout 30 "$BIN" "/bogus?x=1" > /dev/null 2>&1
check "op unknown: exit 2" "$([ "$?" -eq 2 ] && echo 0 || echo 1)" ""

# =============== run 10: directory mode ==================================
timeout 30 "$BIN" fixtures > "$TDIR/dir.out" 2>/dev/null
DIR_RC=$?
DIR_N=$(grep -c "$FIND" "$TDIR/dir.out")
check "dir: 6 findings (4 major, 2 minor)" \
    "$([ "$DIR_N" -eq 6 ] && [ "$(grep -c '^major ' "$TDIR/dir.out")" -eq 4 ] && [ "$(grep -c '^minor ' "$TDIR/dir.out")" -eq 2 ] && echo 0 || echo 1)" \
    "n=$DIR_N"
check "dir: per-file header lines" \
    "$(grep -q 'file: .*tree-good.svg' "$TDIR/dir.out" && grep -q 'file: .*tree-stick.svg' "$TDIR/dir.out" && grep -q 'file: .*house.svg' "$TDIR/dir.out" && echo 0 || echo 1)" ""
check "dir: house skip line present" \
    "$(grep -q '^skip unknown-shape .*house.svg' "$TDIR/dir.out" && echo 0 || echo 1)" ""
check "dir: summary exact" \
    "$(grep -qx '=== findings: 6 (4 major) ===' "$TDIR/dir.out" && echo 0 || echo 1)" \
    "$(grep '===' "$TDIR/dir.out")"
check "dir: exit code 1" "$([ "$DIR_RC" -eq 1 ] && echo 0 || echo 1)" "rc=$DIR_RC"
timeout 30 "$BIN" fixtures --json > "$TDIR/dir.json" 2>/dev/null
check "dir json: parses" "$(python3 -m json.tool < "$TDIR/dir.json" >/dev/null 2>&1 && echo 0 || echo 1)" ""
check "dir json: 6 items, all checks pinned" \
    "$(python3 -c "
import json
d = json.load(open('$TDIR/dir.json'))
ids = sorted(x['check'] for x in d)
want = sorted(['empty-shape','stem-missing','crown-roundness','stem-taper','proportions','symmetry'])
print(0 if len(d) == 6 and ids == want and all('file' in x and 'shape' in x and 'severity' in x and 'reason' in x for x in d) else 1)
" 2>/dev/null)" ""

# =============== summary ==================================================
SECS=$SECONDS
echo "=== exoqms-svg tests: $PASS ok, $FAILED fail (${SECS}s) ==="
[ "$FAILED" -eq 0 ]
