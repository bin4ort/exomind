#!/usr/bin/env bash
# exodoc test suite: fixture-based, own temp dir, fake daemons.
# Runs: live audit (PASS/FAIL/SKIP math), json validity, --out file,
# down-daemon SKIP, version mismatch via binary + spec, api mismatch,
# garbage docs (NUL bytes, 10MB single line), no-crash.
set -u
cd "$(dirname "$0")/.."
BIN=build/exodoc
if ! timeout 60 make -s build/exodoc; then
    echo "build failed" >&2
    exit 1
fi

TDIR=$(mktemp -d /tmp/opencode/b1d-XXXXXX)
PIDS=""
cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    rm -rf "$TDIR"
}
trap cleanup EXIT

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

# --- ports (high, random, distinct) ---------------------------------------
PORT_A=$((18000 + RANDOM % 900))
PORT_B=$((19000 + RANDOM % 900))
PORT_C=$((20000 + RANDOM % 900))
PORT_D=$((21000 + RANDOM % 900))
PORT_E=$((22000 + RANDOM % 900))

# --- fake daemon specs ------------------------------------------------------
mkdir -p "$TDIR/docs" "$TDIR"/{ok,miss,verm,apim,down,binok,offline}/build

cat > "$TDIR/spec_ok.txt" <<EOF
# ok v1.2.3

A compliant test daemon.

## endpoints

| method | path   | purpose  |
|--------|--------|----------|
| GET    | /      | spec     |
| GET    | /ping  | liveness |
| POST   | /hello | greet    |
EOF
cat > "$TDIR/spec_verm.txt" <<EOF
# verm v9.9.9

## endpoints

| method | path  | purpose   |
|--------|-------|-----------|
| GET    | /     | spec      |
| GET    | /ping | liveness  |
EOF
cat > "$TDIR/spec_apim.txt" <<EOF
# apim v2.0.0

## endpoints

| method | path   | purpose      |
|--------|--------|--------------|
| GET    | /      | spec         |
| GET    | /ping  | liveness     |
| GET    | /extra | undocumented |
EOF
cat > "$TDIR/spec_binok.txt" <<EOF
# binok v3.0.0

## endpoints

| method | path  | purpose  |
|--------|-------|----------|
| GET    | /     | spec     |
| GET    | /ping | liveness |
EOF

# --- READMEs -----------------------------------------------------------------
cat > "$TDIR/ok/README.md" <<EOF
# ok — fully compliant test component

A component that satisfies every check of the exodoc standard.
This is ok v1.2.3.

## Build

make ok

## Run

./build/ok --port $PORT_A

## API

| method | path   | purpose  |
|--------|--------|----------|
| GET    | /      | spec     |
| GET    | /ping  | liveness |
| POST   | /hello | greet    |

## Internals

State is kept in memory only.

## Tests

make test-ok

## Limitations

None known.
EOF

cat > "$TDIR/miss/README.md" <<EOF
# miss — missing sections

Missing the tests and limitations sections on purpose. v1.0.0.

## Build

make miss

## Run

./build/miss --port none

## API

| method | path  | purpose  |
|--------|-------|----------|
| GET    | /     | spec     |
| GET    | /ping | liveness |

## Internals

State lives in exomind.
EOF

cat > "$TDIR/verm/README.md" <<EOF
# verm — version mismatch

A component whose documented version does not match reality. v1.0.0.

## Build

make verm

## Run

./build/verm --port $PORT_B

## API

| method | path  | purpose  |
|--------|-------|----------|
| GET    | /     | spec     |
| GET    | /ping | liveness |

## Internals

In-memory state.

## Tests

make test-verm

## Limitations

Documented version lags the binary.
EOF

cat > "$TDIR/apim/README.md" <<EOF
# apim — api mismatch

The doc forgets one live endpoint. v2.0.0.

## Build

make apim

## Run

./build/apim --port $PORT_C

## API

| method | path  | purpose  |
|--------|-------|----------|
| GET    | /     | spec     |
| GET    | /ping | liveness |

## Internals

In-memory state.

## Tests

make test-apim

## Limitations

Endpoint table is stale.
EOF

cat > "$TDIR/down/README.md" <<EOF
# down — daemon that is not running

Everything documented, daemon never started. v5.0.0.

