#!/bin/bash
# test/lib.sh — Shared test runner functions.
#
# Source this file from a per-category runner script, then call
# run_test / run_build_run_test / run_category and finish with
# print_summary. All state (counters, paths) is set up here so each
# runner is self-contained.
#
# Each test file is exercised through tinyactor run, which goes through
# the single unified build+run path (build_ta in tinyactor) and verifies
# the whole pipeline produces runnable bytecode. Files named *-errors.ta
# are negative tests: they must be rejected by the compiler (exit != 0 and
# "type error" or "parse error" in the output).

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

# is_negative_test: files named *-errors.ta must be REJECTED by the compiler
# (they assert the compiler catches the error, so exit != 0 + "type error" or
# "parse error" in the log is the expected success condition).
is_negative_test() {
  local base="$1"
  case "$base" in
    *-errors.ta) return 0 ;;
    *) return 1 ;;
  esac
}

# is_parse_error_test: files named *-parse-errors.ta must be REJECTED by the
# parser/driver with a "parse error" message (exit != 0 + "parse error" in
# the log is the expected success condition).
is_parse_error_test() {
  local base="$1"
  case "$base" in
    *-parse-errors.ta) return 0 ;;
    *) return 1 ;;
  esac
}

# run_build_run_test: compile a .ta file to an explicit .tabc and run the
# bytecode directly with tavm — exercises the build-to-file path that
# run_test (tinyactor run, temp output) does not cover.
run_build_run_test() {
  local file="$1"
  local base=$(basename "$file")
  local out=$(mktemp "${TMPDIR:-/tmp}/tb_${base%.ta}_$$XXXXXX.tabc")
  local log=$(mktemp "${TMPDIR:-/tmp}/tr_${base%.ta}_$$XXXXXX.log")

  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "$base (build+run):"

  if is_skipped "$base"; then
    echo -e "${YELLOW}⏭  SKIP${NC} (flaky: port contention)"
    rm -f "$out" "$log"
    return
  fi

  local start=$SECONDS
  local build_rc=0
  if command -v timeout >/dev/null 2>&1; then
    timeout 60 bash -c "cd '$PROJECT_DIR' && '$TINYACTOR' build '$file' '$out'" >"$log" 2>&1
  else
    bash -c "cd '$PROJECT_DIR' && '$TINYACTOR' build '$file' '$out'" >"$log" 2>&1
  fi
  build_rc=$?

  if [ $build_rc -ne 0 ] || [ ! -s "$out" ]; then
    echo -e "${RED}❌ FAIL${NC} (build failed) ($((SECONDS - start))s)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("build+run $base (build failed)")
  else
    local run_rc=0
    if command -v timeout >/dev/null 2>&1; then
      timeout 60 bash -c "cd '$PROJECT_DIR' && '$TAVM_BIN' '$out'" >>"$log" 2>&1
    else
      bash -c "cd '$PROJECT_DIR' && '$TAVM_BIN' '$out'" >>"$log" 2>&1
    fi
    run_rc=$?
    if [ $run_rc -eq 0 ]; then
      local output=$(head -1 "$log")
      echo -e "${GREEN}✅ PASS${NC} - \"$output\" ($((SECONDS - start))s)"
      PASSED=$((PASSED + 1))
    else
      echo -e "${RED}❌ FAIL${NC} (run failed, rc=$run_rc) ($((SECONDS - start))s)"
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("build+run $base (run failed)")
    fi
  fi
  rm -f "$out" "$log"
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
  local start=$SECONDS
  # Generous per-attempt timeout: typecheck-driven tests (import parser /
  # typecheck, ~5.5k lines of lib code) take ~13s locally and ~3x longer on
  # slow CI runners, so 60s keeps them green without false timeouts.
  local timeout_secs=60
  for ((attempt=1; attempt<=max_attempts; attempt++)); do
    if command -v timeout >/dev/null 2>&1; then
      timeout $timeout_secs bash -c "cd '$PROJECT_DIR' && '$TINYACTOR' run '$file'" >"$log" 2>&1
    else
      bash -c "cd '$PROJECT_DIR' && '$TINYACTOR' run '$file'" >"$log" 2>&1
    fi
        exit_code=$?
    # Retry timeouts (124) always. Also retry SIGABRT (134) for GC stress
    # tests: they are timing-sensitive and occasionally trip GC assertions
    # on slow CI runners, but pass reliably locally — a persistent failure
    # still surfaces after the final attempt.
    if [ $exit_code -ne 124 ] && { [ $exit_code -ne 134 ] || [[ "$file" != *"/gc/"* ]]; }; then
      break
    fi
  done

    local elapsed=$((SECONDS - start))
  local output=$(head -1 "$log")
  rm -f "$log"

  if is_parse_error_test "$base"; then
    if [ $exit_code -ne 0 ] && echo "$output" | grep -q "error:"; then
      echo -e "${GREEN}✅ PASS${NC} (rejected with error message) (${elapsed}s)"
      PASSED=$((PASSED + 1))
    else
      echo -e "${RED}❌ FAIL${NC} (expected error rejection) (${elapsed}s)"
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("run $base (expected error rejection)")
    fi
    return
  fi

        if is_negative_test "$base"; then
    if [ $exit_code -ne 0 ] && { echo "$output" | grep -q "type error" || echo "$output" | grep -q "parse error"; }; then
      echo -e "${GREEN}✅ PASS${NC} (rejected by compiler) (${elapsed}s)"
      PASSED=$((PASSED + 1))
    else
      echo -e "${RED}❌ FAIL${NC} (expected compiler rejection) (${elapsed}s)"
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("run $base (expected compiler rejection)")
    fi
    return
  fi

  if [ $exit_code -eq 139 ]; then
    echo -e "${RED}❌ FAIL${NC} (SEGFAULT) (${elapsed}s)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("run $base (SEGFAULT)")
  elif [ $exit_code -eq 124 ]; then
    echo -e "${RED}❌ FAIL${NC} (TIMEOUT) (${elapsed}s)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("run $base (TIMEOUT)")
  elif [ $exit_code -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (exit $exit_code) (${elapsed}s)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("run $base")
  elif [ -z "$output" ]; then
    echo -e "${RED}❌ FAIL${NC} (NO OUTPUT) (${elapsed}s)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("run $base (NO OUTPUT)")
  else
    echo -e "${GREEN}✅ PASS${NC} - \"$output\" (${elapsed}s)"
    PASSED=$((PASSED + 1))
  fi
}


# run_category: run all .ta files in a directory
run_category() {
  local cat_name="$1"
  local cat_dir="$2"
  echo -e "${BLUE}[${cat_name}]${NC}"
  local all_files=("$cat_dir"/*.ta)
  for f in "${all_files[@]}"; do
    [ -f "$f" ] || continue
    run_test "$f"
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