#!/bin/bash
# benchmark/run_benchmarks.sh — Main benchmark runner
#
# Usage:
#   ./run_benchmarks.sh              # Run all benchmarks
#   ./run_benchmarks.sh core         # Run only core benchmarks
#   ./run_benchmarks.sh actor        # Run only actor benchmarks
#   ./run_benchmarks.sh gc           # Run only GC benchmarks
#   ./run_benchmarks.sh --regression # Run and check for regression

set -e

# Source library
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib.sh"

# macOS: wrap heavy benchmarks with /usr/bin/time -l so peak RSS shows up
# in the captured output (spawn1m's headline metric). No-op elsewhere.
TIME_CMD=""
if [ "$(uname)" = "Darwin" ]; then
  TIME_CMD="/usr/bin/time -l "
fi

# Parse arguments
REGRESSION_CHECK=0
CATEGORY=""

for arg in "$@"; do
  case "$arg" in
    --regression)
      REGRESSION_CHECK=1
      ;;
    *)
      CATEGORY="$arg"
      ;;
  esac
done

# Categories to run
if [ -z "$CATEGORY" ]; then
  CATEGORIES="core actor gc compiler"
else
  CATEGORIES="$CATEGORY"
fi

# ============================================
# Core Benchmarks
# ============================================

run_core_benchmarks() {
  print_header "Core Performance"

      # Fibonacci
  result=$(run_benchmark "core/fib" \
    "cd '$PROJECT_DIR' && '$TINYACTOR' run benchmark/core/fib.ta" 5)
  time=$(echo "$result" | cut -d'|' -f1)
  output=$(echo "$result" | cut -d'|' -f2 | tail -n 1 | tr -d '\n' | xargs)
  exit_code=$(echo "$result" | cut -d'|' -f3)

  save_result "core" "fib" "$time" "$output" "$exit_code"
  print_result "fib" "$time" "$output" "$exit_code"

  if [ $REGRESSION_CHECK -eq 1 ]; then
    check_regression "core" "fib" "$time"
  fi

  # List map
  result=$(run_benchmark "core/list-map" \
    "cd '$PROJECT_DIR' && '$TINYACTOR' run benchmark/core/list-map.ta" 5)
  time=$(echo "$result" | cut -d'|' -f1)
  output=$(echo "$result" | cut -d'|' -f2 | tail -n 1 | tr -d '\n' | xargs)
  exit_code=$(echo "$result" | cut -d'|' -f3)

  save_result "core" "list-map" "$time" "$output" "$exit_code"
  print_result "list-map" "$time" "$output" "$exit_code"

  if [ $REGRESSION_CHECK -eq 1 ]; then
    check_regression "core" "list-map" "$time"
  fi

  # Tail call
  result=$(run_benchmark "core/tailcall" \
    "cd '$PROJECT_DIR' && '$TINYACTOR' run benchmark/core/tailcall.ta" 5)
  time=$(echo "$result" | cut -d'|' -f1)
  output=$(echo "$result" | cut -d'|' -f2 | tail -n 1 | tr -d '\n' | xargs)
  exit_code=$(echo "$result" | cut -d'|' -f3)

  save_result "core" "tailcall" "$time" "$output" "$exit_code"
  print_result "tailcall" "$time" "$output" "$exit_code"

  if [ $REGRESSION_CHECK -eq 1 ]; then
    check_regression "core" "tailcall" "$time"
  fi
}

# ============================================
# Actor Benchmarks
# ============================================

run_actor_benchmarks() {
  print_header "Actor Concurrency"

  # Message throughput
  result=$(run_benchmark "actor/message-throughput" \
    "cd '$PROJECT_DIR' && '$TINYACTOR' run benchmark/actor/message-throughput.ta")
  time=$(echo "$result" | cut -d'|' -f1)
  output=$(echo "$result" | cut -d'|' -f2)
  exit_code=$(echo "$result" | cut -d'|' -f3)

  save_result "actor" "message-throughput" "$time" "$output" "$exit_code"
  print_result "message-throughput" "$time" "$output" "$exit_code"

  if [ $REGRESSION_CHECK -eq 1 ]; then
    check_regression "actor" "message-throughput" "$time"
  fi

  # Actor spawn
  result=$(run_benchmark "actor/spawn" \
    "cd '$PROJECT_DIR' && '$TINYACTOR' run benchmark/actor/spawn.ta")
  time=$(echo "$result" | cut -d'|' -f1)
  output=$(echo "$result" | cut -d'|' -f2)
  exit_code=$(echo "$result" | cut -d'|' -f3)

  save_result "actor" "spawn" "$time" "$output" "$exit_code"
  print_result "spawn" "$time" "$output" "$exit_code"

    if [ $REGRESSION_CHECK -eq 1 ]; then
    check_regression "actor" "spawn" "$time"
  fi

  # 1M actor spawn — memory/scale stress (run once; multi-worker steals
  # make it ~1s; RSS is the headline metric, extracted from time -l)
  result=$(run_benchmark "actor/spawn1m" \
    "cd '$PROJECT_DIR' && ${TIME_CMD}'$TINYACTOR' run benchmark/actor/spawn1m.ta 2>&1 | grep -E 'maximum resident set size|^1000000$' | tr '\n' ' '" 1)
  time=$(echo "$result" | cut -d'|' -f1)
  output=$(echo "$result" | cut -d'|' -f2)
  exit_code=$(echo "$result" | cut -d'|' -f3)

  save_result "actor" "spawn1m" "$time" "$output" "$exit_code"
  print_result "spawn1m" "$time" "$output" "$exit_code"

  if [ $REGRESSION_CHECK -eq 1 ]; then
    check_regression "actor" "spawn1m" "$time"
  fi

  # Scheduler fairness — 32 CPU-bound actors on the default worker count
  # (over-subscribed like real deployments; per-actor core placement noise
  # from P/E cores averages out across the actors each worker round-robins).
  result=$(run_benchmark "actor/fairness" \
    "cd '$PROJECT_DIR' && '$TINYACTOR' run benchmark/actor/fairness.ta" 1)
  time=$(echo "$result" | cut -d'|' -f1)
  output=$(echo "$result" | cut -d'|' -f2)
  exit_code=$(echo "$result" | cut -d'|' -f3)

  save_result "actor" "fairness" "$time" "$output" "$exit_code"
  print_result "fairness" "$time" "$output" "$exit_code"

  if [ $REGRESSION_CHECK -eq 1 ]; then
    check_regression "actor" "fairness" "$time"
  fi
}

