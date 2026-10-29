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

# 模块 flag-mix 门（#415）：模块 dylib 的编译 flag 跟目标文件名后缀走，
# 不随 ambient 配置漂移。DRIVER_DEPS/TEST_DEPS 把四个变体全列在依赖里，
# plain make 建 _asan/_tsan 变体时曾继承 plain MOD_CFLAGS——产出「新鲜但
# 无插桩」的 dylib，随后的 ASAN=1 行按 mtime 跳过重建，那一轮 C 模块 ASan
# 覆盖静默归零。断言用 `make -Bn` 干跑：不碰任何产物、不受本地树 mtime
# 新旧影响，plain 树上任何时点都可跑（issue 实测 nm 计数：错配置 0 个
# undefined-asan 符号，正确 11 个）。
check_module_flag_mix() {
    local ext="${HTTP_EXT:?HTTP_EXT 未由 Makefile 导出}"
  local dry plain line fail=""
  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "module flag-mix gate (#415):"
  # (a) plain 配置下后缀定 flag：_asan 带 -fsanitize=address，_tsan 带
  #     -fsanitize=thread，plain 两者皆无。
  dry=$(make -Bn -C "$PROJECT_DIR" \
    "lib/sexp_asan.$ext" "lib/sexp_tsan.$ext" "lib/sexp.$ext" 2>/dev/null)
  line=$(echo "$dry" | grep -F -- "-o lib/sexp_asan.$ext")
  case "$line" in
    *-fsanitize=address*) ;;
    *) fail="_asan 变体未带 -fsanitize=address（plain 配置建出无插桩赝品）" ;;
  esac
  if [ -z "$fail" ]; then
    line=$(echo "$dry" | grep -F -- "-o lib/sexp_tsan.$ext")
    case "$line" in
      *-fsanitize=thread*) ;;
      *) fail="_tsan 变体未带 -fsanitize=thread（plain 配置建出无插桩赝品）" ;;
    esac
  fi
  if [ -z "$fail" ]; then
    line=$(echo "$dry" | grep -F -- "-o lib/sexp.$ext")
    case "$line" in
      *-fsanitize*) fail="plain 变体混入了 -fsanitize flag" ;;
    esac
  fi
  # (b) 反向钉：ASAN=1 配置下 plain 变体不得被插桩（否则随后的 plain VM
  #     dlopen 它直接 abort——「Interceptors are not working」）。
  if [ -z "$fail" ]; then
    plain=$(make -Bn -C "$PROJECT_DIR" ASAN=1 "lib/sexp.$ext" 2>/dev/null)
    line=$(echo "$plain" | grep -F -- "-o lib/sexp.$ext")
    case "$line" in
      *-fsanitize*) fail="ASAN=1 配置把 plain 变体也插桩了（plain VM 会 dlopen 失败）" ;;
    esac
  fi
  if [ -z "$fail" ]; then
    echo -e "${GREEN}✅ PASS${NC} (flags follow suffix, plain 干净)"
    PASSED=$((PASSED + 1))
  else
    echo -e "${RED}❌ FAIL${NC} ($fail)"
    echo "$dry" | sed 's/^/         /' | head -4
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("module flag-mix gate ($fail)")
  fi
}

check_module_flag_mix

run_category "Module" "$SCRIPT_DIR/module"
print_summary
exit $([ $FAILED -eq 0 ])