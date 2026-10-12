#!/bin/bash
# test/run_compiler_tests.sh — 编译器层测试（parser / typecheck / lower / .kl writer）。
#
# 常规部分 = test/compiler/*.ta 的正/负例（run_category）。此外本 runner 显式
# 接线一个需要额外条件的门：
#
#   V5   「全仓 0 处依赖旧引号行为」落成 grep 断言（设计文档 §4.4 V5）：
#        .kl 的引号规则改了 4 种拼写（'nil / 'true / 'false / '<数字>）的读法。
#
# G-B（管线等价门：.ta 路与 .kl 路编出的 .tabc 逐字节相同）在 C1 里以
# test/kl-driver-roundtrip.ta（单进程、直接调 driver 的公开半程）暂代，因为
# 那时接缝还不存在；C2 起 driver 有了 --emit-kl / --from-kl，G-B 就搬成
# run_bootstrap_tests.sh 里的两进程 + cmp 形态（复用那里的种子重建作 A 路），
# 即这条门现在跑在真正的接缝上。
source "$(dirname "$0")/lib.sh"

# V5: 旧引号拼写的依赖面必须为 0。两半：
#   (a) reader 的消费面 —— 没有任何 sexp.parse 调用点喂进带引号的特殊 token
#       字面量。白名单 = .kl 引号规则自己的三个测试文件（它们断言的正是**新**
#       行为：'nil 是符号 nil；实测只有 sexp-basic.ta:36 用字面量实参，另两个
#       把输入装在表里喂 sexp.parse，留白名单防将来的误报）；
#   (b) 编译器自身源码（lib/*.ta）不出现 `(quote nil|true|false)` 这种「引号
#       里的值」拼法 —— 源码要符号就写 'nil，两种 reader 下都是符号。
#       冻结语料（test/kernfuzz-frozen/*.sexp）**不在 C reader 的消费面上**：读
#       它们的是 tools/kernfuzz/{sexp.py,snapshot.scm}（python/guile 各自的
#       reader），全仓 0 处 .ta 经 file.read + sexp.parse 读它们 —— 所以 V5 只
#       需上面两半断言，不需要第三种。
# 真实管线的两条保护：运行期唯一的 reader 输入（bytecode.lib_src() 前奏文本）
# 实测无引号，由 test/compiler/kl-roundtrip.ta 断言（它的前奏断言块）；编译路径
# 的 TA 源码侧不受这条 C 规则影响（TA parser 的 ' 一直是"读成名字"），G-B 的
# 逐字节相同（两个 .tabc 头 #fns/#consts 一致）就是证据。
check_v5_legacy_quotes() {
  # 白名单含 test/backend/corpus/src/sexp-basic.ta：那是 run_corpus.sh 生成的
  # **副本**（test/backend/corpus/ 被 gitignore），内容是同一份已在白名单里的
  # fixture。grep 走文件系统，所以跑过 backend-gate 的树上它会出现——不加这条
  # 会变成"先跑哪个 gate"决定 V5 红绿的顺序依赖。
  local allow='test/basic/sexp-basic.ta|test/backend/corpus/src/sexp-basic.ta|test/compiler/kl-roundtrip.ta'
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

# G-B（管线等价门）不在这里：它是两进程 + cmp 的形态，跑在
# test/run_bootstrap_tests.sh（复用那里的种子重建作 A 路，见该文件头部）。

check_v5_legacy_quotes

print_summary
exit $([ $FAILED -eq 0 ])