#!/bin/bash
# quick benchmark: 10k batch sets and 10k batch gets through one HTTP round trip each
set -u
cd "$(dirname "$0")/.."
PORT=17450
DATA=$(mktemp -d)
setsid ./build/exomind --host 127.0.0.1 --port $PORT --data "$DATA/d.dat" </dev/null >/dev/null 2>&1 &
SRV=$!
sleep 0.4

python3 - <<'EOF' > /tmp/opencode/bench_set.json
import json
print(json.dumps([["set", f"bench:{i}", "x" * 100] for i in range(10000)]))
EOF
python3 - <<'EOF' > /tmp/opencode/bench_get.json
import json
print(json.dumps([["get", f"bench:{i}"] for i in range(10000)]))
EOF

timed() {
    local desc="$1"
    shift
    local t0 t1
    t0=$(date +%s%N)
    "$@" >/dev/null 2>&1
    t1=$(date +%s%N)
    echo "$desc: $(( (t1 - t0) / 1000000 )) ms"
}

echo "--- 10,000 sets in one batch request"
timed "sets" curl -s -X POST "http://127.0.0.1:$PORT/batch" \
    --data-binary @/tmp/opencode/bench_set.json
echo "--- 10,000 gets in one batch request"
timed "gets" curl -s -X POST "http://127.0.0.1:$PORT/batch" \
    --data-binary @/tmp/opencode/bench_get.json
echo "--- search across 10k values"
timed "search" curl -s "http://127.0.0.1:$PORT/search?q=xxxx&limit=10"
echo "--- store stats after bench"
curl -s "http://127.0.0.1:$PORT/stats"
kill $SRV 2>/dev/null
wait $SRV 2>/dev/null
rm -rf "$DATA"
