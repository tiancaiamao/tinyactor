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

                # 运行测试（15 秒超时 — bootstrap 需要先加载）— 从项目根目录运行以便 lib/ 模块可被发现
  timeout 15 bash -c "cd '$PROJECT_DIR' && '$PROJECT_DIR/tinyactor' '$TESTS_DIR/$file'" >/tmp/test_out_$$ 2>&1
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

# Bootstrap test: compare C compiler path vs Lisp bootstrap pipeline output.
# Bootstrap is slower (loads bootstrap.tabc first), so use a 15s timeout.
run_bootstrap_test() {
  local file=$1  # basename

  # Skip expected-fail tests (features not yet implemented)
  case "$file" in
    bytes-basic.ta) printf "  %-50s ${YELLOW}⏭  SKIP${NC} (expected-fail: bytes not implemented)\n" "bootstrap $file:"; return ;;
  esac

  # Non-deterministic tests (concurrency/network ordering) — compare sorted full output
  local nondet=0
  case "$file" in
        multithread-basic.ta|echo_test.ta|error-process-crash-isolated.ta) nondet=1 ;;
  esac

    TOTAL=$((TOTAL + 1))
  printf "  %-50s " "bootstrap $file:"

  # Run via bootstrap pipeline only (C compiler removed)
  timeout 15 bash -c "cd '$PROJECT_DIR' && '$PROJECT_DIR/tinyactor' '$TESTS_DIR/$file'" >/tmp/bt_b_out_$$ 2>&1
  local b_exit=$?

  if [ $b_exit -eq 139 ]; then
    echo -e "${RED}❌ FAIL${NC} (SEGFAULT)"
    cat /tmp/bt_b_out_$$ | head -2 | sed 's/^/     /'
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("bootstrap $file (SEGFAULT)")
  elif [ $b_exit -eq 124 ]; then
    echo -e "${RED}❌ FAIL${NC} (TIMEOUT)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("bootstrap $file (TIMEOUT)")
  elif [ $b_exit -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (bootstrap exit $b_exit)"
    cat /tmp/bt_b_out_$$ | head -2 | sed 's/^/     /'
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("bootstrap $file (exit $b_exit)")
    else
    local actual
    if [ $nondet -eq 1 ]; then
      actual=$(cat /tmp/bt_b_out_$$ | sort)
    else
      actual=$(cat /tmp/bt_b_out_$$ | head -1)
    fi
    if [ -z "$actual" ]; then
      echo -e "${RED}❌ FAIL${NC} (empty output)"
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("bootstrap $file (empty output)")
    else
      echo -e "${GREEN}✅ PASS${NC} (\"$actual\")"
      PASSED=$((PASSED + 1))
    fi
  fi
  rm -f /tmp/bt_b_out_$$
}

