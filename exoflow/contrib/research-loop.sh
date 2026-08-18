#!/bin/sh
# research-loop.sh - tracked-topic refresh worker for exoflow.
#
# Keeps stale knowledge self-refreshing: the loop flow in
# exoflow/contrib/topic-refresh.flow runs `every 6h` with three steps
# (search -> fetch -> diffnote); this script is the worker that executes
# them against real daemons, and also maintains the tracked-topic
# registry. Topics are tracked in exomind (the memory is the source of
# truth); exocrawl is used read-only through its /search and /fetch ops.
#
# MEMORY LAYOUT (keys in exomind):
#   topic:index            space-separated registry of tracked topic ids
#   topic:<id>             definition lines (key<TAB>value):
#                            query<TAB><search query>   (required)
#                            n<TAB><result count>       (default 10)
#                            url<TAB><url>              (optional; repeat,
#                                                        overrides results)
#   topic:<id>:results     transient (TTL 6h): url<TAB>title<TAB>snippet,
#                            written by the search step from exocrawl /search
#   topic:<id>:snap.new    transient (TTL 6h): fresh knowledge snapshot,
#                            written by the fetch step
#   topic:<id>:snap        last snapshot the note was diffed against
#   topic:<id>:note        append-only delta log: `=== <iso ts> ===` followed
#                            by the lines that are new/changed vs the previous
#                            snapshot. A refresh that changes nothing appends
#                            nothing - silence is information. Capped at
#                            NOTECAP bytes (default 256 KiB, oldest blocks
#                            trimmed). The snapshot's metadata lines
#                            (`# topic:`, `# query:`, `# refreshed:`) never
#                            enter the note, so a changing refresh timestamp
#                            can never cause a phantom delta.
#
# The diff is line-based: a line of the new snapshot that is absent from
# the previous snapshot counts as new/changed; removed lines are not
# reported (the promoted snapshot is the truth). The explicit `url:`
# lines in a topic definition are fetched instead of the search results.
#
# USAGE
# -----
#   research-loop.sh -m EXOMIND add <id> '<query>' [n [url [url...]]]
#   research-loop.sh -m EXOMIND del <id>
#   research-loop.sh -m EXOMIND list
#   research-loop.sh -m EXOMIND [-X EXOCRAWL] search|fetch|diffnote
#   research-loop.sh -u EXOFLOW -f FLOW -w NAME [-m EXOMIND] [-X EXOCRAWL]
#                    [-M N] [-q]
#
# FLAGS
# -----
#   -m URL   exomind base URL (required)
#   -X URL   exocrawl base URL (required for the search/fetch phases)
#   -u URL   exoflow base URL (driver mode)
#   -f ID    flow id of the loop root (driver mode; the newest iteration
#            of the loop is resolved automatically per claim, so stale
#            records can never double-drive)
#   -w NAME  worker name / step owner (driver mode; [A-Za-z0-9._-])
#   -M N     safety cap on claimed steps per invocation (default 64)
#   -q       quiet: phase chatter suppressed (claims/done still printed)
#
# The driver claims steps with GET /next, executes the phase named after
# the step id (search|fetch|diffnote) and reports POST /step done|failed -
# the same loop worker.sh demonstrates with simulated work. Driver exits
# 0 when no work is left, 2 when a step failed, 1 on protocol errors.
#
# env overrides: MAXSNAP (snapshot cap, default 65536), MAXFETCH (results
# fetched per topic, default 5), NOTECAP (note cap, default 262144).
set -u

UM="" UX="" UFLOW="" FLOW="" ME="" MODE=""
MAXC=64
QUIET=0
MAXSNAP=${MAXSNAP:-65536}
MAXFETCH=${MAXFETCH:-5}
NOTECAP=${NOTECAP:-262144}

usage() { sed -n '2,56p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        -m) UM=$2; shift 2 ;;
        -X) UX=$2; shift 2 ;;
        -u) UFLOW=$2; shift 2 ;;
        -f) FLOW=$2; shift 2 ;;
        -w) ME=$2; shift 2 ;;
        -M) MAXC=$2; shift 2 ;;
        -q) QUIET=1; shift ;;
        *)  [ -n "$MODE" ] && usage
            MODE=$1; shift; break ;;
    esac
done

say() { printf '%s\n' "$*"; }
log() { [ "$QUIET" = "1" ] && return 0; printf '%s\n' "$*"; }

get() { # url -> http body (bounded transport retries, worker.sh pattern)
    local try body rc
    try=0
    while [ $try -lt 6 ]; do
        body=$(curl -s -m 5 "$1")
        rc=$?
        if [ $rc -eq 0 ] && [ -n "$body" ]; then
            printf '%s' "$body"
            return 0
        fi
        try=$((try + 1))
        sleep 0.4
    done
    return 1
}

