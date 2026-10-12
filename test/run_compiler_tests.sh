#!/bin/bash
# test/run_compiler_tests.sh — 编译器层测试（parser / typecheck / lower / .kl writer）。
#
# 常规部分 = test/compiler/*.ta 的正/负例（run_category）。此外本 runner 显式
# 接线两个需要额外条件的门：
#
#   G-B  test/kl-driver-roundtrip.ta —— 管线等价门：lower(driver.ta) 与
#        read(write(lower(driver.ta))) 两条路编出的 .tabc 逐字节相同
#        （验收判据与 G-D 同构 = cmp 等价）。它放在 test/ 根目录而不是
#        test/compiler/ 里：这是管线门不是单元测试，本地 ~10s（CI ~3x），
#        需要比 run_category 默认 TEST_TIMEOUT=60s 更宽的预算，所以在这里
#        用 TEST_TIMEOUT=300 显式调用。arena 不是问题：默认 64 MiB 就够
#        （实测 floor ≈ 47 MiB，两镜像同进程，见 docs/kl-format.md §arena），
#        所以不设 TA_ACTOR_HEAP（它是**裸字节数**，写 '1G' 会被静默忽略）。
#
#   V5   「全仓 0 处依赖旧引号行为」落成 grep 断言（设计文档 §4.4 V5）：
#        .kl 的引号规则改了 4 种拼写（'nil / 'true / 'false / '<数字>）的读法。
source "$(dirname "$0")/lib.sh"

# V5: 旧引号拼写的依赖面必须为 0。两半：
#   (a) reader 的消费面 —— 没有任何 sexp.parse 调用点喂进带引号的特殊 token
#       字面量。白名单 = .kl 引号规则自己的三个测试文件（它们断言的正是**新**
#       行为：'nil 是符号 nil；实测只有 sexp-basic.ta:36 用字面量实参，另两个
#       把输入装在表里喂 sexp.parse，留白名单防将来的误报）；
#   (b) 编译器自身源码（lib/*.ta）不出现 `(quote nil|true|false)` 这种「引号
#       里的值」拼法 —— 源码要符号就写 'nil，两种 reader 下都是符号。
# 真实管线的两条保护：运行期唯一的 reader 输入（bytecode.lib_src() 前奏文本）
# 实测无引号，由 test/kl-driver-roundtrip.ta 断言；编译路径的 TA 源码侧不受
# 这条 C 规则影响（TA parser 的 ' 一直是"读成名字"），G-B 的逐字节相同
# （617 fns / 1211 consts）就是证据。
check_v5_legacy_quotes() {
  local allow='test/basic/sexp-basic.ta|test/compiler/kl-roundtrip.ta|test/kl-driver-roundtrip.ta'
  local hits_a hits_b
  hits_a=$(grep -rnE --include=*.ta --include=*.c --include=*.py \
    "sexp\.parse\(\"[^\"]*'(nil|true|false|[0-9])" \
    "$PROJECT_DIR/lib" "$PROJECT_DIR/test" "$PROJECT_DIR/tools" 2>/dev/null | grep -Ev "$allow")
  hits_b=$(grep -rnE --include=*.ta "\(quote (nil|true|false)\)" "$PROJECT_DIR/lib" 2>/dev/null)

  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "V5 legacy quote use:"
  if [ -z "$hits_a" ] && [ -z "$hits_b" ]; then
    echo -e "${GREEN}✅ PASS${NC} (0 处依赖旧引号行为)"
    PASSED=$((PASSED + 1))
  else
    echo -e "${RED}❌ FAIL${NC}"
    if [ -n "$hits_a" ]; then
      echo "       sexp.parse 字面量依赖旧读法:"
      echo "$hits_a" | sed 's/^/         /'
    fi
    if [ -n "$hits_b" ]; then
      echo "       lib/ 里出现 (quote nil|true|false):"
      echo "$hits_b" | sed 's/^/         /'
    fi
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("V5 legacy quote use")
  fi
}

run_category "Compiler" "$SCRIPT_DIR/compiler"

# G-B: 管线门（见文件头注释）；本地 ~10s，CI ~3x，所以给 300s 预算。
TEST_TIMEOUT=300 run_test "$SCRIPT_DIR/kl-driver-roundtrip.ta"

check_v5_legacy_quotes

print_summary
exit $([ $FAILED -eq 0 ])