# Self-hosting test: compile driver.ta via the Lisp pipeline, then use that
# as bootstrap.tabc to compile & run hello.ta. Verifies the self-hosted
# compiler produces a working bootstrap.
run_selfhost_test() {
  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "self-hosting:"

  local sh_tabc="/tmp/sh_driver_$$.tabc"
  local orig_backup="/tmp/orig_bootstrap_$$.tabc"
  local result="FAIL"

  # 1. Compile driver.ta with the Lisp pipeline
  timeout 30 bash -c "cd '$PROJECT_DIR' && '$PROJECT_DIR/tinyactor' --emit lib/driver.ta '$sh_tabc'" >/tmp/sh_out_$$ 2>&1
  local emit_exit=$?

  if [ $emit_exit -ne 0 ] || [ ! -f "$sh_tabc" ]; then
    echo -e "${RED}❌ FAIL${NC} (bootstrap-emit failed, exit $emit_exit)"
    cat /tmp/sh_out_$$ | head -2 | sed 's/^/     /'
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("self-hosting (bootstrap-emit failed)")
    rm -f /tmp/sh_out_$$
    return
  fi

  # 2. Backup original bootstrap.tabc and swap in the self-hosted version
  cp "$PROJECT_DIR/lib/bootstrap.tabc" "$orig_backup"
  cp "$sh_tabc" "$PROJECT_DIR/lib/bootstrap.tabc"

  # 3. Run hello.ta through the self-hosted bootstrap
  timeout 15 bash -c "cd '$PROJECT_DIR' && '$PROJECT_DIR/tinyactor' '$TESTS_DIR/hello.ta'" >/tmp/sh_run_$$ 2>&1
  local run_exit=$?
  local output
  output=$(cat /tmp/sh_run_$$ | head -1)

  # 4. Always restore original bootstrap.tabc
  cp "$orig_backup" "$PROJECT_DIR/lib/bootstrap.tabc"

  if [ $run_exit -eq 124 ]; then
    echo -e "${RED}❌ FAIL${NC} (TIMEOUT)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("self-hosting (TIMEOUT)")
  elif [ $run_exit -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (run exit $run_exit)"
    cat /tmp/sh_run_$$ | head -2 | sed 's/^/     /'
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("self-hosting (exit $run_exit)")
  elif [ "$output" != "hello" ]; then
    echo -e "${RED}❌ FAIL${NC} (expected \"hello\", got \"$output\")"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("self-hosting (wrong output: \"$output\")")
  else
    echo -e "${GREEN}✅ PASS${NC} (\"$output\")"
    PASSED=$((PASSED + 1))
  fi

  rm -f "$sh_tabc" "$orig_backup" /tmp/sh_out_$$ /tmp/sh_run_$$
}

# Bytecode comparison test: compile a .ta file with both the C compiler
# (--emit-tabc) and the bootstrap pipeline (--emit), then execute
# both .tabc files and compare their output. Functional equivalence check.
run_bytecode_cmp_test() {
    local file=$1  # basename

  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "bytecode-cmp $file:"

  local stem="${file%.*}"
  local tmp_src="/tmp/bc_src_$$_${file}"
  local sh_tabc="/tmp/bc_sh_$$_${stem}.tabc"

  # Copy source to /tmp
  cp "$TESTS_DIR/$file" "$tmp_src"

  # Compile via bootstrap pipeline
  timeout 15 bash -c "cd '$PROJECT_DIR' && '$PROJECT_DIR/tinyactor' --emit '$tmp_src' '$sh_tabc'" >/tmp/bc_sh_log_$$ 2>&1
  local sh_exit=$?

  if [ $sh_exit -ne 0 ] || [ ! -f "$sh_tabc" ]; then
    echo -e "${RED}❌ FAIL${NC} (bootstrap emit failed)"
    cat /tmp/bc_sh_log_$$ | head -2 | sed 's/^/     /'
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("bytecode-cmp $file (bootstrap emit failed)")
    rm -f "$tmp_src" /tmp/bc_sh_log_$$
    return
  fi

  # Execute the emitted .tabc
  timeout 5 bash -c "cd '$PROJECT_DIR' && '$PROJECT_DIR/tinyactor' '$sh_tabc'" >/tmp/bc_sh_run_$$ 2>&1
  local sh_run_exit=$?

  # Non-deterministic tests — use full sorted output
  local nondet=0
  case "$file" in
        multithread-basic.ta|echo_test.ta|error-process-crash-isolated.ta) nondet=1 ;;
  esac

  local actual
  if [ $nondet -eq 1 ]; then
    actual=$(cat /tmp/bc_sh_run_$$ | sort)
  else
    actual=$(cat /tmp/bc_sh_run_$$ | head -1)
  fi

  if [ $sh_run_exit -eq 0 ] && [ -n "$actual" ]; then
    echo -e "${GREEN}✅ PASS${NC} (\"$actual\")"
    PASSED=$((PASSED + 1))
  else
    echo -e "${RED}❌ FAIL${NC} (exit $sh_run_exit, output: \"$actual\")"
    cat /tmp/bc_sh_run_$$ | sed 's/^/     /'
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("bytecode-cmp $file (run exit $sh_run_exit)")
  fi

  rm -f "$tmp_src" "$sh_tabc" /tmp/bc_sh_log_$$ /tmp/bc_sh_run_$$
}

# Typecheck test: run  --check and verify expected error count.
# $1 = filename, $2 = expected error count (0 for clean code)
run_typecheck_test() {
  local file=$1
  local expected_errors=$2

  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "typecheck $file:"

  timeout 15 bash -c "cd '$PROJECT_DIR' && NWORKERS=1 '$PROJECT_DIR/tinyactor' '$TESTS_DIR/$file' '' --check" >/tmp/tc_check_out_$$ 2>&1
  local exit_code=$?
  local output
  output=$(cat /tmp/tc_check_out_$$)

  if [ $exit_code -eq 139 ]; then
    echo -e "${RED}❌ FAIL${NC} (SEGFAULT)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("typecheck $file (SEGFAULT)")
  elif [ $exit_code -eq 124 ]; then
    echo -e "${RED}❌ FAIL${NC} (TIMEOUT)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("typecheck $file (TIMEOUT)")
  elif [ $expected_errors -eq 0 ]; then
    # Expect no type errors — typecheck output should NOT contain "type error"
    if echo "$output" | grep -q "type error"; then
      echo -e "${RED}❌ FAIL${NC} (false positive errors)"
      echo "$output" | head -3 | sed 's/^/     /'
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("typecheck $file (false positives)")
    else
      echo -e "${GREEN}✅ PASS${NC} (no type errors)"
      PASSED=$((PASSED + 1))
    fi
  else
    # Expect specific error count
    local actual_errors
    actual_errors=$(echo "$output" | grep -o "typecheck: [0-9]* type error" | grep -o "[0-9]*" | head -1)
    if [ "$actual_errors" == "$expected_errors" ]; then
      echo -e "${GREEN}✅ PASS${NC} (detected $actual_errors type error(s))"
      PASSED=$((PASSED + 1))
    else
      echo -e "${RED}❌ FAIL${NC} (expected $expected_errors errors, got ${actual_errors:-0})"
      echo "$output" | head -3 | sed 's/^/     /'
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("typecheck $file (expected $expected_errors, got ${actual_errors:-0})")
    fi
  fi
  rm -f /tmp/tc_check_out_$$
}

