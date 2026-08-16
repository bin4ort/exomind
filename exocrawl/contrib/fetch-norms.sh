#!/usr/bin/env bash
# fetch-norms.sh — harvest freely-available international norms into
# exomind keys (norm:<id>) via exocrawl's /fetch endpoint.
# Usage: bash exocrawl/contrib/fetch-norms.sh [EXOCRAWL_URL] [EXOMIND_URL]
# Each norm is stored capped at 30 KB (token-efficient); the registry is
# maintained in the `norm:index` key.
set -u
EXOCRAWL="${1:-http://127.0.0.1:7658}"
EXOMIND="${2:-http://127.0.0.1:7654}"
MAX="${MAX_NORM:-30000}"

declare -a IDS=("rfc2119" "rfc8259" "pep8" "wcag22" "nng10")
declare -a URLS=(
  "https://www.rfc-editor.org/rfc/rfc2119.txt"
  "https://www.rfc-editor.org/rfc/rfc8259.txt"
  "https://peps.python.org/pep-0008/"
  "https://www.w3.org/TR/WCAG22/"
  "https://www.nngroup.com/articles/ten-usability-heuristics/"
)
declare -a NAMES=(
  "RFC 2119 - Key words for use in RFCs (MUST/SHOULD language)"
  "RFC 8259 - The JavaScript Object Notation (JSON) Data Interchange Format"
  "PEP 8 - Style Guide for Python Code"
  "WCAG 2.2 - Web Content Accessibility Guidelines"
  "NN/g 10 Usability Heuristics for User Interface Design"
)

i=0
for id in "${IDS[@]}"; do
    # already harvested?
    HAVE=$(curl -s --max-time 3 "$EXOMIND/get?key=norm:$id" | head -c 20)
    if [ -n "$HAVE" ] && [ "$HAVE" != "missing" ]; then
        echo "skip $id (already stored)"
        i=$((i + 1))
        continue
    fi
    echo "fetch $id <- ${URLS[$i]}"
    TEXT=$(timeout 60 curl -s --max-time 55 \
        "$EXOCRAWL/fetch?url=${URLS[$i]}&max=$MAX" 2>/dev/null)
    if [ -z "$TEXT" ] || echo "$TEXT" | grep -q '^error:'; then
        echo "  FAIL: $TEXT"
        curl -s --max-time 5 "$EXOMIND/set?key=norm:$id" \
            -d "fetch failed at $(date -u +%FT%TZ)" > /dev/null
        i=$((i + 1))
        continue
    fi
    # store with a header line
    {
        printf '# %s\n# source: %s\n# harvested: %s\n\n' \
            "${NAMES[$i]}" "${URLS[$i]}" "$(date -u +%FT%TZ)"
        printf '%s' "$TEXT"
    } | curl -s --max-time 10 --data-binary @- \
        "$EXOMIND/set?key=norm:$id&ttl=2592000" > /dev/null
    echo "  stored $(wc -c <<< "$TEXT") chars"
    i=$((i + 1))
done

# refresh the registry
REGISTRY=""
for id in "${IDS[@]}"; do
    HAVE=$(curl -s --max-time 3 "$EXOMIND/get?key=norm:$id" | head -c 10)
    if [ -n "$HAVE" ] && [ "$HAVE" != "missing" ]; then
        REGISTRY="$REGISTRY$id "
    fi
done
curl -s --max-time 5 "$EXOMIND/set?key=norm:index" -d "$REGISTRY" > /dev/null
echo "norm:index = $REGISTRY"