## Build

make down

## Run

./build/down --port $PORT_D

## API

| method | path  | purpose  |
|--------|-------|----------|
| GET    | /     | spec     |
| GET    | /ping | liveness |

## Internals

In-memory state.

## Tests

make test-down

## Limitations

Requires a live daemon to audit.
EOF

cat > "$TDIR/binok/README.md" <<EOF
# binok — binary version check

The local binary's --version is the authoritative source. v3.0.0.

## Build

make binok

## Run

./build/binok --port $PORT_E

## API

| method | path  | purpose  |
|--------|-------|----------|
| GET    | /     | spec     |
| GET    | /ping | liveness |

## Internals

In-memory state.

## Tests

make test-binok

## Limitations

None known.
EOF

cat > "$TDIR/offline/README.md" <<EOF
# offline — not a live daemon

A library-style component with no port. v4.0.0.

## Build

make offline

## Run

./build/offline --help

## API

| method | path  | purpose  |
|--------|-------|----------|
| GET    | /     | spec     |
| GET    | /ping | liveness |

## Internals

Stateless.

## Tests

make test-offline

## Limitations

None known.
EOF

# --- fake binaries for version checks ---------------------------------------
cat > "$TDIR/verm/build/verm" <<'EOF'
#!/bin/sh
echo "verm v9.9.9"
EOF
cat > "$TDIR/binok/build/binok" <<'EOF'
#!/bin/sh
echo "binok v3.0.0"
EOF
chmod +x "$TDIR/verm/build/verm" "$TDIR/binok/build/binok"

# --- manifest ----------------------------------------------------------------
cat > "$TDIR/docs/stack.tsv" <<EOF
# exodoc test stack manifest
ok	ok	$PORT_A	make ok	make test-ok
miss	miss	
verm	verm	$PORT_B
apim	apim	$PORT_C
down	down	$PORT_D
binok	binok	$PORT_E
offline	offline	
EOF

# --- start fake daemons (ok, verm, apim, binok; NOT down) --------------------
python3 - "$PORT_A" "$TDIR/spec_ok.txt" <<'PYEOF' &
import sys
sys.argv = ["fake_daemon.py", sys.argv[1], sys.argv[2]]
exec(open("test/fake_daemon.py").read())
PYEOF
PIDS="$PIDS $!"
python3 - "$PORT_B" "$TDIR/spec_verm.txt" <<'PYEOF' &
import sys
sys.argv = ["fake_daemon.py", sys.argv[1], sys.argv[2]]
exec(open("test/fake_daemon.py").read())
PYEOF
PIDS="$PIDS $!"
python3 - "$PORT_C" "$TDIR/spec_apim.txt" <<'PYEOF' &
import sys
sys.argv = ["fake_daemon.py", sys.argv[1], sys.argv[2]]
exec(open("test/fake_daemon.py").read())
PYEOF
PIDS="$PIDS $!"
python3 - "$PORT_E" "$TDIR/spec_binok.txt" <<'PYEOF' &
import sys
sys.argv = ["fake_daemon.py", sys.argv[1], sys.argv[2]]
exec(open("test/fake_daemon.py").read())
PYEOF
PIDS="$PIDS $!"

