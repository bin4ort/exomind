#!/usr/bin/env bash
# exoqms test suite: own exomind (7681) + own exoqms instances (7682-7686)
# + own exodoc build + stub exoqms-ui binaries. All fixture state is
# created under a fresh temp dir; every command runs under `timeout`.
set -u
cd "$(dirname "$0")/.." || exit 1

PASS=0; FAIL=0; WARN=0
t() { # name expected actual
    if [ "$2" = "$3" ]; then PASS=$((PASS+1)); printf 'PASS %-54s [%s]\n' "$1" "$3";
    else FAIL=$((FAIL+1)); printf 'FAIL %-54s got [%s] expected [%s]\n' "$1" "$3" "$2"; fi
}
t_contains() { # name expected-substring actual
    if printf '%s' "$3" | grep -qF -- "$2"; then PASS=$((PASS+1)); printf 'PASS %-54s [%s]\n' "$1" "$2";
    else FAIL=$((FAIL+1)); printf 'FAIL %-54s [%s] missing from [%s]\n' "$1" "$2" "$3"; fi
}
t_nc() { # name actual
    if printf '%s' "$2" | grep -q 'error:'; then PASS=$((PASS+1)); printf 'PASS %-54s [%s]\n' "$1" "$2";
    else FAIL=$((FAIL+1)); printf 'FAIL %-54s expected error: got [%s]\n' "$1" "$2"; fi
}

TDIR=$(mktemp -d /tmp/opencode/b1q-test.XXXXXX)
PIDS=""
cleanup() {
    for p in $PIDS; do kill -9 "$p" 2>/dev/null; done
    [ -n "${KEEP:-}" ] || rm -rf "$TDIR"
}
trap cleanup EXIT

EM_PORT=${EM_PORT:-7681}; QMS_A=${QMS_A:-7682}; QMS_B=${QMS_B:-7683}
QMS_C=${QMS_C:-7684}; QMS_D=${QMS_D:-7685}; QMS_E=${QMS_E:-7686}
EM="http://127.0.0.1:$EM_PORT"
BASE=http://127.0.0.1

# ---------- build helpers (exodoc from the repo, exomind from src) --------
make -C "$(pwd)/../exodoc" >/dev/null 2>&1 || make -C ../exodoc >/dev/null 2>&1
EXODOC_BIN="$TDIR/exodoc"
if [ -x ../exodoc/build/exodoc ]; then cp ../exodoc/build/exodoc "$EXODOC_BIN";
elif [ -x "$(pwd)/../exodoc/build/exodoc" ]; then cp "$(pwd)/../exodoc/build/exodoc" "$EXODOC_BIN";
else EXODOC_BIN=exodoc; fi
if ! command -v "$EXODOC_BIN" >/dev/null 2>&1 && [ "$EXODOC_BIN" = exodoc ]; then
    printf 'FAIL %-54s exodoc binary unavailable\n' "setup exodoc"; exit 1
fi
cc -O2 -std=c11 -Wall -Wextra -pthread -D_POSIX_C_SOURCE=200809L \
    ../src/main.c ../src/http.c ../src/store.c ../src/util.c \
    ../src/router.c ../common/exo.c -o "$TDIR/exomind" 2>/dev/null \
 || cc -O2 -std=c11 -Wall -Wextra -pthread -D_POSIX_C_SOURCE=200809L \
    src/main.c src/http.c src/store.c src/util.c src/router.c \
    -o "$TDIR/exomind"

# ---------- fixtures --------------------------------------------------------
mkdir -p "$TDIR/repo/docs" "$TDIR/repo/okc/test" "$TDIR/repo/kit" \
         "$TDIR/badrepo/docs" "$TDIR/badrepo/badcomp"

cat > "$TDIR/repo/docs/stack.tsv" <<EOF
# b1 exoqms fixture manifest: name<TAB>dir<TAB>port<TAB>build_cmd<TAB>test_cmd<TAB>version_flag
okc	okc		make	bash test/test.sh	
EOF
cat > "$TDIR/repo/okc/test/test.sh" <<'EOF'
#!/bin/sh
echo "okc: 17 passed, 0 failed"
exit 0
EOF
chmod +x "$TDIR/repo/okc/test/test.sh"
cat > "$TDIR/repo/okc/standard.md" <<'EOF'
# okc standard v1.0.0 — fixture standard

The fixture component's conformance statement. This file exists so the
docs-coverage check can find a standards reference.
EOF
cat > "$TDIR/repo/okc/README.md" <<EOF
# okc — compliant fixture component

A component that satisfies every check of the exodoc standard.
This is okc v1.2.3.

## Build

make

## Run

./build/okc --port 0

## API

| method | path   | purpose  |
|--------|--------|----------|
| GET    | /      | spec     |
| GET    | /ping  | liveness |

## Internals

State is kept in exomind only.

## Tests

bash test/test.sh

## Limitations

None known.
EOF

cat > "$TDIR/badrepo/docs/stack.tsv" <<EOF
# bad fixture: the README misses the tests and limitations sections
badcomp	badcomp		make	bash test.sh	
EOF
cat > "$TDIR/badrepo/badcomp/README.md" <<EOF
# badcomp — non-compliant fixture component

Missing the tests and limitations sections on purpose. v1.0.0.

## Build

make

## Run

./build/badcomp

## API

| method | path  | purpose  |
|--------|-------|----------|
| GET    | /     | spec     |

## Internals

State lives in exomind.
EOF

cat > "$TDIR/stub-ui" <<'EOF'
#!/bin/sh
if [ -n "${STUB_UI_FLAG:-}" ] && [ -f "$STUB_UI_FLAG" ]; then
    printf '[{"check":"spacing","severity":"warn","selector":".gap","reason":"gap"}]'
    exit 1
fi
printf '[]'
exit 0
EOF
chmod +x "$TDIR/stub-ui"

