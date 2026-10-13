#!/bin/sh
# run_kl_sem_tests.sh — `.kl` 语义正例（kl-kernel 步 3，docs/kl-format.md §9.4）。
#
# 每个测试 = 一份 .kl（desugar 后的正例；match.kl 是 match.lisp 974703b^ 的
# 脱糖教学对照）+ 同名 .expected：
#   --from-kl 编译 -> tavm 运行 -> stdout 与 .expected cmp。
# `.kl` 路不跑 typecheck（结构性事实，docs §1.1），所以这些测试同时守着
# 「reader + compile_low + 前奏」的语义面：负例由 kernfuzz/parse-errors 侧
# 负责，这里只收可运行的正确程序。

set -e
cd "$(dirname "$0")/.."

BC=lib/bootstrap.tabc
W=$(mktemp -d "${TMPDIR:-/tmp}/klsem.XXXXXX")
trap 'rm -rf "$W"' EXIT

total=0
fail=0
for kl in test/kl/*.kl; do
  name=$(basename "$kl" .kl)
  total=$((total + 1))
  if ! ./tavm -q "$BC" --from-kl "$kl" "$W/$name.tabc" > "$W/$name.cerr" 2>&1; then
    echo "FAIL from-kl $name"; head -4 "$W/$name.cerr"; fail=$((fail + 1)); continue
  fi
  if ! ./tavm "$W/$name.tabc" > "$W/$name.out" 2> "$W/$name.rerr"; then
    echo "FAIL run $name"; head -4 "$W/$name.rerr"; fail=$((fail + 1)); continue
  fi
  if ! cmp -s "$W/$name.out" "test/kl/$name.expected"; then
    echo "FAIL output $name: got '$(cat "$W/$name.out")' want '$(cat "test/kl/$name.expected")'"
    fail=$((fail + 1)); continue
  fi
done

echo "kl sem tests: $total programs (from-kl -> run -> cmp)"
if [ "$fail" -gt 0 ]; then
  echo "KL SEM TESTS FAILED: $fail program(s)"
  exit 1
fi
echo "KL SEM TESTS PASSED"