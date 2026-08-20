#!/usr/bin/env bash
# fetch-norms.sh — harvest freely-available international norms into
# exomind keys (norm:<id>) via exocrawl's /fetch endpoint.
# Usage: bash exocrawl/contrib/fetch-norms.sh [EXOCRAWL_URL] [EXOMIND_URL]
#   --dry-run <fixtures-dir> [EXOMIND_URL] — read <fixtures-dir>/<id>.txt
#   instead of the network (offline tests); everything downstream
#   (strip/cap/exomind write/index) is identical to a real run.
# Each norm is stored capped at 30 KB (token-efficient); the registry is
# maintained in the `norm:index` key.
set -u
DRYRUN=0
FIXTURES=""
if [ "${1:-}" = "--dry-run" ]; then
    DRYRUN=1
    FIXTURES="${2:?--dry-run needs a fixtures dir as its 2nd argument}"
    shift 2
fi
if [ "$DRYRUN" = 1 ]; then
    # dry-run has no crawler leg: a single exomind URL (default 7654)
    EXOMIND="${1:-http://127.0.0.1:7654}"
else
    EXOCRAWL="${1:-http://127.0.0.1:7658}"
    EXOMIND="${2:-http://127.0.0.1:7654}"
fi
MAX="${MAX_NORM:-30000}"

declare -a IDS=("rfc2119" "rfc8259" "pep8" "wcag22" "nng10" "rfc3986" "a11y"
                "pep20" "isocpp" "ecma262" "css-selectors" "html-landmarks"
                "material-design" "iso9241-11" "a11y-check" "diataxis"
                "ms-style" "rfc7322" "iso9001-75")
declare -a URLS=(
  "https://www.rfc-editor.org/rfc/rfc2119.txt"
  "https://www.rfc-editor.org/rfc/rfc8259.txt"
  "https://peps.python.org/pep-0008/"
  "https://www.w3.org/TR/WCAG22/"
  "https://www.nngroup.com/articles/ten-usability-heuristics/"
  "https://www.rfc-editor.org/rfc/rfc3986.txt"
  "https://www.w3.org/WAI/fundamentals/accessibility-intro/"
  "https://peps.python.org/pep-0020/"
  "https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines"
  "https://tc39.es/ecma262/"
  "https://www.w3.org/TR/selectors-4/"
  "https://html.spec.whatwg.org/multipage/dom.html#the-landmarks"
  "https://m3.material.io/"
  "https://www.iso.org/obp/ui/en/#iso:std:iso:9241:-11:en"
  "https://www.a11yproject.com/checklist/"
  "https://diataxis.fr/"
  "https://learn.microsoft.com/en-us/style-guide/"
  "https://www.rfc-editor.org/rfc/rfc7322.txt"
  "https://www.iso.org/obp/ui/en/#iso:std:iso:9001:2015:en"
)
declare -a NAMES=(
  "RFC 2119 - Key words for use in RFCs (MUST/SHOULD language)"
  "RFC 8259 - The JavaScript Object Notation (JSON) Data Interchange Format"
  "PEP 8 - Style Guide for Python Code"
  "WCAG 2.2 - Web Content Accessibility Guidelines"
  "NN/g 10 Usability Heuristics for User Interface Design"
  "RFC 3986 - Uniform Resource Identifier (URI): Generic Syntax"
  "W3C WAI - Introduction to Web Accessibility"
  "PEP 20 - The Zen of Python"
  "ISO C++ Core Guidelines (summary/TOC)"
  "ECMA-262 ECMAScript Language Specification (TOC + key sections)"
  "W3C Selectors Level 4"
  "WHATWG HTML - Landmarks and semantics"
  "Google Material Design 3 principles"
  "ISO 9241-11 - Usability: definitions and concepts (summary)"
  "A11y Project checklist"
  "Diataxis framework (documentation)"
  "Microsoft Style Guide (summary)"
  "RFC 7322 - RFC Style Guide"
  "ISO 9001 clause 7.5 - Documented information"
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
    if [ "$DRYRUN" = 1 ]; then
        F="$FIXTURES/$id.txt"
        if [ ! -r "$F" ]; then
            echo "  FAIL: fixture $F missing"
            curl -s --max-time 5 "$EXOMIND/set?key=norm:$id" \
                -d "fetch failed at $(date -u +%FT%TZ)" > /dev/null
            i=$((i + 1))
            continue
        fi
        echo "fetch $id <- fixture $F"
        TEXT=$(head -c "$MAX" "$F")
    else
        echo "fetch $id <- ${URLS[$i]}"
        TEXT=$(timeout 60 curl -s --max-time 55 \
            "$EXOCRAWL/fetch?url=${URLS[$i]}&max=$MAX&polite=0" 2>/dev/null)
        if [ -z "$TEXT" ] || echo "$TEXT" | grep -q '^error:'; then
            echo "  FAIL: $TEXT"
            curl -s --max-time 5 "$EXOMIND/set?key=norm:$id" \
                -d "fetch failed at $(date -u +%FT%TZ)" > /dev/null
            i=$((i + 1))
            continue
        fi
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