cat > "$TDIR/stub-code" <<'EOF'
#!/bin/sh
# exoqms-code stub: JSON findings with severity major/minor (flag files)
if [ -n "${STUB_CODE_MAJOR_FLAG:-}" ] && [ -f "$STUB_CODE_MAJOR_FLAG" ]; then
    printf '[{"check":"unchecked-fopen","severity":"major","file":"a.c","line":42,"reason":"fopen result not checked"}]'
    exit 1
fi
if [ -n "${STUB_CODE_MINOR_FLAG:-}" ] && [ -f "$STUB_CODE_MINOR_FLAG" ]; then
    printf '[{"check":"unchecked-pread","severity":"minor","file":"b.c","line":7,"reason":"pread result not checked"}]'
    exit 1
fi
if [ -n "${STUB_CODE_TRACE:-}" ]; then printf '%s\n' "$@" > "$STUB_CODE_TRACE"; fi
printf '[]'
exit 0
EOF
chmod +x "$TDIR/stub-code"

cat > "$TDIR/stub-kit" <<'EOF'
#!/bin/sh
# exokit stub: pass, or a major finding when the flag file exists
if [ -n "${STUB_KIT_MAJOR_FLAG:-}" ] && [ -f "$STUB_KIT_MAJOR_FLAG" ]; then
    printf '[{"check":"example-fail","severity":"major","file":"kit/examples.tsv","line":3,"reason":"add expected 5 got 6"}]'
    exit 1
fi
if [ -n "${STUB_KIT_TRACE:-}" ]; then printf '%s\n' "$@" > "$STUB_KIT_TRACE"; fi
printf 'pass\n'
exit 0
EOF
chmod +x "$TDIR/stub-kit"

cat > "$TDIR/stub-svg" <<'EOF'
#!/bin/sh
# exoqms-svg stub: JSON findings with severity major/minor (flag files)
if [ -n "${STUB_SVG_MAJOR_FLAG:-}" ] && [ -f "$STUB_SVG_MAJOR_FLAG" ]; then
    printf '[{"file":"tree.svg","shape":"tree","severity":"major","check":"stem-missing","reason":"trunk absent"}]'
    exit 1
fi
if [ -n "${STUB_SVG_MINOR_FLAG:-}" ] && [ -f "$STUB_SVG_MINOR_FLAG" ]; then
    printf '[{"file":"tree.svg","shape":"tree","severity":"minor","check":"symmetry","reason":"lean"}]'
    exit 1
fi
if [ -n "${STUB_SVG_TRACE:-}" ]; then printf '%s\n' "$@" > "$STUB_SVG_TRACE"; fi
printf '[]'
exit 0
EOF
chmod +x "$TDIR/stub-svg"

cat > "$TDIR/stub-hang" <<'EOF'
#!/bin/sh
# sleeps 31s: distinct from other swarm pollers' sleep 30
sleep 31
exit 0
EOF
chmod +x "$TDIR/stub-hang"

# ---------- start the test stack --------------------------------------------
"$TDIR/exomind" --port $EM_PORT --data "$TDIR/exomind.dat" \
    > "$TDIR/exomind.log" 2>&1 &
PIDS="$PIDS $!"; EXOMIND_PID=$!
ok=0
for i in $(seq 1 30); do
    if timeout 3 curl -s "$EM/ping" | grep -q pong; then ok=1; break; fi
    sleep 0.5
done
t "test exomind up on $EM_PORT" 1 "$ok"

start_qms() { # name port token extra...
    local name=$1 port=$2 token=$3; shift 3
    local args="--port $port --exomind $EM --exodoc $EXODOC_BIN"
    local envs=""
    for a in "$@"; do
        case "$a" in
            ENV:*) envs="$envs ${a#ENV:}";;
            *) args="$args $a";;
        esac
    done
    [ -n "$token" ] && args="$args --token $token"
    env $envs ./build/exoqms $args > "$TDIR/$name.log" 2>&1 &
    local daemon_pid=$!
    eval "declare -g QMS_${port}_PID=$daemon_pid"
    PIDS="$PIDS $daemon_pid"
    ok=0
    for i in $(seq 1 40); do
        if timeout 3 curl -s -H "Authorization: Bearer $token" \
            "$BASE:$port/ping" | grep -q pong; then ok=1; break; fi
        sleep 0.5
    done
    t "$name up on $port" 1 "$ok"
}

start_qms qmsa $QMS_A "" --repo "$TDIR/repo" --ui "$TDIR/stub-ui" \
    --code "$TDIR/stub-code" --kit "$TDIR/stub-kit" --svg "$TDIR/stub-svg" \
    --agents b1,b2 --notes24h 1 \
    ENV:STUB_UI_FLAG="$TDIR/ui-flag" \
    ENV:STUB_KIT_MAJOR_FLAG="$TDIR/kit-major" \
    ENV:STUB_KIT_TRACE="$TDIR/kit-trace" \
    ENV:STUB_CODE_MAJOR_FLAG="$TDIR/code-major" \
    ENV:STUB_CODE_MINOR_FLAG="$TDIR/code-minor" \
    ENV:STUB_CODE_TRACE="$TDIR/code-trace" \
    ENV:STUB_SVG_MAJOR_FLAG="$TDIR/svg-major" \
    ENV:STUB_SVG_MINOR_FLAG="$TDIR/svg-minor" \
    ENV:STUB_SVG_TRACE="$TDIR/svg-trace"
start_qms qmsb $QMS_B sekrit --repo "$TDIR/repo" --ui "$TDIR/stub-hang" \
    --code "$TDIR/stub-hang" --kit "$TDIR/stub-hang" --svg "$TDIR/stub-hang" \
    --agents b1,b2 --notes24h 1
start_qms qmsc $QMS_C "" --repo "$TDIR/badrepo" --ui "$TDIR/stub-ui" \
    --agents b1,b2 --notes24h 100000

