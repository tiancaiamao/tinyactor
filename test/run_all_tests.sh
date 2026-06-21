#!/bin/bash
# TinyActor 测试运行脚本
# 运行所有测试并报告结果

PASSED=0
FAILED=0
TOTAL=0
FAILED_TESTS=()

# 颜色输出
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 获取脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TESTS_DIR="$SCRIPT_DIR/scripts"

run_test() {
  local file=$1

  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "$(basename $file):"

        # 运行测试（5 秒超时）— 从项目根目录运行以便 lib/ 模块可被发现
  timeout 5 bash -c "cd '$PROJECT_DIR' && '$PROJECT_DIR/tinyactor' '$TESTS_DIR/$file'" >/tmp/test_out_$$ 2>&1
  exit_code=$?

  output=$(cat /tmp/test_out_$$ | head -1)

  # 判断结果：只有 exit 0 且有非空输出才算 PASS
  if [ $exit_code -eq 139 ]; then
    echo -e "${RED}❌ FAIL${NC} (SEGFAULT, exit $exit_code)"
    cat /tmp/test_out_$$ | head -2 | sed 's/^/     /'
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("$file (SEGFAULT)")
  elif [ $exit_code -eq 124 ]; then
    echo -e "${RED}❌ FAIL${NC} (TIMEOUT, exit $exit_code)"
    cat /tmp/test_out_$$ | head -2 | sed 's/^/     /'
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("$file (TIMEOUT)")
  elif [ $exit_code -ne 0 ]; then
    # 其他非零退出码
    echo -e "${RED}❌ FAIL${NC} (exit $exit_code)"
    cat /tmp/test_out_$$ | head -2 | sed 's/^/     /'
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("$file")
  elif [ -z "$output" ]; then
    # exit 0 但无输出
    echo -e "${RED}❌ FAIL${NC} (NO OUTPUT, exit $exit_code)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("$file (NO OUTPUT)")
  else
    echo -e "${GREEN}✅ PASS${NC} (exit $exit_code) - \"$output\""
    PASSED=$((PASSED + 1))
  fi
  rm -f /tmp/test_out_$$
}

# 进入测试目录
cd "$TESTS_DIR"

echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}TinyActor Test Suite${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""

# 运行所有 .lisp 测试
echo -e "${BLUE}Running Lisp tests...${NC}"
for file in *.lisp; do
  [ -f "$file" ] && run_test "$file"
done
echo ""

# 运行所有 .ta 测试
echo -e "${BLUE}Running .ta tests...${NC}"
for file in *.ta; do
  [ -f "$file" ] && run_test "$file"
done
echo ""

# 输出汇总
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

# 如果有失败的测试，列出它们
if [ ${#FAILED_TESTS[@]} -gt 0 ]; then
  echo -e "${YELLOW}Failed tests:${NC}"
  for test in "${FAILED_TESTS[@]}"; do
    echo -e "  ${RED}❌${NC} $test"
  done
  echo ""
fi

echo -e "${BLUE}=========================================${NC}"

# 返回适当的退出码
if [ $FAILED -eq 0 ]; then
  exit 0
else
  exit 1
fi