# ============================================
# GC Benchmarks
# ============================================

run_gc_benchmarks() {
  print_header "Garbage Collection"

  # Tree allocation
  result=$(run_benchmark "gc/tree" \
    "cd '$PROJECT_DIR' && '$TINYACTOR' run benchmark/gc/tree.ta")
  time=$(echo "$result" | cut -d'|' -f1)
  output=$(echo "$result" | cut -d'|' -f2)
  exit_code=$(echo "$result" | cut -d'|' -f3)

  save_result "gc" "tree" "$time" "$output" "$exit_code"
  print_result "tree" "$time" "$output" "$exit_code"

  if [ $REGRESSION_CHECK -eq 1 ]; then
    check_regression "gc" "tree" "$time"
  fi

  # String churn
  result=$(run_benchmark "gc/string-churn" \
    "cd '$PROJECT_DIR' && '$TINYACTOR' run benchmark/gc/string-churn.ta")
  time=$(echo "$result" | cut -d'|' -f1)
  output=$(echo "$result" | cut -d'|' -f2)
  exit_code=$(echo "$result" | cut -d'|' -f3)

  save_result "gc" "string-churn" "$time" "$output" "$exit_code"
  print_result "string-churn" "$time" "$output" "$exit_code"

  if [ $REGRESSION_CHECK -eq 1 ]; then
    check_regression "gc" "string-churn" "$time"
  fi
}

# ============================================
# Compiler Benchmarks
# ============================================

run_compiler_benchmarks() {
  print_header "Compiler Performance"

  # Tokenizer
  result=$(run_benchmark "compiler/tokenizer" \
    "cd '$PROJECT_DIR' && '$TINYACTOR' build lib/tokenizer.ta /tmp/tokenizer_bench.tabc" 5)
  time=$(echo "$result" | cut -d'|' -f1)
  output=$(echo "$result" | cut -d'|' -f2 | tail -n 1 | tr -d '\n' | xargs)
  exit_code=$(echo "$result" | cut -d'|' -f3)

  # Check if build succeeded
  if [ "$exit_code" = "0" ] && [ -f /tmp/tokenizer_bench.tabc ]; then
    output="wrote /tmp/tokenizer_bench.tabc"
    rm -f /tmp/tokenizer_bench.tabc
  fi

  save_result "compiler" "tokenizer" "$time" "$output" "$exit_code"
  print_result "tokenizer" "$time" "$output" "$exit_code"

  if [ $REGRESSION_CHECK -eq 1 ]; then
    check_regression "compiler" "tokenizer" "$time"
  fi

  # Parser
  result=$(run_benchmark "compiler/parser" \
    "cd '$PROJECT_DIR' && '$TINYACTOR' build lib/parser.ta /tmp/parser_bench.tabc" 5)
  time=$(echo "$result" | cut -d'|' -f1)
  output=$(echo "$result" | cut -d'|' -f2 | tail -n 1 | tr -d '\n' | xargs)
  exit_code=$(echo "$result" | cut -d'|' -f3)

  # Check if build succeeded
  if [ "$exit_code" = "0" ] && [ -f /tmp/parser_bench.tabc ]; then
    output="wrote /tmp/parser_bench.tabc"
    rm -f /tmp/parser_bench.tabc
  fi

  save_result "compiler" "parser" "$time" "$output" "$exit_code"
  print_result "parser" "$time" "$output" "$exit_code"

  if [ $REGRESSION_CHECK -eq 1 ]; then
    check_regression "compiler" "parser" "$time"
  fi
}

# ============================================
# Main
# ============================================

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}TinyActor Benchmark Suite${NC}"
echo -e "${BLUE}========================================${NC}"
echo "Git commit: $GIT_COMMIT"
echo "Git date: $GIT_DATE"
echo ""

for cat in $CATEGORIES; do
  case "$cat" in
    core) run_core_benchmarks ;;
    actor) run_actor_benchmarks ;;
    gc) run_gc_benchmarks ;;
    compiler) run_compiler_benchmarks ;;
    *)
      echo -e "${YELLOW}Unknown category: $cat${NC}"
      ;;
  esac
done

print_summary

if [ $REGRESSION_CHECK -eq 1 ]; then
  echo -e "${CYAN}Regression check: ${GREEN}PASSED${NC}${NC}"
fi