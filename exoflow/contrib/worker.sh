#!/bin/sh
# exoflow contrib worker: a SIMULATED AGENT that drives the orchestration
# loop against any compliant exoflow daemon.
#
# Loop:  GET /next?flow=F&worker=ME   (auto-claims a runnable step)
#        -> inspect the step description; if it starts with "fail:" the
#           step is failed on purpose (failure-path drill), otherwise it
#           is "executed" (sleep, simulated work) and POSTed as done.
#        -> "none"  : no runnable work left; exit 0.
#
# Usage:
#   worker.sh -u URL -f FLOW -w NAME [-m MAX] [-s SLEEP] [-r PARK] [-e EXOMIND] [-q]
#     -u URL      exoflow base URL (required), e.g. http://127.0.0.1:7676
#     -f FLOW     flow id (required), e.g. the id returned by POST /flow
#     -w NAME     worker name, included in audit notes (required)
#     -m MAX      safety cap on claimed steps (default 64)
#     -s SLEEP    simulated work seconds per step (default 0.5)
#     -r PARK     on `none`, re-poll up to PARK times (default 1 = exit 0
#                 immediately, per the loop spec). Parking keeps a worker
#                 available so a multi-worker run contends for steps
#     -e EXOMIND  optional exomind URL; every claim/step is also appended as
#                 a timestamped note "FLOW|STEP|ACT|WORKER" (| separators:
#                 exomind escapes control chars, so tabs would not survive)
#                 so ordering can be verified from note timestamps
#     -q          quiet: only log claimed/done/failed, not chatter
#
# Exit codes: 0 = loop finished (none left), 1 = protocol/driver error,
# 2 = step failed (POST /step answered 'failed').
#
# stdout protocol: one line per action, e.g. `worker w1: claimed s2`,
# `worker w1: done s2`, `worker w1: no work left`.

usage() {
    sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
}

URL=""
FLOW=""
ME=""
MAX=64
SLEEP=0.5
PARK=1
XM=""
QUIET=0

while [ $# -gt 0 ]; do
    case "$1" in
        -u) URL=$2; shift 2 ;;
        -f) FLOW=$2; shift 2 ;;
        -w) ME=$2; shift 2 ;;
        -m) MAX=$2; shift 2 ;;
        -s) SLEEP=$2; shift 2 ;;
        -r) PARK=$2; shift 2 ;;
        -e) XM=$2; shift 2 ;;
        -q) QUIET=1; shift ;;
        *) usage ;;
    esac
done

[ -n "$URL" ] && [ -n "$FLOW" ] && [ -n "$ME" ] || usage

log() {
    [ "$QUIET" -eq 1 ] && [ "$1" != "claimed" ] && [ "$1" != "done" ] \
        && [ "$1" != "failed" ] && return 0
    printf 'worker %s: %s\n' "$ME" "$*"
}

note() { # step action  (timestamped audit note in exomind for ordering)
    [ -z "$XM" ] && return 0
    printf '%s|%s|%s|%s\n' "$FLOW" "$1" "$2" "$ME" \
        | curl -s -m 3 -X POST "$XM/note" --data-binary @- >/dev/null 2>&1
}

get() { # url -> http body; bounded retries on transport failures
    local try body rc
    try=0
    while [ $try -lt 6 ]; do
        body=$(curl -s -m 3 "$1")
        rc=$?
        if [ $rc -eq 0 ] && [ -n "$body" ]; then
            printf '%s' "$body"
            return 0
        fi
        try=$((try + 1))
        sleep 0.4
    done
    printf 'error: transport-failure'
    return 1
}

step_desc() { # stepid -> description (last TSV field of the step line)
    curl -s -m 3 "$URL/flow?id=$FLOW" 2>/dev/null \
        | awk -F '\t' -v s="$1" '$1=="step" && $2==s { print $NF; exit }'
}

claimed=0
parked=0
while [ "$claimed" -lt "$MAX" ]; do
    r=$(get "$URL/next?flow=$FLOW&worker=$ME") || exit 1
    case "$r" in
        none*)  # contract: `none`; some implementations say `none none`
            parked=$((parked + 1))
            if [ "$parked" -ge "$PARK" ]; then
                log "no work left"
                exit 0
            fi
            log "no work now; parking"
            sleep "$SLEEP"
            continue
            ;;
        ok\ *)
            set -- $r
            STEP=$2
            [ -n "$STEP" ] || { log "bad ok response: '$r'"; exit 1; }
            claimed=$((claimed + 1))
            log "claimed $STEP"
            note "$STEP" claimed
            ;;
        *)
            log "unexpected /next reply: '$r'"
            exit 1
            ;;
    esac

    desc=$(step_desc "$STEP")
    case "$desc" in
        fail:*)
            sleep "$SLEEP"
            r=$(curl -s -m 3 -X POST "$URL/step?flow=$FLOW&id=$STEP" \
                --data-binary "failed $ME saw: $desc")
            if [ "$r" = "ok" ]; then
                log "failed $STEP"
                note "$STEP" failed
                exit 2
            else
                log "unexpected /step failed reply: '$r'"
                exit 1
            fi
            ;;
        *)
            sleep "$SLEEP"
            r=$(curl -s -m 3 -X POST "$URL/step?flow=$FLOW&id=$STEP" \
                --data-binary "done $ME")
            if [ "$r" = "ok" ]; then
                log "done $STEP"
                note "$STEP" done
            elif [ "$r" = "failed" ]; then
                log "step $STEP rejected as failed"
                exit 2
            else
                log "unexpected /step done reply: '$r'"
                exit 1
            fi
            ;;
    esac
done

log "reached max steps ($MAX); giving up"
exit 1
