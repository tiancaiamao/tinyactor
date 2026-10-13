#!/bin/sh
# run_kl_link_gate.sh — 步 2 回归门：链接 == 整 .kl 逐字节 + --from-kl 闭环。
#
# 对每个程序单元（driver 闭包 + 每个 lib / lib/bootstrap 单元各自成程序）：
#   1. --emit-kl-dir 写逐模块 .kl + manifest；同源 --emit-kl 写整 .kl；
#      两个 .tabc 产物必须逐字节一致（分块发射不参与编译，G-C 同款判据）。
#   2. --link-kl 按 manifest 顺序拼接 -> linked.kl，与整 .kl **逐字节** cmp。
#      任何跨块 lower 耦合（const 出口、main 调用位置等）被破坏时这里先红。
#   3. --from-kl 闭环：linked.kl 与整 .kl 各自编 .tabc，两者逐字节一致。
#
# sig cache 用本次运行独享的目录（热 cache 跳过 typecheck 不影响本门的
# 逐字节比较对象——比较的都是同一编译器的产物）。

set -e
cd "$(dirname "$0")/.."

BC=lib/bootstrap.tabc
CACHE=$(mktemp -d "${TMPDIR:-/tmp}/klgate-cache.XXXXXX")
WORK=$(mktemp -d "${TMPDIR:-/tmp}/klgate-work.XXXXXX")
trap 'rm -rf "$CACHE" "$WORK"' EXIT

total=0
fail=0
for src in lib/bootstrap/driver.ta lib/*.ta lib/bootstrap/*.ta; do
  total=$((total + 1))
  name=$(basename "$src" .ta)
  dir="$WORK/$name"
  mkdir -p "$dir"

  # 1. 逐模块发射 vs 整发射：编译产物必须不变
  if ! ./tavm -q "$BC" --emit-kl-dir "$dir" "$src" "$dir/prog.tabc" "$CACHE" > "$dir/emit.log" 2>&1; then
    echo "FAIL emit-kl-dir $src"; head -4 "$dir/emit.log"; fail=$((fail + 1)); continue
  fi
  if ! ./tavm -q "$BC" --emit-kl "$dir/whole.kl" "$src" "$dir/prog2.tabc" "$CACHE" > "$dir/emit2.log" 2>&1; then
    echo "FAIL emit-kl $src"; head -4 "$dir/emit2.log"; fail=$((fail + 1)); continue
  fi
  if ! cmp -s "$dir/prog.tabc" "$dir/prog2.tabc"; then
    echo "FAIL compile-changed $src (emit-kl-dir 不该影响编译产物)"
    fail=$((fail + 1)); continue
  fi

  # 2. 链接 == 整 .kl 逐字节
  args=""
  while IFS= read -r f; do
    args="$args $dir/$f"
  done < "$dir/manifest"
  if ! ./tavm -q "$BC" --link-kl "$dir/linked.kl" $args > "$dir/link.log" 2>&1; then
    echo "FAIL link-kl $src"; head -4 "$dir/link.log"; fail=$((fail + 1)); continue
  fi
  if ! cmp -s "$dir/linked.kl" "$dir/whole.kl"; then
    echo "FAIL link-ne-whole $src (linked .kl != whole .kl)"
    fail=$((fail + 1)); continue
  fi

  # 3. --from-kl 闭环：两条 .kl 路产物一致
  if ! ./tavm -q "$BC" --from-kl "$dir/linked.kl" "$dir/fromlink.tabc" > "$dir/fk1.log" 2>&1; then
    echo "FAIL from-kl(linked) $src"; head -4 "$dir/fk1.log"; fail=$((fail + 1)); continue
  fi
  if ! ./tavm -q "$BC" --from-kl "$dir/whole.kl" "$dir/fromwhole.tabc" > "$dir/fk2.log" 2>&1; then
    echo "FAIL from-kl(whole) $src"; head -4 "$dir/fk2.log"; fail=$((fail + 1)); continue
  fi
  if ! cmp -s "$dir/fromlink.tabc" "$dir/fromwhole.tabc"; then
    echo "FAIL fromkl-mismatch $src"
    fail=$((fail + 1)); continue
  fi
done

echo "kl link gate: $total programs (emit/link/cmp/from-kl closed loop)"
if [ "$fail" -gt 0 ]; then
  echo "KL LINK GATE FAILED: $fail program(s)"
  exit 1
fi
echo "KL LINK GATE PASSED"