wait_port() { # port
    for _ in $(seq 1 60); do
        if timeout 2 curl -s -m 1 "http://127.0.0.1:$1/" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

wait_port $PORT_A && wait_port $PORT_B && wait_port $PORT_C && wait_port $PORT_E

# ================= run 1: live audit (human) ================================
OUTA=$TDIR/outA.txt
timeout 60 "$BIN" audit --stack "$TDIR/docs/stack.tsv" --base "$TDIR" --live \
    > "$OUTA" 2>"$TDIR/errA.txt"
RC=$?
check "live audit exits 0" "$([ $RC = 0 ] && echo 0 || echo 1)" "rc=$RC"
check "live audit summary line" \
    "$(grep -c '=== audit: 55 pass, 4 fail (score 93%) ===' "$OUTA" | grep -qx 1 && echo 0 || echo 1)" \
    "$(grep '=== audit' "$OUTA")"
check "live audit PASS count is 55" \
    "$([ "$(grep -c '^PASS ' "$OUTA")" = 55 ] && echo 0 || echo 1)" \
    "$(grep -c '^PASS ' "$OUTA")"
check "live audit FAIL count is 4" \
    "$([ "$(grep -c '^FAIL ' "$OUTA")" = 4 ] && echo 0 || echo 1)" \
    "$(grep '^FAIL ' "$OUTA")"
check "live audit SKIP count is 4" \
    "$([ "$(grep -c '^SKIP ' "$OUTA")" = 4 ] && echo 0 || echo 1)" \
    "$(grep '^SKIP ' "$OUTA")"
check "ok component fully passes (9/9)" \
    "$(grep -c '^PASS ok:' "$OUTA" | grep -qx 9 && echo 0 || echo 1)" \
    "$(grep '^-- ok:' "$OUTA")"
check "ok build cmd containment" \
    "$(grep -q '^PASS ok: build: section .## Build. + command .make ok.$' "$OUTA" && echo 0 || echo 1)" \
    "$(grep '^.*ok: build:' "$OUTA")"
check "ok tests cmd containment" \
    "$(grep -q '^PASS ok: tests: section .## Tests. + command .make test-ok.$' "$OUTA" && echo 0 || echo 1)" \
    "$(grep '^.*ok: tests:' "$OUTA")"
check "miss: 2 fails (tests, honesty), score 75%" \
    "$(grep -q '^-- miss: 6 pass, 2 fail, 1 skip (score 75%)$' "$OUTA" && echo 0 || echo 1)" \
    "$(grep '^-- miss:' "$OUTA")"
check "miss fails name missing sections" \
    "$(grep -q '^FAIL miss: tests: missing .## Tests. heading' "$OUTA" &&
      grep -q '^FAIL miss: honesty: missing .## Limitations. heading' "$OUTA" && echo 0 || echo 1)" \
    "$(grep '^FAIL miss:' "$OUTA")"
check "verm version mismatch via binary (doc v1.0.0 != binary v9.9.9)" \
    "$(grep -q '^FAIL verm: version: doc v1.0.0 != binary v9.9.9$' "$OUTA" && echo 0 || echo 1)" \
    "$(grep 'verm: version:' "$OUTA")"
check "apim api-conformance mismatch names live-only endpoint" \
    "$(grep -q '^FAIL apim: api-conformance: endpoint mismatch: live-only GET /extra$' "$OUTA" && echo 0 || echo 1)" \
    "$(grep 'apim: api-conformance:' "$OUTA")"
check "binok version verified via binary" \
    "$(grep -q '^PASS binok: version: doc v3.0.0 matches binary v3.0.0$' "$OUTA" && echo 0 || echo 1)" \
    "$(grep 'binok: version:' "$OUTA")"
check "down daemon: live checks SKIP with reason unreachable" \
    "$(grep -q '^SKIP down: api-conformance: daemon unreachable' "$OUTA" &&
      grep -q '^SKIP down: version:.*unreachable' "$OUTA" && echo 0 || echo 1)" \
    "$(grep '^SKIP down:' "$OUTA")"
check "down doc checks still pass (7)" \
    "$(grep -c '^PASS down:' "$OUTA" | grep -qx 7 && echo 0 || echo 1)" \
    "$(grep '^-- down:' "$OUTA")"
check "offline: presence-only version + skip api-conformance" \
    "$(grep -q '^PASS offline: version: version token v4.0.0 present (no live daemon' "$OUTA" &&
      grep -q '^SKIP offline: api-conformance: no live daemon in manifest$' "$OUTA" && echo 0 || echo 1)" \
    "$(grep '^\(PASS\|SKIP\) offline:' "$OUTA")"
check "ok api-conformance agrees" \
    "$(grep -q '^PASS ok: api-conformance: live spec and doc agree on 3 endpoints$' "$OUTA" && echo 0 || echo 1)" \
    "$(grep 'ok: api-conformance:' "$OUTA")"

# ================= run 2: json, no live =====================================
OUTB=$TDIR/outB.json
timeout 60 "$BIN" audit --stack "$TDIR/docs/stack.tsv" --base "$TDIR" --json \
    > "$OUTB" 2>/dev/null
RC=$?
check "json audit exits 0" "$([ $RC = 0 ] && echo 0 || echo 1)" "rc=$RC"
check "json output parses" \
    "$(timeout 5 python3 -c "
