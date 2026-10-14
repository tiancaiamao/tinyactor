#!/bin/bash
# test/run_module_tests.sh — run the module system tests
source "$(dirname "$0")/lib.sh"

# 插桩构建（COV/ASAN/TSAN：MOD_SUFFIX 非空）下 json-mpc 从 ~18s（plain）
# 涨到数十秒乃至 TSAN 的 169s（见 Makefile test-tsan 历史注释）——60s 默认
# 预算在慢 runner + -j4 争抢下边际超时（PR #342 coverage-c 曾挂死）。统一
# 在此加宽：脚本知道构建配置，不必每个 Makefile target 单独 override。
case "${MOD_SUFFIX:-}" in
  _*) TEST_TIMEOUT=$(( TEST_TIMEOUT < 300 ? 300 : TEST_TIMEOUT )) ;;
esac

run_category "Module" "$SCRIPT_DIR/module"
print_summary
exit $([ $FAILED -eq 0 ])