xm_get() { # key -> value, "" when the key is missing
    local v
    v=$(get "$UM/get?key=$1") || return 1
    printf '%s' "$v" | grep -q '^missing$' && return 0
    printf '%s' "$v"
}

json_escape() { # stdin -> JSON string escapes on one line (line-based)
    awk '{ gsub(/\\/, "\\\\"); gsub(/"/, "\\\""); gsub(/\t/, "\\t");
           gsub(/\r/, "\\r"); printf "%s\\n", $0 }'
}

xm_set() { # key ttl body - JSON shape, immune to exomind's form-body
           # sniffing (a fetched line like "key=x&value=y" must never be
           # misread as a urlencoded form)
    local esc
    esc=$(printf '%s\n' "$3" | json_escape)
    esc=${esc%\\n}
    curl -s -m 15 -H 'Content-Type: application/json' -X POST \
        "$UM/set?key=$1&ttl=$2" \
        --data-binary "{\"key\":\"$1\",\"value\":\"$esc\",\"ttl\":$2}" \
        >/dev/null
}

xm_append() { # key text (newline-merged by exomind)
    printf '%s' "$2" | curl -s -m 15 -X POST "$UM/append?key=$1" \
        --data-binary @- >/dev/null
}

xm_del() { # key
    curl -s -m 15 -X POST "$UM/del?key=$1" >/dev/null
}

id_ok() { # [A-Za-z0-9._-] like exoflow step ids (safe in URLs and keys)
    case "$1" in
        ''|*[!A-Za-z0-9._-]*) return 1 ;;
    esac
    return 0
}

topic_ids() { # tracked ids, one per line
    xm_get topic:index | tr ' ' '\n' | grep -v '^$'
}

def_get() { # topic id, field -> first value
    local d
    d=$(xm_get "topic:$1") || return 1
    printf '%s\n' "$d" | awk -F '\t' -v f="$2" \
        '$1==f { v=$2; sub(/\r$/, "", v); print v; exit }'
}

def_all() { # topic id, field -> every value (url: repeats)
    local d
    d=$(xm_get "topic:$1") || return 1
    printf '%s\n' "$d" | awk -F '\t' -v f="$2" \
        '$1==f { v=$2; sub(/\r$/, "", v); print v }'
}

# ---------------- phases ----------------

PHASE_ERR=0

phase_search() { # exocrawl /search per tracked topic -> topic:<id>:results
    PHASE_ERR=0
    local ids id q n res red
    ids=$(topic_ids) || return 1
    [ -n "$ids" ] || { log "search: no tracked topics"; return 0; }
    for id in $ids; do
        q=$(def_get "$id" query) || { PHASE_ERR=1; continue; }
        if [ -z "$q" ]; then
            log "search: $id has no query; skipping"
            PHASE_ERR=1
            continue
        fi
        n=$(def_get "$id" n)
        [ -n "$n" ] || n=10
        res=$(curl -s -m 30 -G --data-urlencode "q=$q" \
            --data-urlencode "n=$n" "$UX/search" 2>/dev/null)
        if [ -z "$res" ] || printf '%s' "$res" | grep -q '^error:'; then
            log "search: $id FAILED (${res:-no reply})"
            PHASE_ERR=1
            continue
        fi
        # /search lines are <idx>\t<title>\t<url>\t<snippet>; keep the
        # url\<title>\<snippet> triple for the fetch step
        red=$(printf '%s\n' "$res" | awk -F '\t' \
            'NF>=3 && $3!="" { print $3 "\t" $2 "\t" $4 }')
        [ -n "$red" ] || { log "search: $id returned no usable results"; PHASE_ERR=1; continue; }
        xm_set "topic:$id:results" 21600 "$red"
        log "search: $id $(( $(printf '%s' "$red" | grep -c .) )) results"
    done
    [ "$PHASE_ERR" = "0" ]
}