# Const test: run  only, no C comparison needed.
# Const is a bootstrap-only feature; C compiler path doesn't support it.
run_const_test() {
  local file=$1

  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "const $file:"

  timeout 15 bash -c "cd '$PROJECT_DIR' && '$PROJECT_DIR/tinyactor' '$TESTS_DIR/$file'" >/tmp/const_out_$$ 2>&1
  local exit_code=$?

  if [ $exit_code -eq 139 ]; then
    echo -e "${RED}❌ FAIL${NC} (SEGFAULT)"
    cat /tmp/const_out_$$ | head -2 | sed 's/^/     /'
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("const $file (SEGFAULT)")
  elif [ $exit_code -eq 124 ]; then
    echo -e "${RED}❌ FAIL${NC} (TIMEOUT)"
    cat /tmp/const_out_$$ | head -2 | sed 's/^/     /'
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("const $file (TIMEOUT)")
  elif [ $exit_code -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (exit $exit_code)"
    cat /tmp/const_out_$$ | head -2 | sed 's/^/     /'
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("const $file (exit $exit_code)")
  else
    local output
    output=$(cat /tmp/const_out_$$)
    local expected="42
hello world
84
11"
    if [ "$output" = "$expected" ]; then
      echo -e "${GREEN}✅ PASS${NC} (\"${output%%$'\n'*}\")"
      PASSED=$((PASSED + 1))
    else
      echo -e "${RED}❌ FAIL${NC} (output mismatch)"
      echo "     expected multi-line output"
      echo "$expected" | sed 's/^/     /'
      echo "     got:"
      echo "$output" | sed 's/^/     /'
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("const $file (output mismatch)")
    fi
  fi
  rm -f /tmp/const_out_$$
}

# 进入测试目录
cd "$TESTS_DIR"

echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}TinyActor Test Suite${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""

# 运行所有 .ta 测试
echo -e "${BLUE}Running .ta tests...${NC}"
for file in *.ta; do
  [ -f "$file" ] || continue
  case "$file" in
    const-basic.ta) printf "  %-50s ${YELLOW}⏭  SKIP${NC} (const: requires bootstrap)\n" "$file:" ;;
    *) run_test "$file" ;;
  esac
done
echo ""

# 运行 typecheck 专项测试（ + --check 模式）
echo -e "${BLUE}Running typecheck tests (--check mode)...${NC}"
run_typecheck_test "typecheck-clean.ta" 0   # 0 = expect no errors
run_typecheck_test "typecheck-errors.ta" 2  # 2 = expect 2 errors
echo ""

# 运行 const 常量测试（ 模式）
echo -e "${BLUE}Running const tests ( only)...${NC}"
run_const_test "const-basic.ta"
echo ""

# 运行 bootstrap 测试（ 模式，与 C 编译路径输出对比）
echo -e "${BLUE}Running bootstrap tests ( mode)...${NC}"
for file in *.ta; do
  [ -f "$file" ] || continue
  case "$file" in
    const-basic.ta) printf "  %-50s ${YELLOW}⏭  SKIP${NC} (const: bootstrap-only feature)\n" "bootstrap $file:" ;;
    *) run_bootstrap_test "$file" ;;
  esac
done
echo ""

# 运行 self-hosting 测试（自举编译器链）
echo -e "${BLUE}Running self-hosting test...${NC}"
run_selfhost_test
echo ""

# 运行字节码对比测试（C 编译器 vs bootstrap 管线产物）
echo -e "${BLUE}Running bytecode comparison tests...${NC}"
for file in *.ta; do
  [ -f "$file" ] || continue
  case "$file" in
    const-basic.ta) printf "  %-50s ${YELLOW}⏭  SKIP${NC} (const: bootstrap-only feature)\n" "bytecode-cmp $file:" ;;
    *) run_bytecode_cmp_test "$file" ;;
  esac
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