em_set() { # key value
    timeout 5 curl -s "$EM/set?key=$1" -d "{\"key\":\"$1\",\"value\":\"$2\",\"ttl\":0}" | grep -q ok
}
em_set "agent:b1:status" ok
em_set "agent:b2:status" ok

# ============================================================================
t "ping answers pong" "pong" "$(timeout 5 curl -s $BASE:$QMS_A/ping)"
t_contains "GET / is self-describing" "exoqms v0.4" \
    "$(timeout 5 curl -s $BASE:$QMS_A/)"

# ---- objectives (ISO 9001 6.2) ---------------------------------------------
O1=$(timeout 5 curl -s -d $'iter5 tests passing\tmetric:iter5:tests_passing\t300' \
        $BASE:$QMS_A/objectives)
O2=$(timeout 5 curl -s -d $'zero bugs\tmetric:iter5:bugs_found\t0' \
        $BASE:$QMS_A/objectives)
O3=$(timeout 5 curl -s -d $'release mode\tmetric:iter5:mode\trelease\tq2' \
        $BASE:$QMS_A/objectives)
t_contains "objective 1 created" "ok " "$O1"
t_contains "objective 2 created" "ok " "$O2"
t_contains "objective 3 created" "ok " "$O3"
ID1=$(printf '%s' "$O1" | awk '{print $2}')
ID3=$(printf '%s' "$O3" | awk '{print $2}')
OBJLIST=$(timeout 5 curl -s "$BASE:$QMS_A/objectives")
t "objectives list has 3 lines" 3 "$(printf '%s\n' "$OBJLIST" | grep -c .)"
t_contains "objective record carries period" "q2" "$OBJLIST"
OBJJSON=$(timeout 5 curl -s "$BASE:$QMS_A/objectives?json=1")
t "objectives json parses to 3" 3 "$(printf '%s' "$OBJJSON" | python3 -c 'import sys,json;print(len(json.load(sys.stdin)))')"
t_contains "objectives json has title" "iter5 tests passing" "$OBJJSON"
t_nc "objectives garbage body -> 400" \
    "$(timeout 5 curl -s -d "only-one-field" $BASE:$QMS_A/objectives)"

# ---- NC lifecycle (ISO 9001 8.7 / 10.2) ------------------------------------
NC1=$(timeout 5 curl -s -d $'broken build\tmajor\tcompiler warnings in exosched' \
        $BASE:$QMS_A/nc)
NC2=$(timeout 5 curl -s -d $'stale docs\tminor\texodoc found 2 gaps' \
        $BASE:$QMS_A/nc)
NC3=$(timeout 5 curl -s -d $'test flake\tminor\tintermittent failure' \
        $BASE:$QMS_A/nc)
A1=$(printf '%s' "$NC1" | awk '{print $2}')
A2=$(printf '%s' "$NC2" | awk '{print $2}')
A3=$(printf '%s' "$NC3" | awk '{print $2}')
t_contains "nc1 created ok" "ok $A1" "$NC1"
t_contains "nc1 starts open" "open" "$(timeout 5 curl -s "$BASE:$QMS_A/nc?id=$A1")"
t "nc severity invalid -> error" 1 \
    "$(timeout 5 curl -s -d $'x\tfatal\tboom' $BASE:$QMS_A/nc | grep -c 'error: severity must be major or minor')"

S1=$(timeout 5 curl -s -X POST "$BASE:$QMS_A/nc?id=$A1&action=analyse")
t_contains "open -> analysis" "ok $A1 analysis" "$S1"
S2=$(timeout 5 curl -s -X POST "$BASE:$QMS_A/nc?id=$A1&action=correct")
t_contains "analysis -> corrective" "ok $A1 corrective" "$S2"
S3=$(timeout 5 curl -s -X POST "$BASE:$QMS_A/nc?id=$A1&action=verify")
t_contains "corrective -> verify" "ok $A1 verify" "$S3"
S4=$(timeout 5 curl -s -d "verified in review" "$BASE:$QMS_A/nc?id=$A1&action=close")
t_contains "verify -> closed (note body)" "ok $A1 closed" "$S4"
D1=$(timeout 5 curl -s "$BASE:$QMS_A/nc?id=$A1")
t_contains "nc1 detail closed" "closed" "$D1"
t_contains "nc1 closed_by api" "api" "$D1"

INV=$(timeout 5 curl -s -X POST "$BASE:$QMS_A/nc?id=$A2&action=verify")
t_nc "verify from open rejected" "$INV"
t_contains "rejection names expected status" "expected corrective" "$INV"
INV2=$(timeout 5 curl -s -X POST -d "no evidence here" "$BASE:$QMS_A/nc?id=$A2&action=close")
t_nc "close from open without evidence rejected" "$INV2"
t_contains "close rejection explains rule" "close requires" "$INV2"

S5=$(timeout 5 curl -s -d $'root caused\tverified by suite\tnote here\tbob' \
        "$BASE:$QMS_A/nc?id=$A3&action=close")
t_contains "close from any status with CA+evidence" "ok $A3 closed" "$S5"
D3=$(timeout 5 curl -s "$BASE:$QMS_A/nc?id=$A3")
t_contains "corrective action stored" "root caused" "$D3"
t_contains "closed_by stored" "bob" "$D3"
t_contains "evidence stored" "verified by suite" "$D3"

t_contains "nc transitions in the note feed" "transition:" \
    "$(timeout 5 curl -s "$EM/notes?q=transition&limit=5")"
t "nc status filter (open = 1)" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/nc?status=open" | grep -c .)"
NCLIST=$(timeout 5 curl -s "$BASE:$QMS_A/nc")
t "nc list has 3 lines" 3 "$(printf '%s\n' "$NCLIST" | grep -c .)"
t "nc list json parses to 3" 3 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/nc?json=1" | python3 -c 'import sys,json;print(len(json.load(sys.stdin)))')"
t_nc "nc unknown id -> 404" "$(timeout 5 curl -s "$BASE:$QMS_A/nc?id=zzz")"

