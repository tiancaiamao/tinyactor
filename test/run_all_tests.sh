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
    multithread-basic.ta|echo_test.ta) nondet=1 ;;
  esac

  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "bootstrap $file:"

  # Expected output: C compiler path
  timeout 5 bash -c "cd '$PROJECT_DIR' && '$PROJECT_DIR/tinyactor' '$TESTS_DIR/$file'" >/tmp/bt_c_out_$$ 2>&1
  local c_exit=$?

  # Actual output: bootstrap (Lisp) pipeline
  timeout 15 bash -c "cd '$PROJECT_DIR' && '$PROJECT_DIR/tinyactor' --bootstrap '$TESTS_DIR/$file'" >/tmp/bt_b_out_$$ 2>&1
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
    local expected actual
    if [ $nondet -eq 1 ]; then
      expected=$(cat /tmp/bt_c_out_$$ | sort)
      actual=$(cat /tmp/bt_b_out_$$ | sort)
    else
      expected=$(cat /tmp/bt_c_out_$$ | head -1)
      actual=$(cat /tmp/bt_b_out_$$ | head -1)
    fi
    if [ "$expected" != "$actual" ]; then
      echo -e "${RED}❌ FAIL${NC} (output mismatch)"
      echo "     expected: \"$expected\""
      echo "     actual:   \"$actual\""
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("bootstrap $file (output mismatch)")
    else
      echo -e "${GREEN}✅ PASS${NC} (\"$actual\")"
      PASSED=$((PASSED + 1))
    fi
  fi
  rm -f /tmp/bt_c_out_$$ /tmp/bt_b_out_$$
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
  timeout 30 bash -c "cd '$PROJECT_DIR' && '$PROJECT_DIR/tinyactor' --bootstrap-emit lib/driver.ta '$sh_tabc'" >/tmp/sh_out_$$ 2>&1
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
  timeout 15 bash -c "cd '$PROJECT_DIR' && '$PROJECT_DIR/tinyactor' --bootstrap '$TESTS_DIR/hello.ta'" >/tmp/sh_run_$$ 2>&1
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
# (--emit-tabc) and the bootstrap pipeline (--bootstrap-emit), then execute
# both .tabc files and compare their output. Functional equivalence check.
run_bytecode_cmp_test() {
  local file=$1  # basename

  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "bytecode-cmp $file:"

  local stem="${file%.*}"
  local tmp_src="/tmp/bc_src_$$_${file}"
  local c_tabc="/tmp/bc_c_$$_${stem}.tabc"
  local sh_tabc="/tmp/bc_sh_$$_${stem}.tabc"

  # Copy source to /tmp so --emit-tabc writes there
  cp "$TESTS_DIR/$file" "$tmp_src"

  # 1. Compile via C compiler path
  timeout 5 bash -c "cd '$PROJECT_DIR' && '$PROJECT_DIR/tinyactor' '$tmp_src' --emit-tabc" >/tmp/bc_c_log_$$ 2>&1
  local c_exit=$?
  # --emit-tabc derives output name from input: /tmp/bc_src_$$_file -> .tabc
  local c_out="/tmp/bc_src_$$_${stem}.tabc"

  if [ $c_exit -ne 0 ] || [ ! -f "$c_out" ]; then
    echo -e "${RED}❌ FAIL${NC} (C emit failed)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("bytecode-cmp $file (C emit failed)")
    rm -f "$tmp_src" /tmp/bc_c_log_$$
    return
  fi

  # 2. Compile via bootstrap pipeline
  timeout 15 bash -c "cd '$PROJECT_DIR' && '$PROJECT_DIR/tinyactor' --bootstrap-emit '$tmp_src' '$sh_tabc'" >/tmp/bc_sh_log_$$ 2>&1
  local sh_exit=$?

  if [ $sh_exit -ne 0 ] || [ ! -f "$sh_tabc" ]; then
    echo -e "${RED}❌ FAIL${NC} (bootstrap emit failed)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("bytecode-cmp $file (bootstrap emit failed)")
    rm -f "$tmp_src" "$c_out" /tmp/bc_c_log_$$ /tmp/bc_sh_log_$$
    return
  fi

    # 3. Execute both .tabc files and compare output
  timeout 5 bash -c "cd '$PROJECT_DIR' && '$PROJECT_DIR/tinyactor' '$c_out'" >/tmp/bc_c_run_$$ 2>&1
  local c_run_exit=$?

  timeout 5 bash -c "cd '$PROJECT_DIR' && '$PROJECT_DIR/tinyactor' '$sh_tabc'" >/tmp/bc_sh_run_$$ 2>&1
  local sh_run_exit=$?

  # Non-deterministic tests — compare sorted full output
  local nondet=0
  case "$file" in
    multithread-basic.ta|echo_test.ta) nondet=1 ;;
  esac

  local c_cmp sh_cmp
  if [ $nondet -eq 1 ]; then
    c_cmp=$(cat /tmp/bc_c_run_$$ | sort)
    sh_cmp=$(cat /tmp/bc_sh_run_$$ | sort)
  else
    c_cmp=$(cat /tmp/bc_c_run_$$ | head -1)
    sh_cmp=$(cat /tmp/bc_sh_run_$$ | head -1)
  fi

  if [ "$c_cmp" == "$sh_cmp" ] && [ $c_run_exit -eq 0 ] && [ $sh_run_exit -eq 0 ]; then
    echo -e "${GREEN}✅ PASS${NC} (\"$c_cmp\")"
    PASSED=$((PASSED + 1))
  else
    echo -e "${RED}❌ FAIL${NC} (output differs)"
    echo "     C tabc:     \"$c_cmp\" (exit $c_run_exit)"
    echo "     bootstrap:  \"$sh_cmp\" (exit $sh_run_exit)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("bytecode-cmp $file (output differs)")
  fi

  rm -f "$tmp_src" "$c_out" "$sh_tabc" /tmp/bc_c_log_$$ /tmp/bc_sh_log_$$ /tmp/bc_c_run_$$ /tmp/bc_sh_run_$$
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
  [ -f "$file" ] && run_test "$file"
done
echo ""

# 运行 bootstrap 测试（--bootstrap 模式，与 C 编译路径输出对比）
echo -e "${BLUE}Running bootstrap tests (--bootstrap mode)...${NC}"
for file in *.ta; do
  [ -f "$file" ] && run_bootstrap_test "$file"
done
echo ""

# 运行 self-hosting 测试（自举编译器链）
echo -e "${BLUE}Running self-hosting test...${NC}"
run_selfhost_test
echo ""

# 运行字节码对比测试（C 编译器 vs bootstrap 管线产物）
echo -e "${BLUE}Running bytecode comparison tests...${NC}"
for file in *.ta; do
  [ -f "$file" ] && run_bytecode_cmp_test "$file"
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