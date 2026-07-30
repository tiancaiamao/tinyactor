#!/bin/bash
# TinyActor test runner — runs all TA tests via the tinyactor CLI.
#
# Each .ta file is tested through multiple pipelines:
#   1. run:   tinyactor run <file>             (compile + run in one step)
#   2. build: tinyactor build <file> + tavm    (two-step: compile then run)
# These exercise different argument-handling paths in the shell script
# and verify the build pipeline produces runnable bytecode.

PASSED=0
FAILED=0
TOTAL=0
FAILED_TESTS=()

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TESTS_DIR="$SCRIPT_DIR/scripts"
TAVM="$PROJECT_DIR/tavm"
BOOTSTRAP="$PROJECT_DIR/lib/bootstrap.tabc"

# ============================================================
# Test a signle .ta file via tinyactor run
# ============================================================
run_test() {
  local file="$1"
  local base=$(basename "$file")
  local log="/tmp/tr_${base%.ta}_$$.log"

    TOTAL=$((TOTAL + 1))
  printf "  %-50s " "$base:"

  # Retry on timeout for flaky network tests
  local max_attempts=3
  case "$base" in
    echo_test.ta)
      # Known flaky: port 8091 contention in CI
      echo -e "${YELLOW}⏭  SKIP${NC} (flaky: port contention)"
      return
      ;;
  esac

  local exit_code=0
  for ((attempt=1; attempt<=max_attempts; attempt++)); do
    timeout 15 bash -c "cd '$PROJECT_DIR' && ./tinyactor run '$TESTS_DIR/$file'" >"$log" 2>&1
    exit_code=$?
    [ $exit_code -ne 124 ] && break
  done

  local output=$(cat "$log" | head -1)
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

# ============================================================
# Build .ta to .tabc, then run with bare tavm
# ============================================================
run_build_run_test() {
  local file="$1"
  local base=$(basename "$file")
  local tabc="/tmp/br_${base%.ta}_$$.tabc"
  local log="/tmp/br_${base%.ta}_$$.log"

    TOTAL=$((TOTAL + 1))
  printf "  %-50s " "build+run $base:"

  # Skip flaky network tests
  case "$base" in
    echo_test.ta)
      echo -e "${YELLOW}⏭  SKIP${NC} (flaky: port contention)"
      return
      ;;
  esac

    # Build step
  timeout 15 bash -c "cd '$PROJECT_DIR' && ./tinyactor build '$TESTS_DIR/$file' '$tabc'" >"$log" 2>&1
  local build_exit=$?

  if [ $build_exit -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (build failed, exit $build_exit)"
    cat "$log" | head -2 | sed 's/^/     /'
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("build+run $base (build failed)")
    rm -f "$tabc" "$log"
    return
  fi

    # Run step with bare tavm (in project dir so file-relative paths work)
  timeout 15 bash -c "cd '$PROJECT_DIR' && '$TAVM' '$tabc'" >"$log" 2>&1
  local run_exit=$?
  local run_output=$(cat "$log" | head -1)

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
    cat "$log" | head -2 | sed 's/^/     /'
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

# ============================================================
# Bootstrap fixed point: rebuild bootstrap.tabc, verify bit-identical
# ============================================================
run_fixed_point_test() {
  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "bootstrap fixed point:"

  local rebuilt="/tmp/fp_$$.tabc"
  local log="/tmp/fp_$$.log"
  timeout 30 bash -c "cd '$PROJECT_DIR' && ./tinyactor build lib/driver.ta '$rebuilt'" >"$log" 2>&1

  if [ $? -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (rebuild failed)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("bootstrap fixed point (rebuild failed)")
  elif cmp -s "$rebuilt" "$BOOTSTRAP"; then
    echo -e "${GREEN}✅ PASS${NC} (bit-identical)"
    PASSED=$((PASSED + 1))
  else
    echo -e "${RED}❌ FAIL${NC} (mismatch)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("bootstrap fixed point (mismatch)")
  fi

  rm -f "$rebuilt" "$log"
}

# ============================================================
# Self-hosting: rebuild bootstrap, use it to compile+run hello.ta
# ============================================================
run_selfhost_test() {
  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "self-hosting:"

  local sh_tabc="/tmp/sh_$$.tabc"
  local sh_hello="/tmp/sh_hello_$$.tabc"
  local log="/tmp/sh_$$.log"
  local log2="/tmp/sh2_$$.log"
  local log3="/tmp/sh3_$$.log"

  # Rebuild bootstrap
  timeout 30 bash -c "cd '$PROJECT_DIR' && ./tinyactor build lib/driver.ta '$sh_tabc'" >"$log" 2>&1
  if [ $? -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (rebuild failed)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("self-hosting (rebuild failed)")
    rm -f "$sh_tabc" "$log"
    return
  fi

  # Compile hello.ta with the rebuilt bootstrap
  timeout 15 bash -c "cd '$PROJECT_DIR' && ./tavm '$sh_tabc' test/scripts/hello.ta '$sh_hello'" >"$log2" 2>&1
  if [ $? -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (rebuilt compiler can't compile)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("self-hosting (compile failed)")
    rm -f "$sh_tabc" "$sh_hello" "$log" "$log2"
    return
  fi

    # Run the compiled hello (in project dir for consistent CWD)
  timeout 3 bash -c "cd '$PROJECT_DIR' && '$TAVM' '$sh_hello'" >"$log3" 2>&1
  local run_exit=$?
  local run_output=$(cat "$log3" | head -1)

  if [ "$run_output" == "hello" ] && [ $run_exit -eq 0 ]; then
    echo -e "${GREEN}✅ PASS${NC} (\"$run_output\")"
    PASSED=$((PASSED + 1))
  else
    echo -e "${RED}❌ FAIL${NC} (output \"$run_output\", exit $run_exit)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("self-hosting (run failed)")
  fi

  rm -f "$sh_tabc" "$sh_hello" "$log" "$log2" "$log3"
}

# ============================================================
# Main
# ============================================================
cd "$TESTS_DIR"

echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}TinyActor Test Suite${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""

ALL_TESTS=(*.ta)

echo -e "${BLUE}[1/4] Direct run tests (tinyactor run) ...${NC}"
for f in "${ALL_TESTS[@]}"; do
  [ -f "$f" ] && run_test "$f"
done
echo ""

echo -e "${BLUE}[2/4] Build + run tests (tinyactor build + tavm) ...${NC}"
for f in "${ALL_TESTS[@]}"; do
  [ -f "$f" ] && run_build_run_test "$f"
done
echo ""

echo -e "${BLUE}[3/4] Bootstrap tests ...${NC}"
run_fixed_point_test
run_selfhost_test
echo ""

# Summary
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

echo -e "${BLUE}=========================================${NC}"

exit $([ $FAILED -eq 0 ])