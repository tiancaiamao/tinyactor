#!/bin/sh
# run_corpus.sh — 「lisp 内核能吃下多少真 TA 代码」的量化台。不 gate 任何东西。
#
# 测两件事，分开报，因为它们经常差很远：
#   编译覆盖  lower-ast + compile 能过 = 进得来管线
#   语义一致  tavm 跑出的 stdout == tinyactor 跑出的 stdout = 真的对
# 只跑到「进得来」会高估：字节码生成了不等于跑得对（print_val、栈深、
# 字符串这些坑都在运行期才暴露）。
#
# 一文件一进程 + 超时：编译器里有构造会让 compile 不收敛，一个坏文件不能
# 带走整轮结果——挂住本身就是一条数据（status=hang）。
#
# test/basic/*-errors.ta 不计入：负例本来就该被拒，拒了不能说明 lisp 对。
set -u
cd "$(dirname "$0")/../.."

TAVM=${TAVM:-./tavm}
CORPUS=test/backend/corpus
ONE=test/backend/corpus.one
OUT=test/backend/corpus.report
PER_FILE_TIMEOUT=${PER_FILE_TIMEOUT:-20}

make tavm
mkdir -p "$CORPUS"
: > "$OUT"

total=0
mkdir -p "$CORPUS/src"
for f in test/basic/*.ta; do
  name=$(basename "$f")
  case "$name" in
    *-errors.ta) continue ;;
  esac
  total=$((total + 1))
  # 先展开 import 树成单文件再喂管线：lisp 管线是单命名空间，不展开的
  # 话 dotted 调用（bool.not…）被编成模块 cfunc，运行期宿主 miss 返回
  # nil —— 那测的是管线的缺陷，不是 tavm 的语义。
  python3 tools/expand_imports.py "$f" "$CORPUS/src/$name" > /dev/null
  printf '%s\n' "$CORPUS/src/$name" > "$ONE"
  # timeout 在 macOS 上是 coreutils 的（brew 装 gtimeout），两个名字都试。
  TO=timeout
  command -v timeout >/dev/null 2>&1 || TO=gtimeout
  line=$("$TO" "$PER_FILE_TIMEOUT" ./tinyactor run test/backend/corpus1.ta 2>/dev/null)
  rc=$?
  if [ "$rc" -ge 124 ]; then
    line=$(printf '%s\thang\t' "$name")
  elif [ "$rc" -ne 0 ]; then
    line=$(printf '%s\tdriver-error\t' "$name")
  fi
  status=$(printf '%s' "$line" | cut -f2)
  case "$status" in
    ok)
      # 时间戳归一：log-lib 的行契约含 epoch-ms，两边各自取当前时钟，
      # 数值必然不同——语义比对按 [TS] 占位
      NORM='s/\[[0-9]{10,}\]/[TS]/g'
      ta_out=$("$TO" "$PER_FILE_TIMEOUT" ./tinyactor run "$f" 2>&1 | grep -v '^warning: dlopen failed' | sed -E "$NORM")
      # tavm -q：只取程序自己的输出。TA 的 runtime 丢弃 main 的返回值
      # （`fn main() { 42 }` 在 tinyactor 下无输出），而 tavm 默认会把
      # entry 的值也打出来，且 print 不换行——那句没法从输出里摘掉。
      vm_out=$("$TO" "$PER_FILE_TIMEOUT" "$TAVM" -q "$CORPUS/$(printf '%s' "$name" | sed 's/\.ta$//').tabc" 2>&1 | sed -E "$NORM")
      if [ "$ta_out" = "$vm_out" ]; then
        verdict=same
      else
        verdict=diff
      fi
      printf '%s\t%s\t%s\n' "$name" "$status" "$verdict" >> "$OUT"
      ;;
    *)
      printf '%s\t%s\n' "$name" "$status" >> "$OUT"
      ;;
  esac
done

echo "=== 语料覆盖 (test/basic, 不含 *-errors.ta, 共 $total 个)"
awk -F'\t' '
  $2 == "ok"      { ok++ }
  $2 == "reject"  { reject++ }
  $2 == "hang"    { hang++ }
  $2 == "parse"   { parse++ }
  $2 == "tokenize"{ tok++ }
  $2 == "lower"   { lower++ }
  $2 == "no-read" { noread++ }
  $2 == "driver-error" { drv++ }
  $3 == "same"    { same++ }
  $3 == "diff"    { diff++ }
  END {
    printf "  编译覆盖 ok      %3d / %d\n", ok, ok+reject+hang+parse+tok+lower+noread+drv
    printf "  语义一致 same    %3d\n", same
    printf "  语义不一致 diff  %3d\n", diff
    printf "  --- 其余卡点 ---\n"
    printf "  compile 拒绝     %3d\n", reject
    printf "  compile 不收敛   %3d\n", hang
    printf "  parse 失败       %3d\n", parse
    printf "  tokenize 失败    %3d\n", tok
    printf "  lower 失败       %3d\n", lower
    if (drv)   printf "  驱动异常         %3d\n", drv
    if (noread)printf "  读不到           %3d\n", noread
  }' "$OUT"
echo "=== 明细: $OUT"