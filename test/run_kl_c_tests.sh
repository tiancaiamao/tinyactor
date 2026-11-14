#!/bin/sh
# run_kl_c_tests.sh — KL→C 原生后端（M2）回归门。
#
# 每个 test/kl/*.kl 走两条管线，两边的 stdout 都必须与 .expected 逐字节相同：
#   字节码基线： --from-kl x.kl → x.tabc → ./tavm x.tabc
#   原生后端：  --emit-c  x.kl → x.c    → cc → x → ./x
# 第二条同时证明了 cgen.ta 的输出是可编译、可链接、可运行的 C —— 三个失败面
# （生成非法 C / 链接缺符号 / 语义偏移）各自报不同的名字，不混在一起。
# 每个程序跑两遍：普通一遍 + TA_GC_STRESS=1（每次分配都收集）一遍，两遍的
# stdout 都必须与 .expected 逐字节相同。actor-* 再跑第三遍 NWWORKERS=2（见
# 下面多 worker 一遍的注释）。
#
# clang 是硬依赖：lib/native/rt.h 的 stage ABI 建在 __attribute__((musttail)) 上
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
if [ ! -f lib/native/rt.o ]; then
  make -s lib/native/rt.o
fi
# 与 Makefile 的 TAC_RUNTIME_OBJ 同款：运行时对象 − vm.o − main.o（run_proc /
# tls_current_proc 由 lib/native/rt.c 重新提供），+ lib/native/rt.o。
OBJS="$(ls src/*.o | grep -v -e '/vm\.o$' -e '/main\.o$' -e '/rt_smoke\.o$' | tr '\n' ' ') lib/native/rt.o"

# 多 worker 那遍的看门狗：丢唤醒的回归表现为挂住（不是崩），没有上限就只有
# 上级 job 的 timeout 兜。GNU timeout 在 macOS 默认没有，回落到 gtimeout；
# 两样都没有就直跑（宁可慢，不可假失败）。
TO=""
if command -v timeout >/dev/null 2>&1; then TO="timeout 60"
elif command -v gtimeout >/dev/null 2>&1; then TO="gtimeout 60"
fi

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
  # $TAC_LDFLAGS：Linux 上必须把可执行文件的符号导出去（-Wl,--export-dynamic），
  # 否则 .so 里的 tls_current_proc 在 dlopen 时解析不到 —— macOS 走
  # -undefined dynamic_lookup，那边是空的。由 Makefile 的 RDYNAMIC 传进来。
  if ! $TAC_CC -O2 -Isrc -Ilib/native -I. -o "$W/$name" "$W/$name.c" $OBJS $TAC_LDFLAGS -lpthread -ldl -lm \
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

  # 第二遍：每次堆分配都请求一次收集（ta_inline.h 的 TA_GC_STRESS）。这是
  # 帧发布纪律（RT_PUBLISH / rt_fill_frame / 溢出实参）唯一的硬门：一次
  # 收集就搬走并压缩整个活集，任何一格没铺 nil 或发布得比写的早，残留的
  # 旧 Val 就会在这里当场变成悬垂指针。
  if ! TA_GC_STRESS=1 "$W/$name" > "$W/$name.stress" 2> "$W/$name.stresserr"; then
    echo "FAIL nat-run-stress $name"; head -4 "$W/$name.stresserr"; fail=$((fail + 1)); continue
  fi
  if ! cmp -s "$W/$name.stress" "test/kl/$name.expected"; then
    echo "FAIL stress $name: got '$(cat "$W/$name.stress")' want '$(cat "test/kl/$name.expected")'"
    fail=$((fail + 1)); continue
  fi

  # 第三遍：多 worker（NWWORKERS=2）。调度器与字节码腿共用 scheduler.c，但
  # ccall 挂起后的收尾（run_proc 认领/发布所有权）只存在于 lib/native/rt.c —— 单线程那两
  # 遍走不到 worker_loop 的唤醒路径，多 worker 的崩溃与丢唤醒只能在 NW>1 现形。
  # 只对 actor-* 跑：非 actor 程序没有可并行的 proc，多这一遍只花时间不涨覆盖。
  # 输出与线程数无关（7 个用例 × NW=2 × 10 次逐字节一致，加这道门前实测过）。
  case "$name" in
  actor-*)
    if ! $TO env NWWORKERS=2 "$W/$name" > "$W/$name.nw2" 2> "$W/$name.nw2err"; then
      echo "FAIL nat-run-nw2 $name"; head -4 "$W/$name.nw2err"; fail=$((fail + 1)); continue
    fi
    if ! cmp -s "$W/$name.nw2" "test/kl/$name.expected"; then
      echo "FAIL nw2 $name: got '$(cat "$W/$name.nw2")' want '$(cat "test/kl/$name.expected")'"
      fail=$((fail + 1)); continue
    fi
    ;;
  esac
done

echo "kl c-backend tests: $total programs (emit-c -> cc -> run x2 [plain + TA_GC_STRESS=1] -> cmp expected + vm; actor-* + NWWORKERS=2)"
if [ "$fail" -gt 0 ]; then
  echo "KL C BACKEND TESTS FAILED: $fail program(s)"
  exit 1
fi
echo "KL C BACKEND TESTS PASSED"