#!/bin/bash
# test/run_bootstrap_tests.sh — bootstrap fixed-point + self-hosting tests.
#
# Rebuild lib/bootstrap.tabc from lib/driver.ta ONCE, then verify:
#   1. fixed point: the rebuild is bit-identical to the committed bootstrap
#   2. self-hosting: the rebuilt compiler can compile+run hello.ta
#
# Note: rebuilding driver.ta runs the full typecheck over lib/ and takes
# a couple of minutes, so the two checks share a single rebuild.
source "$(dirname "$0")/lib.sh"

run_bootstrap_tests() {
  # Single rebuild shared by both checks below
  local rebuilt="/tmp/fp_$$.tabc"
  local log="/tmp/fp_$$.log"
  local t0=$SECONDS
  timeout 300 bash -c "cd '$PROJECT_DIR' && '$TINYACTOR' build lib/driver.ta '$rebuilt'" >"$log" 2>&1
  local build_exit=$?
  local rebuild_secs=$((SECONDS - t0))

  # 1. Fixed point: rebuilt must be bit-identical to the committed bootstrap
  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "bootstrap fixed point:"
  if [ $build_exit -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (rebuild failed in ${rebuild_secs}s)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("bootstrap fixed point (rebuild failed)")
  elif cmp -s "$rebuilt" "$BOOTSTRAP"; then
    echo -e "${GREEN}✅ PASS${NC} (bit-identical, rebuild ${rebuild_secs}s)"
    PASSED=$((PASSED + 1))
  else
    echo -e "${RED}❌ FAIL${NC} (mismatch, rebuild ${rebuild_secs}s)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("bootstrap fixed point (mismatch)")
  fi

  # 2. Self-hosting: use the rebuilt compiler to compile+run hello.ta
  local sh_hello="/tmp/sh_hello_$$.tabc"
  local log2="/tmp/sh2_$$.log"
  local log3="/tmp/sh3_$$.log"
  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "self-hosting:"
  if [ $build_exit -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (rebuild failed)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("self-hosting (rebuild failed)")
  else
    timeout 15 bash -c "cd '$PROJECT_DIR' && '$TAVM_BIN' '$rebuilt' test/basic/hello.ta '$sh_hello'" >"$log2" 2>&1
    if [ $? -ne 0 ]; then
      echo -e "${RED}❌ FAIL${NC} (rebuilt compiler can't compile)"
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("self-hosting (compile failed)")
    else
      timeout 3 bash -c "cd '$PROJECT_DIR' && '$TAVM_BIN' '$sh_hello'" >"$log3" 2>&1
      local run_exit=$?
      local run_output=$(head -1 "$log3")
      if [ "$run_output" == "hello" ] && [ $run_exit -eq 0 ]; then
        echo -e "${GREEN}✅ PASS${NC} (\"$run_output\")"
        PASSED=$((PASSED + 1))
      else
        echo -e "${RED}❌ FAIL${NC} (output \"$run_output\", exit $run_exit)"
        FAILED=$((FAILED + 1))
        FAILED_TESTS+=("self-hosting (run failed)")
      fi
    fi
  fi

  rm -f "$rebuilt" "$log" "$sh_hello" "$log2" "$log3"
}

# ============================================================
# Main
# ============================================================
echo -e "${BLUE}[Bootstrap]${NC}"
run_bootstrap_tests
echo ""
print_summary
exit $([ $FAILED -eq 0 ])