# ---- metric trend (ISO 9004 sustained success) ------------------------------
em_set "metric:iter1:tests_passing" 100
em_set "metric:iter2:tests_passing" 120
TR=$(timeout 5 curl -s "$BASE:$QMS_A/trends")
t "trends has 2 value lines" 2 "$(printf '%s\n' "$TR" | grep -c 'metric:iter')"
t "trend up verdict" "trend up" "$(printf '%s\n' "$TR" | tail -1)"
TRJ=$(timeout 5 curl -s "$BASE:$QMS_A/trends?json=1")
t "trends json parses" 1 "$(printf '%s' "$TRJ" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(1 if d["trend"]=="up" and len(d["values"])==2 else 0)')"

# ---- full audit program (all 5 checks) --------------------------------------
AUD=$(timeout 30 curl -s -d $'iter5 closing audit\t' $BASE:$QMS_A/audit)
AUDID=$(printf '%s' "$AUD" | awk '{print $2}')
t_contains "full audit runs, ok + score" "ok $AUDID 100%" "$AUD"
REP=$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$AUDID")
fcount() { printf '%s\n' "$REP" | awk -F'\t' -v c="$1" -v r="$2" '$1==c && $2==r {n++} END {print n+0}'; }
t "audit report has 9 findings" 9 \
    "$(printf '%s\n' "$REP" | awk -F'\t' '$1=="component-tests"||$1=="doc-compliance"||$1=="dogfood"||$1=="ui-audit"||$1=="metrics"||$1=="code-safety"||$1=="asset-logic"||$1=="docs-coverage"||$1=="kit-fidelity" {n++} END {print n+0}')"
t "kit-fidelity passed in default program" 1 "$(fcount kit-fidelity pass)"
t "component-tests passed" 1 "$(fcount component-tests pass)"
t "doc-compliance passed" 1 "$(fcount doc-compliance pass)"
t "dogfood passed" 1 "$(fcount dogfood pass)"
t "ui-audit skipped (no target)" 1 "$(fcount ui-audit skip)"
t "metrics passed" 1 "$(fcount metrics pass)"
t "code-safety passed (manifest dirs, 0 major)" 1 "$(fcount code-safety pass)"
t "asset-logic passed (repo root, 0 major)" 1 "$(fcount asset-logic pass)"
t "docs-coverage passed in default program" 1 "$(fcount docs-coverage pass)"
t_contains "doc evidence has exodoc score" "score" "$REP"
t "audits list has 1 line" 1 "$(timeout 5 curl -s "$BASE:$QMS_A/audits" | grep -c .)"
t_nc "unknown check id rejected" \
    "$(timeout 5 curl -s -d $'x\tnosuchcheck' $BASE:$QMS_A/audit)"

# ---- docs-coverage (merge gate: README + test/test.sh + standard) ----------
DC1=$(timeout 20 curl -s -d $'docs ok\tdocs-coverage' $BASE:$QMS_A/audit)
DC1ID=$(printf '%s' "$DC1" | awk '{print $2}')
t "docs-coverage passes when every module ships the files" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$DC1ID" | awk -F'\t' '$1=="docs-coverage" && $2=="pass" {n++} END {print n+0}')"
t_contains "docs-coverage evidence counts the modules" "1/1 modules" \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$DC1ID")"

DC2=$(timeout 20 curl -s -d $'docs bad\tdocs-coverage' $BASE:$QMS_C/audit)
DC2ID=$(printf '%s' "$DC2" | awk '{print $2}')
DC2REP=$(timeout 5 curl -s "$BASE:$QMS_C/audit?id=$DC2ID")
t "docs-coverage fails on missing README/test/standard" 1 \
    "$(printf '%s\n' "$DC2REP" | awk -F'\t' '$1=="docs-coverage" && $2=="fail" {n++} END {print n+0}')"
t_contains "docs-coverage failure names the module" "badcomp" "$DC2REP"
t_contains "docs-coverage failure names the missing files" "test/test.sh" "$DC2REP"

# ---- agent-health (freeze detection) ---------------------------------------
# simulate: a fired exosched reminder for a silent agent (exosched writes
# "fired timer" notes; the zombie agent has no activity and no done marker)
timeout 5 curl -s -X POST "$EM/note" -d 'fired timer sim:123 agent:zombie: freeze probe at 9999999999999' > /dev/null
AH=$(timeout 20 curl -s -d $'freeze sim\tagent-health\tzombie' $BASE:$QMS_A/audit)
AHID=$(printf '%s' "$AH" | awk '{print $2}')
AHREP=$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$AHID")
t "agent-health: silent agent flagged frozen" 1 \
    "$(printf '%s\n' "$AHREP" | awk -F'\t' '$1=="agent-health" && $2=="fail" {n++} END {print n+0}')"
t_contains "agent-health: freeze evidence" "likely frozen" "$AHREP"
em_set "agent:zombie:done" ok
AH2=$(timeout 20 curl -s -d $'freeze sim2\tagent-health\tzombie' $BASE:$QMS_A/audit)
AH2ID=$(printf '%s' "$AH2" | awk '{print $2}')
AH2REP=$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$AH2ID")
t "agent-health: done marker clears" 1 \
    "$(printf '%s\n' "$AH2REP" | awk -F'\t' '$1=="agent-health" && $2=="pass" {n++} END {print n+0}')"
timeout 5 curl -s "$EM/del?key=agent:zombie:done" > /dev/null

# ---- ui-audit variants ------------------------------------------------------
UI1=$(timeout 20 curl -s -d $'ui check\tui-audit' $BASE:$QMS_A/audit)
t_contains "ui-audit without target skips" "ok " "$UI1"
UI1ID=$(printf '%s' "$UI1" | awk '{print $2}')
t "ui-audit skip finding" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$UI1ID" | awk -F'\t' '$1=="ui-audit" && $2=="skip" {n++} END {print n+0}')"

