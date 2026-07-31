#!/bin/bash
# benchmark/lib.sh — Benchmark framework library
#
# Provides timing, result storage, and regression detection for TinyActor benchmarks.

set -e

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

# Project paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TAVM_BIN="${TAVM:-$PROJECT_DIR/tavm}"
TINYACTOR="$PROJECT_DIR/tinyactor"
BENCHMARK_DIR="$PROJECT_DIR/benchmark"
RESULTS_DIR="$BENCHMARK_DIR/results"

# Git commit for tracking
GIT_COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
GIT_DATE=$(git log -1 --format=%ci 2>/dev/null || echo "unknown")

# Ensure results directory exists
mkdir -p "$RESULTS_DIR"

# Results file path
RESULTS_FILE="$RESULTS_DIR/results.json"
HISTORY_FILE="$RESULTS_DIR/history.csv"

# ============================================
# Benchmark Timing Functions
# ============================================

# time_execution: Run a command and return execution time in seconds
# Usage: time_execution "command" -> returns time in seconds
time_execution() {
    local cmd="$1"
    local start_time end_time elapsed

    start_time=$(python3 -c "import time; print(time.time())")
    bash -c "$cmd" > /dev/null 2>&1
    local exit_code=$?
    end_time=$(python3 -c "import time; print(time.time())")
    elapsed=$(python3 -c "import time; s='$start_time'; e='$end_time'; print(float(e) - float(s))")

    echo "$elapsed"
    return $exit_code
}

# time_with_output: Run command, measure time, and capture output
# Usage: time_with_output "command" -> returns "time|output|exit_code"
time_with_output() {
    local cmd="$1"
    local start_time end_time elapsed output

    start_time=$(python3 -c "import time; print(time.time())")
    output=$(bash -c "$cmd" 2>&1)
    local exit_code=$?
    end_time=$(python3 -c "import time; print(time.time())")
    elapsed=$(python3 -c "import time; s='$start_time'; e='$end_time'; print(float(e) - float(s))")

    echo "${elapsed}|${output}|${exit_code}"
    return $exit_code
}

# ============================================
# Benchmark Execution
# ============================================

# run_benchmark: Run a single benchmark multiple times and return median
# Usage: run_benchmark <name> <command> <iterations>
run_benchmark() {
    local name="$1"
    local cmd="$2"
    local iterations="${3:-5}"
    local times=()
    local output=""
    local exit_code=0

    printf "${CYAN}Running: $name${NC} ($iterations iterations)\n" >&2

    for i in $(seq 1 $iterations); do
        result=$(time_with_output "$cmd")
        elapsed="${result%%|*}"
        rest="${result#*|}"
        out="${rest%|*}"
        exit_code="${rest##*|}"

        times+=("$elapsed")

        # Capture output from first run
        if [ $i -eq 1 ]; then
            output="$out"
        fi

        printf "  run %d: %.3fs\n" $i "$elapsed" >&2
    done

    # Calculate median
    IFS=$'\n' sorted=($(sort -n <<<"${times[*]}"))
    unset IFS
    local median="${sorted[$((iterations/2))]}"

    # Calculate mean
    local sum=0
    for t in "${times[@]}"; do
        sum=$(python3 -c "print($sum + float('$t'))")
    done
    local mean=$(python3 -c "print($sum / $iterations)")

    printf "  median: %.3fs, mean: %.3fs\n\n" "$median" "$mean" >&2

    echo "$mean|$output|$exit_code"
}

# ============================================
# Result Storage
# ============================================

