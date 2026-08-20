#!/usr/bin/env bash
# norms-loop.sh — drive the norms-refresh flow: claim the harvest step
# via GET /next, run fetch-norms.sh, report POST /step done|failed, and
# append a delta block to norm:refresh:note only when at least one norm
# changed (silence is information).
# Usage:
#   norms-loop.sh -u http://127.0.0.1:7676 -f <flow-id> -w norms-agent \
#                 -m http://127.0.0.1:7654 -X http://127.0.0.1:7658 \
#                 [-R <fixtures-dir>]   # -R runs fetch-norms.sh --dry-run
set -u
U=""   # exoflow URL
F=""   # flow id
W=""   # worker name
M=""   # exomind URL
X="http://127.0.0.1:7658"
R=""   # optional dry-run fixtures dir
while [ $# -gt 0 ]; do
    case "$1" in
        -u) U="$2"; shift 2 ;;
        -f) F="$2"; shift 2 ;;
        -w) W="$2"; shift 2 ;;
        -m) M="$2"; shift 2 ;;
        -X) X="$2"; shift 2 ;;
        -R) R="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done
[ -n "$U" ] && [ -n "$F" ] && [ -n "$W" ] && [ -n "$M" ] || {
    echo "usage: norms-loop.sh -u <flow-url> -f <flow-id> -w <worker> -m <memory> [-X <crawler>] [-R <fixtures>]" >&2
    exit 2
}
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

say() { printf '%s\n' "$*"; }

newest_iter() {
    # resolve the loop's newest iteration id from /loops (owner == $F)
    curl -s -m 5 "$U/loops" | while IFS= read -r lline; do
        [ -z "$lline" ] && continue
        lid=$(printf '%s' "$lline" | awk -F '\t' '{print $1}')
        # keep only iterations whose flow line belongs to this flow
        pline=$(curl -s -m 5 "$U/flow?id=$lid" 2>/dev/null) || continue
        echo "$pline" | grep -q "\b$F\b" && echo "$lid" && break
    done | tail -1
}

while true; do
    fid=$(newest_iter)
    [ -n "$fid" ] || fid=$F
    CLAIM=$(curl -s -m 10 "$U/next?flow=$fid&worker=$W")
    case "$CLAIM" in
        none)
            sleep 1
            continue
            ;;
        ok\ *)
            set -- $CLAIM
            STEP=$2
            ;;
        *)
            say "unexpected /next reply: '$CLAIM'"
            sleep 2
            continue
            ;;
    esac
    if [ "$STEP" = "harvest" ]; then
        REG0=$(curl -s -m 5 "$M/get?key=norm:index")
        if [ -n "$R" ]; then
            OUT=$(bash "$SCRIPT_DIR/exocrawl/contrib/fetch-norms.sh" --dry-run "$R" "$M" 2>&1)
        else
            OUT=$(bash "$SCRIPT_DIR/exocrawl/contrib/fetch-norms.sh" "$X" "$M" 2>&1)
        fi
        FETCHED=$(echo "$OUT" | grep '^fetch ' || true)
        if [ -n "$FETCHED" ]; then
            {
                printf '=== %s ===\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
                printf '%s\n' "$FETCHED"
            } | curl -s -m 10 --data-binary @- \
                "$M/append?key=norm:refresh:note" >/dev/null 2>&1
            RES=$(curl -s -m 10 -X POST "$U/step?flow=$fid&id=$STEP" \
                --data-binary "done $W")
        else
            RES=$(curl -s -m 10 -X POST "$U/step?flow=$fid&id=$STEP" \
                --data-binary "done $W")
        fi
        [ "$RES" = "ok" ] || say "unexpected /step done reply: '$RES'"
    else
        curl -s -m 10 -X POST "$U/step?flow=$fid&id=$STEP" \
            --data-binary "done $W" >/dev/null
        say "no-op step $STEP"
    fi
    sleep 1
done