touch "$TDIR/ui-flag"
UI2=$(timeout 20 curl -s -d $'ui check with findings\tui-audit' \
        "$BASE:$QMS_A/audit?target=/tmp/b1q-fake-ui-target.html")
UI2ID=$(printf '%s' "$UI2" | awk '{print $2}')
t "ui-audit finds findings -> fail" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$UI2ID" | awk -F'\t' '$1=="ui-audit" && $2=="fail" {n++} END {print n+0}')"
t_contains "ui-audit finding evidence has reason" "spacing" \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$UI2ID")"
rm -f "$TDIR/ui-flag"

# ---- code-safety (exoqms-code) ---------------------------------------------
CS1=$(timeout 20 curl -s -d $'code clean\tcode-safety' $BASE:$QMS_A/audit)
CS1ID=$(printf '%s' "$CS1" | awk '{print $2}')
t "code-safety passes with 0 findings" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$CS1ID" | awk -F'\t' '$1=="code-safety" && $2=="pass" {n++} END {print n+0}')"
t "code-safety default target = repo root" 1 \
    "$([ -f "$TDIR/code-trace" ] && grep -q "$TDIR/repo" "$TDIR/code-trace" && echo 1 || echo 0)"

CS2ID=""
if [ ! -f "$TDIR/code-minor" ]; then touch "$TDIR/code-minor"; fi
CS2=$(timeout 20 curl -s -d $'code minor\tcode-safety' $BASE:$QMS_A/audit)
CS2ID=$(printf '%s' "$CS2" | awk '{print $2}')
t "code-safety minor-only findings non-fatal (pass)" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$CS2ID" | awk -F'\t' '$1=="code-safety" && $2=="pass" {n++} END {print n+0}')"
t_contains "code-safety evidence counts minor" "1 finding" \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$CS2ID")"
rm -f "$TDIR/code-minor"

if [ ! -f "$TDIR/code-major" ]; then touch "$TDIR/code-major"; fi
CS3=$(timeout 20 curl -s -d $'code dirty\tcode-safety' $BASE:$QMS_A/audit)
CS3ID=$(printf '%s' "$CS3" | awk '{print $2}')
t "code-safety fails on major finding" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$CS3ID" | awk -F'\t' '$1=="code-safety" && $2=="fail" {n++} END {print n+0}')"
t_contains "code-safety failure names check" "unchecked-fopen" \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$CS3ID")"
rm -f "$TDIR/code-major"

# ---- memory-awareness (mandate enforcement) --------------------------------
curl -s -m 3 -X POST "$EM/set?key=mandate" -d 'MANDATE: read memory first' > /dev/null
MA1=$(timeout 20 curl -s -d $'mem unready\tmemory-awareness' $BASE:$QMS_A/audit)
MA1ID=$(printf '%s' "$MA1" | awk '{print $2}')
t "memory-awareness fails without ready markers" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$MA1ID" | awk -F'\t' '$1=="memory-awareness" && $2=="fail" {n++} END {print n+0}')"
curl -s -m 3 -X POST "$EM/set?key=agent:b1:ready" -d 1 > /dev/null
curl -s -m 3 -X POST "$EM/set?key=agent:b2:ready" -d 1 > /dev/null
MA2=$(timeout 20 curl -s -d $'mem ready\tmemory-awareness' $BASE:$QMS_A/audit)
MA2ID=$(printf '%s' "$MA2" | awk '{print $2}')
t "memory-awareness passes after ack" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$MA2ID" | awk -F'\t' '$1=="memory-awareness" && $2=="pass" {n++} END {print n+0}')"



# ---- detection registry + issue-tracking (recurrence) ----------------------
# earlier sections already ran failing code-safety audits (code-safety CS3),
# so issue:code-safety may carry prior counts; reset it so this section owns
# the record and the expected counters (consec=1, consec=2, fails=2) are exact
timeout 5 curl -s -X POST "$EM/del?key=issue:code-safety" > /dev/null
if [ ! -f "$TDIR/code-major" ]; then touch "$TDIR/code-major"; fi
DR1=$(timeout 20 curl -s -d $'detect 1\tcode-safety' $BASE:$QMS_A/audit)
DR1ID=$(printf '%s' "$DR1" | awk '{print $2}')
t "detection registered (open)" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/issues" | awk -F'\t' '$1=="code-safety" && $2=="open" && $4=="consec=1" {n++} END {print n+0}')"
DR2=$(timeout 20 curl -s -d $'detect 2\tcode-safety' $BASE:$QMS_A/audit)
DR2ID=$(printf '%s' "$DR2" | awk '{print $2}')
t "recurrence counted (consec=2)" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/issues" | awk -F'\t' '$1=="code-safety" && $2=="open" && $4=="consec=2" {n++} END {print n+0}')"
t_contains "issues endpoint lists evidence" "unchecked-fopen" \
    "$(timeout 5 curl -s "$BASE:$QMS_A/issues")"
DR3=$(timeout 20 curl -s -d $'recur\tissue-tracking' $BASE:$QMS_A/audit)
DR3ID=$(printf '%s' "$DR3" | awk '{print $2}')
t "issue-tracking fails on recurrence" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$DR3ID" | awk -F'\t' '$1=="issue-tracking" && $2=="fail" {n++} END {print n+0}')"
t_contains "issue-tracking names the recurring check" "RECURRING code-safety" \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$DR3ID")"
rm -f "$TDIR/code-major"
DR4=$(timeout 20 curl -s -d $'fix\tcode-safety' $BASE:$QMS_A/audit)
DR4ID=$(printf '%s' "$DR4" | awk '{print $2}')
t "detection closes after fix" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/issues" | awk -F'\t' '$1=="code-safety" && $2=="closed" {n++} END {print n+0}')"
t_contains "history kept after close" "fails=2" \
    "$(timeout 5 curl -s "$BASE:$QMS_A/issues")"

