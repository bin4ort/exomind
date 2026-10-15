#!/usr/bin/env bash
# exocpi test suite

set -euo pipefail

EXOCPI="${EXOCPI:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/build/exocpi}"
EXOMIND="${EXOMIND:-http://127.0.0.1:7654}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

pass=0
fail=0

run_test() {
    local name="$1"
    shift
    local expected="$1"
    shift

    # Quote each argument to prevent shell interpretation of & and ?
    local quoted_args=()
    for arg in "$@"; do
        quoted_args+=("'$arg'")
    done
    local cmd="$EXOCPI --exomind $EXOMIND ${quoted_args[*]}"

    local output
    output=$(eval "$cmd" 2>&1) || true
    if echo "$output" | grep -q "$expected"; then
        echo -e "${GREEN}PASS${NC} $name"
        pass=$((pass + 1))
    else
        echo -e "${RED}FAIL${NC} $name"
        echo "  Command: $cmd"
        echo "  Expected: $expected"
        echo "  Actual output: $output"
        fail=$((fail + 1))
    fi
}

# Ensure exomind is running
if ! curl -s "$EXOMIND/ping" | grep -q pong; then
    echo "exomind not running at $EXOMIND"
    exit 1
fi

echo "=== exocpi test suite ==="

# PDCA
run_test "pdca_create" "ok " '/plan?problem=test&target=goal&metric=metric:test&hypothesis=test'
run_test "pdca_create_missing_problem" "error: missing problem, target, or metric" '/plan?target=goal&metric=metric:test&hypothesis=test'
run_test "pdca_create_missing_target" "error: missing problem, target, or metric" '/plan?problem=test&metric=metric:test'
run_test "pdca_create_missing_metric" "error: missing problem, target, or metric" '/plan?problem=test&target=goal'
run_test "pdca_list" "plan" '/pdca?list=1'
run_test "pdca_get_invalid" "error: not found" '/pdca?id=nonexistent'

# Plan phase requires existing pdca_id
run_test "plan_phase_requires_pdca" "error: missing problem, target, or metric" '/plan?problem=test'

# Do phase requires valid pdca_id
run_test "do_phase_requires_pdca" "error: pdca not found" '/do?pdca=test'

# Check phase requires valid pdca_id
run_test "check_phase_requires_pdca" "error: pdca not found" '/check?pdca=test'

# Act standardize requires valid pdca_id
run_test "act_standardize_requires_pdca" "error: pdca not found" '/act?pdca=test'

# Checksheet
run_test "checksheet" "ok ok" '/checksheet?template=defects'

# Act replan requires valid pdca_id
run_test "act_replan_requires_pdca" "error: pdca not found" '/act?pdca=test'

# QC Tools
run_test "pareto" "ok " '/pareto?source=nc:findings'
run_test "fishbone" "ok graph TD" '/fishbone?problem=test&categories=method,material'
run_test "histogram" "ok bins:" '/histogram?metric=metric:test'
run_test "control" "ok CL=" '/control?metric=metric:test&value=4.8'
run_test "scatter_requires_both" "error: missing x or y metric" '/scatter?x=wip'
run_test "run_chart" "ok points:" '/run-chart?metric=metric:test'
run_test "flowchart" "ok mermaid: graph TD" '/flowchart?flow=test'

# Poka-yoke
run_test "poka_register_requires_params" "error: missing name or method" '/poka?register=1'
run_test "poka_list" "test" '/poka?list=1'

# Kanban
run_test "kanban_create_requires_params" "error: missing flow or columns" '/kanban?create=1'

# 5S
run_test "5s_audit" "ok score=" '/5s?audit=1&agent=test'

# Kaizen
run_test "kaizen_suggest" "ok ok" '/kaizen?suggest=1&agent=test&title=test&problem=prob&suggestion=sugg'

# Standard work
run_test "standard_sync" "ok synced" '/standard?sync=1&kit=.'
run_test "standard_drift" "ok drift:" '/standard?drift=1'

# Report
run_test "report" "ok CPI Management Review" '/report?period=monthly'

echo ""
echo "=== Results: $pass pass, $fail fail ==="
exit $fail