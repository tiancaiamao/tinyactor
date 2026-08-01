#!/bin/bash
# test/lib.sh — Shared test runner functions.
#
# Source this file from a per-category runner script, then call
# run_test / run_build_run_test / run_category and finish with
# print_summary. All state (counters, paths) is set up here so each
# runner is self-contained.
#
# Each test file is exercised through multiple pipelines:
#   1. run:   tinyactor run <file>             (compile + run in one step)
#   2. build: tinyactor build <file> + tavm    (two-step: compile then run)
# These exercise different argument-handling paths in the shell script
# and verify the build pipeline produces runnable bytecode.

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Global counters (reset per category)
PASSED=0
FAILED=0
TOTAL=0
FAILED_TESTS=()

# Project paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Portable timeout: GNU timeout is not installed on macOS by default.
# Fall back to gtimeout (coreutils via Homebrew) or a shell implementation
# with the same semantics for the harness: exit 124 when the command exceeds
# the limit, otherwise the command's own exit status.
if ! command -v timeout >/dev/null 2>&1; then
  if command -v gtimeout >/dev/null 2>&1; then
    timeout() { gtimeout "$@"; }
  else
    timeout() {
      local secs=$1; shift
      "$@" &
      local pid=$!
      local waited=0
      while kill -0 "$pid" 2>/dev/null; do
        if [ "$waited" -ge "$secs" ]; then
          kill -TERM "$pid" 2>/dev/null
          wait "$pid" 2>/dev/null
          return 124
        fi
        sleep 1
        waited=$((waited + 1))
      done
      wait "$pid"
      return $?
    }
  fi
fi
TAVM_BIN="${TAVM:-$PROJECT_DIR/tavm}"
TINYACTOR="$PROJECT_DIR/tinyactor"
BOOTSTRAP="$PROJECT_DIR/lib/bootstrap.tabc"

# Skip list: tests known to be flaky (bash word-list matched per basename)
SKIP_LIST="echo_test.ta"

# is_skipped: return 0 if the given basename is in the skip list
is_skipped() {
  local base="$1"
  case " $SKIP_LIST " in
    *" $base "*) return 0 ;;
    *) return 1 ;;
  esac
}

