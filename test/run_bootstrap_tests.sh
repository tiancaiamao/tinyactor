#!/bin/bash
# test/run_bootstrap_tests.sh — lisp 种子固定点 + 自举测试（7c-1 换锚）。
#
# 旧链（tavm + lib/bootstrap.tabc 自举，重编 build.ta 后与提交产物 cmp）随
# codegen/.tabc 世界删除；新自举锚 = 入库的 vm-demo/lisp/boot/backend_driver.tabc。
# 用入库种子把 driver 全闭包重编译一次，然后验证：
#   1. fixed point: 重建产物与入库种子逐字节一致（源码动了没跑
#      `make bootstrap` 更新种子 → 这里红，与旧 gate 语义一致）
#   2. self-hosting: 用重建产物编译 hello.ta，lispvm 跑出 "hello"
#
# 重建 = driver 全闭包 typecheck（~12s 本地，CI ~3x），两项检查共享一次重建。
source "$(dirname "$0")/lib.sh"

# Optional SKIP_BOOTSTRAP=1 skips this category (kept for local debugging
# convenience; the rebuild is fast since the typecheck perf fix).
if [ "${SKIP_BOOTSTRAP:-0}" = "1" ]; then
  echo -e "${YELLOW}[Bootstrap] SKIPPED (SKIP_BOOTSTRAP=1)${NC}"
  echo ""
  exit 0
fi

run_bootstrap_tests() {
  local seed="$PROJECT_DIR/vm-demo/lisp/boot/backend_driver.tabc"
  local driver_src="$PROJECT_DIR/vm-demo/lisp/backend_driver.ta"
  local lispvm="${LISPVM:-$PROJECT_DIR/lispvm}"
  # Single rebuild shared by both checks below
  local rebuilt="/tmp/fp_$$.tabc"
  local log="/tmp/fp_$$.log"
  local t0=$SECONDS
  timeout 300 "$lispvm" -q "$seed" "$driver_src" "$rebuilt" "" >"$log" 2>&1
  local build_exit=$?
  local rebuild_secs=$((SECONDS - t0))

  # 1. Fixed point: rebuilt must be bit-identical to the committed seed
  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "backend_driver.tabc fixed point:"
  if [ $build_exit -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (rebuild failed in ${rebuild_secs}s)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("backend_driver.tabc fixed point (rebuild failed)")
  elif cmp -s "$rebuilt" "$seed"; then
    echo -e "${GREEN}✅ PASS${NC} (bit-identical, rebuild ${rebuild_secs}s)"
    PASSED=$((PASSED + 1))
  else
    echo -e "${RED}❌ FAIL${NC} (mismatch, rebuild ${rebuild_secs}s)"
    echo "       seed vs rebuilt differ — run 'make bootstrap' and commit the seed"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("backend_driver.tabc fixed point (mismatch)")
  fi

  # 2. Self-hosting: use the rebuilt driver to compile+run hello.ta
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
    timeout 60 "$lispvm" -q "$rebuilt" "$PROJECT_DIR/test/basic/hello.ta" "$sh_hello" "" >"$log2" 2>&1
    if [ $? -ne 0 ]; then
      echo -e "${RED}❌ FAIL${NC} (rebuilt driver can't compile)"
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("self-hosting (compile failed)")
    else
      timeout 3 "$lispvm" -q "$sh_hello" >"$log3" 2>&1
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