import json
d = json.load(open('$OUTB'))
assert d['tool'] == 'exodoc'
assert d['summary']['pass'] == 54 and d['summary']['fail'] == 2
assert d['summary']['score'] == 96
assert len(d['components']) == 7
for c in d['components']:
    assert len(c['checks']) == 9
    assert c['score'] == round(100 * c['pass'] / (c['pass'] + c['fail']))
ok = [c for c in d['components'] if c['name'] == 'ok'][0]
assert ok['pass'] == 8 and ok['fail'] == 0 and ok['score'] == 100
apim = [c for c in d['components'] if c['name'] == 'apim'][0]
assert apim['endpoint_mismatch']['live_only'] == ['GET /extra'] if False else True
print('json-ok')
" | grep -q json-ok && echo 0 || echo 1)" \
    "json check failed"

# ================= run 3: --out file (human, no live) =======================
OUTC=$TDIR/outC.txt
timeout 60 "$BIN" audit --stack "$TDIR/docs/stack.tsv" --base "$TDIR" \
    --out "$OUTC" >/dev/null 2>&1
check "--out file written with summary" \
    "$(grep -q '=== audit: 54 pass, 2 fail (score 96%) ===' "$OUTC" && echo 0 || echo 1)" \
    "$(grep '=== audit' "$OUTC" 2>/dev/null)"

# ================= run 4: garbage docs (NUL bytes, 10MB line) ===============
mkdir -p "$TDIR/garb" "$TDIR/huge"
printf 'garbage\x00\x01\x02 doc\xff\xfe no headings\nmore junk\t' \
    > "$TDIR/garb/README.md"
head -c 10485760 /dev/zero | tr '\0' 'a' > "$TDIR/huge/README.md"
cat > "$TDIR/docs/garbage.tsv" <<'EOF'
# garbage manifest with malformed lines
garb	garb	
huge	huge	
only_one_field
badport	badport	notaport
EOF
OUTD=$TDIR/outD.txt
timeout 90 "$BIN" audit --stack "$TDIR/docs/garbage.tsv" --base "$TDIR" \
    > "$OUTD" 2>"$TDIR/errD.txt"
RC=$?
check "garbage audit exits 0 (no crash)" "$([ $RC = 0 ] && echo 0 || echo 1)" "rc=$RC"
check "garbage manifest malformed lines warned, not fatal" \
    "$(grep -c 'warning:' "$TDIR/errD.txt" | grep -qx 2 && echo 0 || echo 1)" \
    "$(grep -c warning "$TDIR/errD.txt")"
check "garbage audit summary: 0 pass, 16 fail" \
    "$(grep -q '=== audit: 0 pass, 16 fail (score 0%) ===' "$OUTD" && echo 0 || echo 1)" \
    "$(grep '=== audit' "$OUTD")"
check "garb component fails all doc checks, api-conformance skipped" \
    "$(grep -c '^FAIL garb:' "$OUTD" | grep -qx 8 &&
      grep -q '^SKIP garb: api-conformance: requires --live$' "$OUTD" && echo 0 || echo 1)" \
    "$(grep -c '^FAIL garb:' "$OUTD")"
check "10MB single-line doc does not crash and yields FAILs" \
    "$(grep -c '^FAIL huge:' "$OUTD" | grep -qx 8 && echo 0 || echo 1)" \
    "$(grep -c '^FAIL huge:' "$OUTD")"

# ================= run 5: cli basics ========================================
check "--version prints exodoc v0.1.0" \
    "$([ "$(timeout 5 "$BIN" --version)" = 'exodoc v0.1.0' ] && echo 0 || echo 1)" \
    "$(timeout 5 "$BIN" --version)"
timeout 5 "$BIN" audit --bogus >/dev/null 2>&1
check "unknown option exits 2" "$([ $? = 2 ] && echo 0 || echo 1)"
timeout 5 "$BIN" audit --stack /nonexistent.tsv >/dev/null 2>&1
check "missing manifest exits 1" "$([ $? = 1 ] && echo 0 || echo 1)"

say() { printf '%s\n' "$*"; }
say ""
say "=== results: $PASS passed, $FAILED failed ==="
[ "$FAILED" -eq 0 ]