# run_test: run a single .ta file via tinyactor run
run_test() {
  local file="$1"
  local base=$(basename "$file")
  local log=$(mktemp "${TMPDIR:-/tmp}/tr_${base%.ta}_$$XXXXXX.log")

  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "$base:"

  # Skip known-flaky network tests
  if is_skipped "$base"; then
    echo -e "${YELLOW}⏭  SKIP${NC} (flaky: port contention)"
    rm -f "$log"
    return
  fi

  # Retry on timeout for flaky network tests
  local max_attempts=3
  local exit_code=0
  for ((attempt=1; attempt<=max_attempts; attempt++)); do
    if command -v timeout >/dev/null 2>&1; then
      timeout 15 bash -c "cd '$PROJECT_DIR' && '$TINYACTOR' run '$file'" >"$log" 2>&1
    else
      bash -c "cd '$PROJECT_DIR' && '$TINYACTOR' run '$file'" >"$log" 2>&1
    fi
    exit_code=$?
    [ $exit_code -ne 124 ] && break
  done

  local output=$(head -1 "$log")
  rm -f "$log"

  if [ $exit_code -eq 139 ]; then
    echo -e "${RED}❌ FAIL${NC} (SEGFAULT)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("run $base (SEGFAULT)")
  elif [ $exit_code -eq 124 ]; then
    echo -e "${RED}❌ FAIL${NC} (TIMEOUT)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("run $base (TIMEOUT)")
  elif [ $exit_code -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (exit $exit_code)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("run $base")
  elif [ -z "$output" ]; then
    echo -e "${RED}❌ FAIL${NC} (NO OUTPUT)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("run $base (NO OUTPUT)")
  else
    echo -e "${GREEN}✅ PASS${NC} - \"$output\""
    PASSED=$((PASSED + 1))
  fi
}

# run_build_run_test: build .ta to .tabc then run with bare tavm.
# The build step goes through tinyactor (which honours $TAVM internally),
# the bare run step uses $TAVM_BIN directly so sanitizer builds
# (TAVM=./tavm_asan) are honoured in both steps.
run_build_run_test() {
  local file="$1"
  local base=$(basename "$file")
  local tabc=$(mktemp "${TMPDIR:-/tmp}/br_${base%.ta}_$$XXXXXX.tabc")
  local log=$(mktemp "${TMPDIR:-/tmp}/br_${base%.ta}_$$XXXXXX.log")

  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "build+run $base:"

  # Skip known-flaky network tests
  if is_skipped "$base"; then
    echo -e "${YELLOW}⏭  SKIP${NC} (flaky: port contention)"
    rm -f "$tabc" "$log"
    return
  fi

  # Build step
  if command -v timeout >/dev/null 2>&1; then
    timeout 15 bash -c "cd '$PROJECT_DIR' && '$TINYACTOR' build '$file' '$tabc'" >"$log" 2>&1
  else
    bash -c "cd '$PROJECT_DIR' && '$TINYACTOR' build '$file' '$tabc'" >"$log" 2>&1
  fi
  local build_exit=$?

  if [ $build_exit -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (build failed, exit $build_exit)"
    head -2 "$log" | sed 's/^/     /'
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("build+run $base (build failed)")
    rm -f "$tabc" "$log"
    return
  fi

  # Run step with bare tavm (in project dir so file-relative paths work)
  if command -v timeout >/dev/null 2>&1; then
    timeout 15 bash -c "cd '$PROJECT_DIR' && '$TAVM_BIN' '$tabc'" >"$log" 2>&1
  else
    bash -c "cd '$PROJECT_DIR' && '$TAVM_BIN' '$tabc'" >"$log" 2>&1
  fi
  local run_exit=$?
  local run_output=$(head -1 "$log")

  if [ $run_exit -eq 139 ]; then
    echo -e "${RED}❌ FAIL${NC} (SEGFAULT)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("build+run $base (SEGFAULT)")
  elif [ $run_exit -eq 124 ]; then
    echo -e "${RED}❌ FAIL${NC} (TIMEOUT)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("build+run $base (TIMEOUT)")
  elif [ $run_exit -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (run exit $run_exit)"
    head -2 "$log" | sed 's/^/     /'
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("build+run $base")
  elif [ -z "$run_output" ]; then
    echo -e "${RED}❌ FAIL${NC} (NO OUTPUT)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("build+run $base (NO OUTPUT)")
  else
    echo -e "${GREEN}✅ PASS${NC} - \"$run_output\""
    PASSED=$((PASSED + 1))
  fi

  rm -f "$tabc" "$log"
}

# run_category: run all .ta files in a directory (run + build+run each)
run_category() {
  local cat_name="$1"
  local cat_dir="$2"
  echo -e "${BLUE}[${cat_name}]${NC}"
  local all_files=("$cat_dir"/*.ta)
  for f in "${all_files[@]}"; do
    [ -f "$f" ] || continue
    run_test "$f"
    run_build_run_test "$f"
  done
  echo ""
}

# print_summary: print test results
print_summary() {
  echo -e "${BLUE}=========================================${NC}"
  echo -e "${BLUE}Test Summary${NC}"
  echo -e "${BLUE}=========================================${NC}"
  echo "Total:   $TOTAL"
  echo -e "Passed:  ${GREEN}$PASSED${NC}"
  if [ $FAILED -gt 0 ]; then
    echo -e "Failed:  ${RED}$FAILED${NC}"
  else
    echo "Failed:  $FAILED"
  fi
  echo ""
  if [ ${#FAILED_TESTS[@]} -gt 0 ]; then
    echo -e "${YELLOW}Failed tests:${NC}"
    for t in "${FAILED_TESTS[@]}"; do
      echo -e "  ${RED}❌${NC} $t"
    done
    echo ""
  fi
}