# ---- kit-fidelity (exokit) --------------------------------------------------
KF1=$(timeout 20 curl -s -d $'kit clean\tkit-fidelity' $BASE:$QMS_A/audit)
KF1ID=$(printf '%s' "$KF1" | awk '{print $2}')
t "kit-fidelity passes with green ledger" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$KF1ID" | awk -F'\t' '$1=="kit-fidelity" && $2=="pass" {n++} END {print n+0}')"
t "kit-fidelity invokes audit --kit <repo>/kit" 1 \
    "$([ -f "$TDIR/kit-trace" ] && grep -q 'audit' "$TDIR/kit-trace" && grep -q -- '--kit' "$TDIR/kit-trace" && grep -q "$TDIR/repo/kit" "$TDIR/kit-trace" && echo 1 || echo 0)"

if [ ! -f "$TDIR/kit-major" ]; then touch "$TDIR/kit-major"; fi
KF2=$(timeout 20 curl -s -d $'kit dirty\tkit-fidelity' $BASE:$QMS_A/audit)
KF2ID=$(printf '%s' "$KF2" | awk '{print $2}')
t "kit-fidelity fails on ledger drift" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$KF2ID" | awk -F'\t' '$1=="kit-fidelity" && $2=="fail" {n++} END {print n+0}')"
t_contains "kit-fidelity failure names check" "example-fail" \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$KF2ID")"
rm -f "$TDIR/kit-major"

# kit-fidelity skips when the repo has no kit/ dir
KF3=$(timeout 20 curl -s -d $'kit absent\tkit-fidelity' $BASE:$QMS_C/audit)
KF3ID=$(printf '%s' "$KF3" | awk '{print $2}')
t "kit-fidelity skips without kit/ dir" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_C/audit?id=$KF3ID" | awk -F'\t' '$1=="kit-fidelity" && $2=="skip" {n++} END {print n+0}')"

# ---- asset-logic (exoqms-svg) ----------------------------------------------
AS1=$(timeout 20 curl -s -d $'svg clean\tasset-logic' $BASE:$QMS_A/audit)
AS1ID=$(printf '%s' "$AS1" | awk '{print $2}')
t "asset-logic passes with 0 findings" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$AS1ID" | awk -F'\t' '$1=="asset-logic" && $2=="pass" {n++} END {print n+0}')"
t "asset-logic invokes --shape auto --json" 1 \
    "$([ -f "$TDIR/svg-trace" ] && grep -q -- "--shape" "$TDIR/svg-trace" && grep -q -- "--json" "$TDIR/svg-trace" && grep -q "$TDIR/repo" "$TDIR/svg-trace" && echo 1 || echo 0)"

if [ ! -f "$TDIR/svg-major" ]; then touch "$TDIR/svg-major"; fi
AS2=$(timeout 20 curl -s -d $'svg dirty\tasset-logic' $BASE:$QMS_A/audit)
AS2ID=$(printf '%s' "$AS2" | awk '{print $2}')
t "asset-logic fails on major finding" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$AS2ID" | awk -F'\t' '$1=="asset-logic" && $2=="fail" {n++} END {print n+0}')"
rm -f "$TDIR/svg-major"

if [ ! -f "$TDIR/svg-minor" ]; then touch "$TDIR/svg-minor"; fi
AS3=$(timeout 20 curl -s -d $'svg minor\tasset-logic' $BASE:$QMS_A/audit)
AS3ID=$(printf '%s' "$AS3" | awk '{print $2}')
t "asset-logic minor-only findings non-fatal (pass)" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$AS3ID" | awk -F'\t' '$1=="asset-logic" && $2=="pass" {n++} END {print n+0}')"
rm -f "$TDIR/svg-minor"

# ---- field checks skip when binary not configured (instance C) -------------
CSKIP=$(timeout 20 curl -s -d $'no code binary\tcode-safety,asset-logic' \
        $BASE:$QMS_C/audit)
CSKIPID=$(printf '%s' "$CSKIP" | awk '{print $2}')
t "code-safety skips without --code" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_C/audit?id=$CSKIPID" | awk -F'\t' '$1=="code-safety" && $2=="skip" {n++} END {print n+0}')"
t "asset-logic skips without --svg" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_C/audit?id=$CSKIPID" | awk -F'\t' '$1=="asset-logic" && $2=="skip" {n++} END {print n+0}')"

# ---- doc-compliance against a bad fixture (instance C) ----------------------
DOCF=$(timeout 20 curl -s -d $'doc fail\tdoc-compliance' $BASE:$QMS_C/audit)
DOCFID=$(printf '%s' "$DOCF" | awk '{print $2}')
t "doc-compliance detects non-compliant README" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_C/audit?id=$DOCFID" | awk -F'\t' '$1=="doc-compliance" && $2=="fail" {n++} END {print n+0}')"
DOGF=$(timeout 20 curl -s -d $'dogfood notes fail\tdogfood' $BASE:$QMS_C/audit)
DOGFID=$(printf '%s' "$DOGF" | awk '{print $2}')
t "dogfood fails when notes threshold unmet" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_C/audit?id=$DOGFID" | awk -F'\t' '$1=="dogfood" && $2=="fail" {n++} END {print n+0}')"
DOGA=$(timeout 20 curl -s -d $'dogfood agent missing\tdogfood' \
        "$BASE:$QMS_A/audit?agents=b1,ghost")
DOGAID=$(printf '%s' "$DOGA" | awk '{print $2}')
t_contains "dogfood fails on missing agent key" "agent:ghost:status" \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$DOGAID")"

# ---- metric decline ----------------------------------------------------------
em_set "metric:iter3:tests_passing" 90
t "trend down verdict" "trend down" \
    "$(timeout 5 curl -s "$BASE:$QMS_A/trends" | tail -1)"