# save_result: Save benchmark result to JSON
# Usage: save_result <category> <name> <time> <output> <exit_code>
save_result() {
    local category="$1"
    local name="$2"
    local time="$3"
    local output="$4"
    local exit_code="$5"

    local timestamp=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
    local entry=$(cat <<EOF
{
  "timestamp": "$timestamp",
  "commit": "$GIT_COMMIT",
  "commit_date": "$GIT_DATE",
  "category": "$category",
  "name": "$name",
  "time": $time,
  "output": $(echo "$output" | jq -Rs .),
  "exit_code": $exit_code
}
EOF
)

                        # Append to results file
    if [ -f "$RESULTS_FILE" ] && [ -s "$RESULTS_FILE" ]; then
        # Remove closing bracket, add comma, append new entry
        local tempfile=$(mktemp)
        # macOS compatible: remove last line (the ']')
        local lines=$(wc -l < "$RESULTS_FILE")
        head -n $((lines - 1)) "$RESULTS_FILE" > "$tempfile"
        # Replace closing '}' with '},'
        sed -i.bak '$ s/}$/},/' "$tempfile"
        echo "$entry" >> "$tempfile"
        echo "]" >> "$tempfile"
        mv "$tempfile" "$RESULTS_FILE"
    else
        echo "[$entry]" > "$RESULTS_FILE"
    fi

    # Append to CSV history
    echo "$timestamp,$GIT_COMMIT,$category,$name,$time,$exit_code" >> "$HISTORY_FILE"
}

# ============================================
# Regression Detection
# ============================================

# check_regression: Check if current result shows regression vs baseline
# Usage: check_regression <category> <name> <current_time> <threshold>
# Returns: 0 if OK, 1 if regression detected
check_regression() {
    local category="$1"
    local name="$2"
    local current_time="$3"
    local threshold="${4:-1.10}"  # 10% slowdown is regression

    # Find baseline (last known good result)
    local baseline=$(jq -r "
        .[]
        | select(.category == \"$category\")
        | select(.name == \"$name\")
        | .time
    " "$RESULTS_FILE" 2>/dev/null | tail -n 2 | head -n 1)

    if [ -z "$baseline" ] || [ "$baseline" = "null" ]; then
        # No baseline, this is the first run
        return 0
    fi

    local ratio=$(python3 -c "print($current_time / $baseline)" 2>/dev/null || echo "1")
    local ratio_int=$(python3 -c "print(int($ratio * 100))" 2>/dev/null || echo "100")

    # Check for regression (more than 10% slower)
    local is_regression=$(python3 -c "
ratio = float('$ratio')
threshold = float('$threshold')
if ratio > threshold:
    print(1)
else:
    print(0)
")

    if [ "$is_regression" = "1" ]; then
        echo -e "${RED}⚠️  REGRESSION: $name${NC}"
        echo -e "  Baseline: ${baseline}s, Current: ${current_time}s (${ratio_int}%)"
        return 1
    fi

    # Check for improvement
    local is_improvement=$(python3 -c "
ratio = float('$ratio')
if ratio < 0.95:
    print(1)
else:
    print(0)
")

    if [ "$is_improvement" = "1" ]; then
        echo -e "${GREEN}✓ IMPROVEMENT: $name${NC}"
        echo -e "  Baseline: ${baseline}s, Current: ${current_time}s (${ratio_int}%)"
    fi

    return 0
}

# ============================================
# Formatting & Output
# ============================================

# print_header: Print category header
print_header() {
    local title="$1"
    echo ""
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}$title${NC}"
    echo -e "${BLUE}========================================${NC}"
}

# print_result: Print benchmark result
print_result() {
    local name="$1"
    local time="$2"
    local output="$3"
    local exit_code="$4"

    # Trim whitespace from exit_code
    exit_code=$(echo "$exit_code" | tr -d '[:space:]')
    # Ensure it's a number, default to 1 if empty
    [ -z "$exit_code" ] && exit_code=1

    if [ "$exit_code" = "0" ]; then
        echo -e "${GREEN}✅ PASS${NC} ${name}: ${time}s"
        if [ -n "$output" ]; then
            echo -e "     Output: $output"
        fi
    else
        echo -e "${RED}❌ FAIL${NC} ${name} (exit $exit_code)"
        if [ -n "$output" ]; then
            echo -e "     Output: $output"
        fi
    fi
}

# ============================================
# Summary
# ============================================

# print_summary: Print benchmark summary
print_summary() {
    echo ""
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}Benchmark Summary${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo "Git commit: $GIT_COMMIT ($GIT_DATE)"
    echo "Results: $RESULTS_FILE"
    echo ""
}