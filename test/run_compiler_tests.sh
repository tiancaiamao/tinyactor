#!/bin/bash
# test/run_compiler_tests.sh — 编译器层测试（parser / typecheck / lower / .kl writer）。
#
# 常规部分 = test/compiler/*.ta 的正/负例（run_category），外加一个需要单独
# 建产物检查 stderr 的门（check_quote_pattern_silent，#443）。
#
# G-B（管线等价门：.ta 路与 .kl 路编出的 .tabc 逐字节相同）在 C1 里以
# test/kl-driver-roundtrip.ta（单进程、直接调 driver 的公开半程）暂代，因为
# 那时接缝还不存在；C2 起 driver 有了 --emit-kl / --from-kl，G-B 就搬成
# run_bootstrap_tests.sh 里的两进程 + cmp 形态（复用那里的种子重建作 A 路），
# 即这条门现在跑在真正的接缝上。
source "$(dirname "$0")/lib.sh"

# issue #443: quoted patterns must be silent. A constructor pattern is not
# type-directed, so the unknown-constructor check reads the names out of the
# pattern — and a quoted pattern keeps its payload in the AST ('Red reads as
# (quote (quote Red)), '[Red, Green] as (quote <chain>)). Handing that payload
# to the symbol-only helpers printed a C error line and returned nil, so the
# compile still exited 0: **only stderr bytes see it**. Hence this gate builds
# the file (rather than running it) and asserts a zero-byte stderr, while
# quote-pattern-clean.ta covers the same source through the run path.
check_quote_pattern_silent() {
  local src="$SCRIPT_DIR/compiler/quote-pattern-clean.ta"
  local dir=$(mktemp -d "${TMPDIR:-/tmp}/qpat_XXXXXX")
  local err="$dir/stderr.txt"
  local out="$dir/quote-pattern-clean.tabc"
  local status=0

  ( cd "$PROJECT_DIR" && "$TINYACTOR" build "$src" "$out" ) 2>"$err" >/dev/null || status=$?

  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "quoted patterns emit no stderr:"
  local bytes=$(wc -c <"$err" | tr -d ' ')
  if [ $status -eq 0 ] && [ "$bytes" = "0" ]; then
    echo -e "${GREEN}✅ PASS${NC} (build exit 0, stderr 0 bytes)"
    PASSED=$((PASSED + 1))
  else
    echo -e "${RED}❌ FAIL${NC} (build exit $status, stderr $bytes bytes)"
    sed 's/^/         /' "$err"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("quoted patterns emit no stderr")
  fi
  rm -rf "$dir"
}

run_category "Compiler" "$SCRIPT_DIR/compiler"

# G-B（管线等价门）不在这里：它是两进程 + cmp 的形态，跑在
# test/run_bootstrap_tests.sh（复用那里的种子重建作 A 路，见该文件头部）。

check_quote_pattern_silent

print_summary
exit $([ $FAILED -eq 0 ])