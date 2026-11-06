#!/bin/sh
# run_kl_c_tests.sh — KL→C 原生后端（M1）回归门。
#
# 每个 test/kl/*.kl 走两条管线，两边的 stdout 都必须与 .expected 逐字节相同：
#   字节码基线： --from-kl x.kl → x.tabc → ./tavm x.tabc
#   原生后端：  --emit-c  x.kl → x.c    → cc → x → ./x
# 第二条同时证明了 cgen.ta 的输出是可编译、可链接、可运行的 C —— 三个失败面
# （生成非法 C / 链接缺符号 / 语义偏移）各自报不同的名字，不混在一起。
#
# clang 是硬依赖：src/rt.h 的 stage ABI 建在 __attribute__((musttail)) 上
# （源码里有 #error 兜底），gcc 编不过不是配置问题。

set -e
cd "$(dirname "$0")/.."

BC=lib/bootstrap.tabc
TAC_CC=${TAC_CC:-clang}
W=$(mktemp -d "${TMPDIR:-/tmp}/klc.XXXXXX")
trap 'rm -rf "$W"' EXIT

if [ ! -x ./tavm ] || [ ! -f "$BC" ]; then
  echo "run_kl_c_tests: 需要先 make tavm 并 make bootstrap" >&2
  exit 1
fi
if [ ! -f src/rt.o ]; then
  make -s src/rt.o
fi
# 与 Makefile 的 TAC_RUNTIME_OBJ 同款：运行时对象 − vm.o − main.o（run_proc /
# tls_current_proc 由 rt.c 重新提供），+ rt.o。
OBJS=$(ls src/*.o | grep -v -e '/vm\.o$' -e '/main\.o$' -e '/rt_smoke\.o$' | tr '\n' ' ')

total=0
fail=0
for kl in test/kl/*.kl; do
  name=$(basename "$kl" .kl)
  total=$((total + 1))

  if ! ./tavm -q "$BC" --from-kl "$kl" "$W/$name.tabc" > "$W/$name.bcerr" 2>&1; then
    echo "FAIL from-kl $name"; head -4 "$W/$name.bcerr"; fail=$((fail + 1)); continue
  fi
  if ! ./tavm "$W/$name.tabc" > "$W/$name.vm" 2> "$W/$name.vmerr"; then
    echo "FAIL vm-run $name"; head -4 "$W/$name.vmerr"; fail=$((fail + 1)); continue
  fi

  if ! ./tavm -q "$BC" --emit-c "$kl" "$W/$name.c" > "$W/$name.cgerr" 2>&1; then
    echo "FAIL emit-c $name"; head -4 "$W/$name.cgerr"; fail=$((fail + 1)); continue
  fi
  if ! $TAC_CC -O2 -Isrc -I. -o "$W/$name" "$W/$name.c" $OBJS -lpthread -ldl -lm \
      > "$W/$name.cc" 2>&1; then
    echo "FAIL cc $name"; head -8 "$W/$name.cc"; fail=$((fail + 1)); continue
  fi
  if ! "$W/$name" > "$W/$name.nat" 2> "$W/$name.naterr"; then
    echo "FAIL nat-run $name"; head -4 "$W/$name.naterr"; fail=$((fail + 1)); continue
  fi

  if ! cmp -s "$W/$name.nat" "test/kl/$name.expected"; then
    echo "FAIL output $name: got '$(cat "$W/$name.nat")' want '$(cat "test/kl/$name.expected")'"
    fail=$((fail + 1)); continue
  fi
  if ! cmp -s "$W/$name.nat" "$W/$name.vm"; then
    echo "FAIL parity $name: 原生 '$(cat "$W/$name.nat")' vs 字节码 '$(cat "$W/$name.vm")'"
    fail=$((fail + 1)); continue
  fi
done

echo "kl c-backend tests: $total programs (emit-c -> cc -> run -> cmp expected + vm)"
if [ "$fail" -gt 0 ]; then
  echo "KL C BACKEND TESTS FAILED: $fail program(s)"
  exit 1
fi
echo "KL C BACKEND TESTS PASSED"