phase_fetch() { # /fetch per top result -> topic:<id>:snap.new
    PHASE_ERR=0
    local ids id q n res urls meta title snip i text SNAP
    ids=$(topic_ids) || return 1
    for id in $ids; do
        res=$(xm_get "topic:$id:results") || continue
        [ -n "$res" ] || { log "fetch: $id nothing to fetch"; continue; }
        q=$(def_get "$id" query)
        urls=$(def_all "$id" url)
        if [ -z "$urls" ]; then
            n=$(def_get "$id" n)
            [ -n "$n" ] || n=10
            urls=$(printf '%s\n' "$res" | awk -F '\t' \
                "NR<=$MAXFETCH { print \$1 }")
        fi
        SNAP="# topic: $id
# query: $q
# refreshed: $(date -u +%FT%TZ)"
        i=0
        for u in $urls; do
            i=$((i + 1))
            meta=$(printf '%s\n' "$res" | awk -F '\t' -v u="$u" \
                '$1==u { print $2 "\t" $3; exit }')
            if [ -n "$meta" ]; then
                title=$(printf '%s' "$meta" | awk -F '\t' '{ print $1 }')
                snip=$(printf '%s' "$meta" | awk -F '\t' '{ print $2 }')
                SNAP="$SNAP
$i. $title - $u"
                [ -n "$snip" ] && SNAP="$SNAP
   $snip"
            else
                SNAP="$SNAP
$i. $u"
            fi
            text=$(curl -s -m 45 -G --data-urlencode "url=$u" \
                --data-urlencode "max=$MAXSNAP" "$UX/fetch" 2>/dev/null)
            if [ -z "$text" ] || printf '%s' "$text" | grep -q '^error:'; then
                log "fetch: $u FAILED (${text:-no reply})"
                PHASE_ERR=1
                continue
            fi
            SNAP="$SNAP
## $u
$text"
        done
        SNAP=$(printf '%s' "$SNAP" | head -c "$MAXSNAP")
        xm_set "topic:$id:snap.new" 21600 "$SNAP"
        log "fetch: $id snapshot ${#SNAP} bytes, $i url(s)"
    done
    [ "$PHASE_ERR" = "0" ]
}

cap_note() { # key: keep the newest blocks within NOTECAP bytes
    local v sz nbc
    v=$(xm_get "$1") || return 1
    [ -n "$v" ] || return 0
    sz=$(printf '%s' "$v" | wc -c)
    [ "$sz" -le "$NOTECAP" ] && return 0
    nbc=$((sz - NOTECAP + 1))
    keep=$(printf '%s' "$v" | tail -c +"$nbc" | awk '
        /^=== [0-9A-Za-z:.+-]+ ===$/ { h=NR }
        { lines[NR]=$0 }
        END { for (i=h; i<=NR; i++) print lines[i] }')
    xm_set "$1" 0 "$keep"
    log "diffnote: $1 trimmed to ${#keep} bytes"
}

phase_diffnote() { # diff snap.new vs snap -> append delta to topic:<id>:note
    PHASE_ERR=0
    local ids id new old delta ts block nlines TMP
    ids=$(topic_ids) || return 1
    TMP=$(mktemp -d /tmp/research-loop.XXXXXX)
    for id in $ids; do
        new=$(xm_get "topic:$id:snap.new") || continue
        [ -n "$new" ] || continue
        old=$(xm_get "topic:$id:snap")
        # delta = lines of the new snapshot absent from the previous one,
        # in document order; metadata and blank lines are structural and
        # never count as knowledge
        printf '%s\n' "$old" > "$TMP/old"
        printf '%s\n' "$new" > "$TMP/new"
        delta=$(awk 'NR==FNR { seen[$0]=1; next }
            $0=="" { next }
            /^# (topic|query|refreshed):/ { next }
            !seen[$0] { print }' "$TMP/old" "$TMP/new")
        if [ -z "$delta" ]; then
            xm_del "topic:$id:snap.new"
            log "diffnote: $id no change (silence)"
            continue
        fi
        ts=$(date -u +%FT%TZ)
        block="=== $ts ==="
        nlines=$(printf '%s\n' "$delta" | grep -c .)
        xm_append "topic:$id:note" "$block
$delta"
        cap_note "topic:$id:note"
        xm_set "topic:$id:snap" 0 "$new"
        xm_del "topic:$id:snap.new"
        say "diffnote: $id +$nlines line(s) @ $ts"
    done
    rm -rf "$TMP"
    [ "$PHASE_ERR" = "0" ]
}

# ---------------- driver mode ----------------

# newest record of the loop rooted at $FLOW ("newest iteration"), "" if the
# flow is not looping (one-shot flows are driven via their own id). only
# records of the family count (id == FLOW, or parent == FLOW) - /loops
# lists every loop in the daemon, and iter numbers are per-family.
newest_iter() {
    local best="" biter=0 iter lid pline par
    for lid in $(curl -s -m 5 "$UFLOW/loops" | awk -F '\t' \
        '$1=="loop" { print $2 }'); do
        if [ "$lid" = "$FLOW" ]; then
            iter=1
        else
            pline=$(curl -s -m 5 "$UFLOW/flow?id=$lid")
            par=$(printf '%s\n' "$pline" | awk -F '\t' \
                '/^loop/ { for (i=1; i<=NF; i++)
                    if (substr($i, 1, 7) == "parent ") { print substr($i, 8); exit } }')
            [ "$par" = "$FLOW" ] || continue
            iter=$(printf '%s\n' "$pline" | awk -F '\t' \
                '/^loop/ { for (i=1; i<=NF; i++)
                    if (substr($i, 1, 5) == "iter ") { print substr($i, 6); exit } }')
        fi
        [ -z "$iter" ] && continue
        if [ "$iter" -gt "$biter" ]; then
            biter=$iter
            best=$lid
        fi
    done
    printf '%s' "$best"
}

driver() {
    local claimed fid step r rc
    claimed=0
    while [ "$claimed" -lt "$MAXC" ]; do
        fid=$(newest_iter)
        [ -n "$fid" ] || fid=$FLOW
        r=$(get "$UFLOW/next?flow=$fid&worker=$ME") || return 1
        case "$r" in
            none)
                # a newer iteration may have spawned mid-claim: re-resolve
                # once before concluding there is no work
                [ "$(newest_iter)" = "$fid" ] && { say "no work left"; return 0; }
                continue
                ;;
            ok\ *)
                set -- $r
                step=$2
                [ -n "$step" ] || { say "bad ok response: '$r'"; return 1; }
                claimed=$((claimed + 1))
                say "claimed $step"
                ;;
            *)
                say "unexpected /next reply: '$r'"
                return 1
                ;;
        esac
        case "$step" in
            search)   phase_search ;;
            fetch)    phase_fetch ;;
            diffnote) phase_diffnote ;;
            *)
                say "unknown step $step"
                curl -s -m 10 -X POST "$UFLOW/step?flow=$fid&id=$step" \
                    --data-binary "failed $ME unknown step" >/dev/null
                return 1
                ;;
        esac
        rc=$?
        if [ "$rc" = "0" ]; then
            r=$(curl -s -m 10 -X POST "$UFLOW/step?flow=$fid&id=$step" \
                --data-binary "done $ME")
            if [ "$r" = "ok" ]; then
                say "done $step"
            else
                say "unexpected /step done reply: '$r'"
                return 1
            fi
        else
            say "failed $step"
            curl -s -m 10 -X POST "$UFLOW/step?flow=$fid&id=$step" \
                --data-binary "failed $ME" >/dev/null
            return 2
        fi
    done
    say "reached max steps ($MAXC); giving up"
    return 1
}