MD=$(timeout 20 curl -s -d $'metrics down\tmetrics' $BASE:$QMS_A/audit)
MDID=$(printf '%s' "$MD" | awk '{print $2}')
t "metrics check fails on decline" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$MDID" | grep -c 'trend down')"
em_set "metric:iter3:tests_passing" 120
t "trend flat verdict" "trend flat" \
    "$(timeout 5 curl -s "$BASE:$QMS_A/trends" | tail -1)"

# ---- consolidated report -----------------------------------------------------
em_set "metric:iter5:tests_passing" 350
em_set "metric:iter5:bugs_found" 0
RPT=$(timeout 5 curl -s "$BASE:$QMS_A/report")
t_contains "report: objective met" "objective" "$RPT"
t "report objectives summary 2/3 met, 1 no-data" $'objectives_summary\t2/3 met\t1 no-data' \
    "$(printf '%s\n' "$RPT" | grep '^objectives_summary')"
t "report open_ncs = 1" $'open_ncs\t1' "$(printf '%s\n' "$RPT" | grep '^open_ncs')"
t_contains "report has last audit" "last_audit" "$RPT"
t "report trend up (iter5 newest)" $'trend\tup' "$(printf '%s\n' "$RPT" | grep '^trend')"
t "report stagnation clear on up" $'stagnation\t0' "$(printf '%s\n' "$RPT" | grep '^stagnation')"
RPJ=$(timeout 5 curl -s "$BASE:$QMS_A/report?json=1")
t "report json parses" 1 "$(printf '%s' "$RPJ" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(1 if d["open_ncs"]==1 and d["trend"]=="up" and d["objectives_summary"]["met"]==2 else 0)')"

# ---- auth (instance B) -------------------------------------------------------
AUTH1=$(timeout 5 curl -s $BASE:$QMS_B/ping)
t "no token -> 401" 1 "$(printf '%s' "$AUTH1" | grep -c 'error: unauthorized')"
AUTH2=$(timeout 5 curl -s -H "Authorization: Bearer wrong" $BASE:$QMS_B/ping)
t "wrong token -> 401" 1 "$(printf '%s' "$AUTH2" | grep -c 'error: unauthorized')"
AUTH3=$(timeout 5 curl -s -H "Authorization: Bearer sekrit" $BASE:$QMS_B/ping)
t "correct token -> pong" "pong" "$AUTH3"

# ---- hanging child killed on timeout (instance B) ----------------------------
START=$(date +%s)
HANG=$(timeout 25 curl -s -H "Authorization: Bearer sekrit" \
        -d $'hang test\tui-audit' "$BASE:$QMS_B/audit?target=/tmp/b1q-hang.html")
DUR=$(( $(date +%s) - START ))
echo "DEBUG HANG=[$HANG]" >&2
HANGID=$(printf '%s' "$HANG" | awk '{print $2}')
HANGREP=$(timeout 5 curl -s -H "Authorization: Bearer sekrit" \
        "$BASE:$QMS_B/audit?id=$HANGID")
t_contains "hanging ui-audit times out and fails" "timed out" "$HANGREP"
t "timeout killed child within ~5s budget" 1 "$([ $DUR -le 12 ] && echo 1 || echo 0)"
sleep 1
t "no stray sleep 31 left behind" 0 "$(pgrep -f 'sleep 31' | wc -l)"
t "daemon alive after timeout kill" "pong" \
    "$(timeout 5 curl -s -H "Authorization: Bearer sekrit" $BASE:$QMS_B/ping)"

# hanging code-safety child killed on timeout (instance B, stub-hang as --code)
START=$(date +%s)
HANG2=$(timeout 25 curl -s -H "Authorization: Bearer sekrit" \
        -d $'hang code test\tcode-safety' "$BASE:$QMS_B/audit")
DUR2=$(( $(date +%s) - START ))
HANG2ID=$(printf '%s' "$HANG2" | awk '{print $2}')
HANG2REP=$(timeout 5 curl -s -H "Authorization: Bearer sekrit" \
        "$BASE:$QMS_B/audit?id=$HANG2ID")
t_contains "hanging code-safety times out and fails" "timed out" "$HANG2REP"
t "code-safety timeout within ~5s budget" 1 "$([ $DUR2 -le 12 ] && echo 1 || echo 0)"
sleep 1
t "no stray sleep 30 after code-safety kill" 0 "$(pgrep -f 'sleep 30' | wc -l)"

# ---- robustness / fuzz -------------------------------------------------------
t_contains "unknown path -> 404" "error: unknown path" \
    "$(timeout 5 curl -s $BASE:$QMS_A/bogus/path)"
t "wrong method -> 405" 1 \
    "$(timeout 5 curl -s -X DELETE $BASE:$QMS_A/objectives | grep -c 'error: use GET')"
t "nc id 404" 1 \
    "$(timeout 5 curl -s $BASE:$QMS_A/nc?id=doesnotexist | grep -c 'error: no such nc')"
t "oversized body -> 413" 1 \
    "$(head -c 2000000 /dev/zero | tr '\0' 'a' > "$TDIR/big.txt"; \
      timeout 15 curl -s --data-binary @"$TDIR/big.txt" \
        $BASE:$QMS_A/nc | grep -c 'error: body too large')"
