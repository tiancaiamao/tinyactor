#!/bin/bash
# test/run_bootstrap_tests.sh — bootstrap fixed-point + self-hosting tests.
#
# run_fixed_point_test: rebuild lib/bootstrap.tabc from lib/driver.ta and
#   verify the output is bit-identical to the committed bootstrap.
# run_selfhost_test: rebuild the bootstrap, use it to compile hello.ta,
#   then run the compiled bytecode and check it prints "hello".
source "$(dirname "$0")/lib.sh"

# ============================================================
# Bootstrap fixed point: rebuild bootstrap.tabc, verify bit-identical
# ============================================================
run_fixed_point_test() {
  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "bootstrap fixed point:"

  local rebuilt="/tmp/fp_$$.tabc"
  local log="/tmp/fp_$$.log"
  timeout 300 bash -c "cd '$PROJECT_DIR' && '$TINYACTOR' build lib/driver.ta '$rebuilt'" >"$log" 2>&1

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
  timeout 300 bash -c "cd '$PROJECT_DIR' && '$TINYACTOR' build lib/driver.ta '$sh_tabc'" >"$log" 2>&1
  if [ $? -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (rebuild failed)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("self-hosting (rebuild failed)")
    rm -f "$sh_tabc" "$log"
    return
  fi

  # Compile hello.ta with the rebuilt bootstrap
  timeout 15 bash -c "cd '$PROJECT_DIR' && '$TAVM_BIN' '$sh_tabc' test/basic/hello.ta '$sh_hello'" >"$log2" 2>&1
  if [ $? -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (rebuilt compiler can't compile)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("self-hosting (compile failed)")
    rm -f "$sh_tabc" "$sh_hello" "$log" "$log2"
    return
  fi

  # Run the compiled hello (in project dir for consistent CWD)
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

  rm -f "$sh_tabc" "$sh_hello" "$log" "$log2" "$log3"
}

# ============================================================
# Main
# ============================================================
echo -e "${BLUE}[Bootstrap]${NC}"
run_fixed_point_test
run_selfhost_test
echo ""
print_summary
exit $([ $FAILED -eq 0 ])