# ---------------- modes ----------------

do_add() { # <id> '<query>' [n [url...]] (args arrive as "$@" from the dispatch)
    [ $# -ge 2 ] || usage
    local id q n def idx
    id=$1; q=$2; shift 2
    id_ok "$id" || { say "bad topic id '$id' (use [A-Za-z0-9._-])"; return 1; }
    [ -n "$q" ] || { say "empty query"; return 1; }
    n=10
    if [ $# -ge 1 ]; then
        case "$1" in
            *[!0-9]*) : ;;
            *) n=$1; shift ;;
        esac
    fi
    def="query	$q"
    [ "$n" = "10" ] || def="$def
n	$n"
    for u in "$@"; do
        def="$def
url	$u"
    done
    xm_set "topic:$id" 0 "$def"
    idx=$(xm_get topic:index)
    case " $idx " in
        *" $id "*) : ;;
        *) xm_set topic:index 0 "$idx $id" ;;
    esac
    say "tracked $id"
}

do_del() { # <id>
    [ $# -ge 1 ] || usage
    local id k keep t
    id=$1
    id_ok "$id" || { say "bad topic id '$id'"; return 1; }
    for k in "topic:$id" "topic:$id:note" "topic:$id:snap" \
        "topic:$id:snap.new" "topic:$id:results"; do
        xm_del "$k"
    done
    keep=""
    for t in $(xm_get topic:index); do
        [ "$t" = "$id" ] || keep="$keep $t"
    done
    xm_set topic:index 0 "$keep"
    say "untracked $id"
}

do_list() {
    local id
    for id in $(topic_ids); do
        printf '%s\t%s\n' "$id" "$(def_get "$id" query)"
    done
}

if [ -n "$UFLOW" ]; then
    [ -n "$FLOW" ] && [ -n "$ME" ] && [ -n "$UM" ] || usage
    driver
    exit $?
fi
[ -n "$MODE" ] && [ -n "$UM" ] || usage
case "$MODE" in
    add)   do_add "$@" ;;
    del)   do_del "$@" ;;
    list)  do_list ;;
    search|fetch)
        [ -n "$UX" ] || usage
        phase_$MODE
        exit $?
        ;;
    diffnote)
        phase_diffnote
        exit $?
        ;;
    *) usage ;;
esac