GARB=$(printf 'GET / HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer \r\n\r\n\x01\x02\xff' | \
    timeout 5 python3 -c '
import socket,sys
s=socket.create_connection(("127.0.0.1",'$QMS_A'),3)
s.sendall(sys.stdin.buffer.read())
s.settimeout(3)
try:
    d=s.recv(1024)
    sys.stdout.buffer.write(d[:60])
except Exception:
    pass')
t "garbage request tolerated" 1 \
    "$(printf '%s' "$GARB" | grep -cE 'HTTP/1.1 (400|404|200)')"
t "daemon alive after fuzz" "pong" "$(timeout 5 curl -s $BASE:$QMS_A/ping)"
t_contains "audit json parses" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audit?id=$AUDID&json=1" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(1 if d["score"]==100 and len(d["findings"])==9 else 0)')"
t_contains "audits json parses" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/audits?json=1" | python3 -c 'import sys,json;print(1 if len(json.load(sys.stdin))>=4 else 0)')"

# ---- console operations (one-shot, in-process, no server) -------------------
GUIDE=$(timeout 5 ./build/exoqms 2>/dev/null); rc=$?
t "console: no args prints the guide, exit 0" 0 "$rc"
t_contains "console: guide is self-describing" "exoqms v" "$GUIDE"
t_contains "console: guide lists the operations" "/objectives" "$GUIDE"
COBJ=$(timeout 5 ./build/exoqms /objectives --exomind $EM --repo "$TDIR/repo" 2>/dev/null); rc=$?
t "console: /objectives GET lists, exit 0" 0 "$rc"
t "console: /objectives reflects the daemon state" 3 "$(printf '%s\n' "$COBJ" | grep -c .)"
t_contains "console: /objectives carries the period field" "q2" "$COBJ"
CONA=$(timeout 20 ./build/exoqms /audit?criteria=metrics --exomind $EM \
    --repo "$TDIR/repo" 2>/dev/null); rc=$?
t "console: /audit?criteria= runs in-process, exit 0" 0 "$rc"
t_contains "console: audit scores the metrics check" "ok " "$CONA"
t_contains "console: audit verdict is a percentage" "100%" "$CONA"
t_contains "console: criteria=detect aliases to the standard program" "ok " \
    "$(timeout 40 ./build/exoqms /audit?criteria=detect --exomind $EM \
      --repo "$TDIR/repo" --ui "$TDIR/stub-ui" --code "$TDIR/stub-code" \
      --kit "$TDIR/stub-kit" --svg "$TDIR/stub-svg" 2>/dev/null)"
t_contains "console: /report works in-process" "objectives_summary" \
    "$(timeout 5 ./build/exoqms /report --exomind $EM 2>/dev/null)"
t_contains "console: /issues lists the registry" "code-safety" \
    "$(timeout 5 ./build/exoqms /issues --exomind $EM 2>/dev/null)"
t "console: bad body exits 1" 1 \
    "$(timeout 5 ./build/exoqms /objectives --body 'only-one-field' \
      --exomind $EM >/dev/null 2>&1; echo $?)"
t "console: unknown op exits 2 (usage)" 2 \
    "$(timeout 5 ./build/exoqms /nosuchop >/dev/null 2>&1; echo $?)"
./build/exoqms --serve --port $QMS_D --exomind $EM --exodoc $EXODOC_BIN \
    --repo "$TDIR/repo" > "$TDIR/consoled.log" 2>&1 &
CONSOLE_PID=$!
PIDS="$PIDS $CONSOLE_PID"
ok=0
for i in $(seq 1 40); do
    if timeout 3 curl -s "$BASE:$QMS_D/ping" | grep -q pong; then ok=1; break; fi
    sleep 0.5
done
t "console: --serve --port binds and answers" 1 "$ok"
kill -9 $CONSOLE_PID 2>/dev/null

# ---- reload after SIGKILL restart --------------------------------------------
eval "kill -9 \$QMS_${QMS_A}_PID" 2>/dev/null
sleep 1
env STUB_UI_FLAG="$TDIR/ui-flag" ./build/exoqms --port $QMS_A \
    --exomind $EM --exodoc $EXODOC_BIN --ui "$TDIR/stub-ui" \
    --repo "$TDIR/repo" --agents b1,b2 --notes24h 1 \
    > "$TDIR/qmsa2.log" 2>&1 &
QMSA_PID=$!
PIDS="$PIDS $QMSA_PID"
ok=0
for i in $(seq 1 40); do
    if timeout 3 curl -s $BASE:$QMS_A/ping | grep -q pong; then ok=1; break; fi
    sleep 0.5
done
t "restarted daemon serves" 1 "$ok"
t "objectives reloaded after restart" 3 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/objectives" | grep -c .)"
t "nc state reloaded after restart" 1 \
    "$(timeout 5 curl -s "$BASE:$QMS_A/nc?id=$A1" | grep -c 'closed')"
t "audits reloaded after restart" 1 \
    "$([ $(timeout 5 curl -s "$BASE:$QMS_A/audits" | grep -c .) -ge 4 ] && echo 1 || echo 0)"

# ---- exomind-down startup retry ----------------------------------------------
kill -9 $EXOMIND_PID 2>/dev/null
sleep 1
./build/exoqms --port $QMS_E --exomind $EM --exodoc $EXODOC_BIN \
    --repo "$TDIR/repo" --agents b1,b2 --notes24h 1 \
    > "$TDIR/qmse.log" 2>&1 &
PIDS="$PIDS $!"
ok=0
for i in $(seq 1 40); do
    if timeout 3 curl -s $BASE:$QMS_E/ping | grep -q pong; then ok=1; break; fi
    sleep 0.5
done
t "daemon serves with exomind down" 1 "$ok"
t_nc "write rejected while exomind down" \
    "$(timeout 5 curl -s -d $'x\tmajor\ty' $BASE:$QMS_E/nc)"
"$TDIR/exomind" --port $EM_PORT --data "$TDIR/exomind.dat" \
    > "$TDIR/exomind2.log" 2>&1 &
PIDS="$PIDS $!"; EXOMIND_PID=$!
ok=0
for i in $(seq 1 30); do
    if timeout 3 curl -s "$EM/ping" | grep -q pong; then ok=1; break; fi
    sleep 0.5
done
t "exomind restarted" 1 "$ok"
ok=0
for i in $(seq 1 20); do
    if [ "$(timeout 5 curl -s "$BASE:$QMS_E/objectives" | grep -c .)" -ge 3 ]; then ok=1; break; fi
    sleep 1
done
t "background reload picked up state" 1 "$ok"

# ============================================================================
printf '=== results: %d passed, %d failed ===\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
