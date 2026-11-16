#!/bin/bash
# run_kl_link_gate.sh — 步 2 回归门：链接 == 整 .kl 逐字节 + --from-kl 闭环。
#
# 对每个程序单元（driver 闭包 + 每个 lib / lib/bootstrap 单元各自成程序）：
#   1. --emit-kl-dir 写逐模块 .kl + manifest；同源 --emit-kl 写整 .kl；
#      两个 .tabc 产物必须逐字节一致（分块发射不参与编译，G-C 同款判据）。
#   2. --link-kl 按 manifest 顺序拼接 -> linked.kl，与整 .kl **逐字节** cmp。
#      任何跨块 lower 耦合（const 出口、main 调用位置等）被破坏时这里先红。
#   3. --from-kl 闭环：linked.kl 与整 .kl 各自编 .tabc，两者逐字节一致。
#
# 单元之间互不相干（各自一棵临时树、各自一棵 sig cache），所以摊到 TEST_JOBS
# 个 worker 上并发（见 test/pool.sh）。单单元要 5 次 tavm 编译，driver 闭包那
# 一个就要 30s，串行跑等于把另外七个核晾着。判定仍是「一声不响 = 通过」：
# 失败才往自己的 log 里写，汇总时按单元顺序重放——输出时序与串行一致。
#
# sig cache 用每单元独享的目录（热 cache 跳过 typecheck 不影响本门的逐字节
# 比较对象——比较的都是同一编译器的产物；共享目录则会让并发 worker 写同一
# 个 <name>-<key>.sig，而 modsig.sig_store 的 file.write 不是原子的）。

set -e
cd "$(dirname "$0")/.."
. "$(dirname "$0")/pool.sh"

BC=lib/bootstrap.tabc
WORK=$(mktemp -d "${TMPDIR:-/tmp}/klgate-work.XXXXXX")
OUT=$(mktemp -d "${TMPDIR:-/tmp}/klgate-out.XXXXXX")
trap 'rm -rf "$WORK" "$OUT"' EXIT

# check_unit <src>：跑完一个程序的 3 步闭环。通过 = 不输出、返回 0；
# 失败 = 打 FAIL 行 + log 头 4 行、返回 1（pool_run 把每个单元的输出截到
# $OUT/<序号>.log）。
check_unit() {
  local src="$1"
  local name dir
  name=$(basename "$src" .ta)
  dir="$WORK/$name"
  mkdir -p "$dir"

  # 1. 逐模块发射 vs 整发射：编译产物必须不变
  # 本门编的全是**安装树**单元（driver 自举 + lib/*.ta 内核/标准库）：根由入口
  # 文件位置决定（= 本仓库），与 cwd / 环境变量无关——同 Makefile 的
  # bootstrap 站点 / tinyactor 的 ensure_tavm_driver。
  #
  # 两条发射路径是**同源两次独立编译**（这正是本步要比的东西），彼此不依赖，
  # 所以并行跑：driver 闭包一次编译就差 10s，串行两次就把木桶钉死在单核上。
  # 各用一棵 sig cache：并发写同一个目录会撞到 modsig.sig_store 的非原子
  # file.write（见文件头）。
  local p1 p2 ok1=0 ok2=0
  ./tavm -q "$BC" --emit-kl-dir "$dir" "$src" "$dir/prog.tabc" "$dir/cache1" > "$dir/emit.log" 2>&1 & p1=$!
  ./tavm -q "$BC" --emit-kl "$dir/whole.kl" "$src" "$dir/prog2.tabc" "$dir/cache2" > "$dir/emit2.log" 2>&1 & p2=$!
  wait "$p1" || ok1=1
  wait "$p2" || ok2=1
  if [ "$ok1" -ne 0 ]; then
    echo "FAIL emit-kl-dir $src"; head -4 "$dir/emit.log"; return 1
  fi
  if [ "$ok2" -ne 0 ]; then
    echo "FAIL emit-kl $src"; head -4 "$dir/emit2.log"; return 1
  fi
  if ! cmp -s "$dir/prog.tabc" "$dir/prog2.tabc"; then
    echo "FAIL compile-changed $src (emit-kl-dir 不该影响编译产物)"
    return 1
  fi

  # 2. 链接 == 整 .kl 逐字节
  local args="" f
  while IFS= read -r f; do
    args="$args $dir/$f"
  done < "$dir/manifest"
  if ! ./tavm -q "$BC" --link-kl "$dir/linked.kl" $args > "$dir/link.log" 2>&1; then
    echo "FAIL link-kl $src"; head -4 "$dir/link.log"; return 1
  fi
  if ! cmp -s "$dir/linked.kl" "$dir/whole.kl"; then
    echo "FAIL link-ne-whole $src (linked .kl != whole .kl)"
    return 1
  fi

  # 3. --from-kl 闭环：两条 .kl 路产物一致（同样是两个互不依赖的编译，并行）
  ./tavm -q "$BC" --from-kl "$dir/linked.kl" "$dir/fromlink.tabc" > "$dir/fk1.log" 2>&1 & p1=$!
  ./tavm -q "$BC" --from-kl "$dir/whole.kl" "$dir/fromwhole.tabc" > "$dir/fk2.log" 2>&1 & p2=$!
  ok1=0; ok2=0
  wait "$p1" || ok1=1
  wait "$p2" || ok2=1
  if [ "$ok1" -ne 0 ]; then
    echo "FAIL from-kl(linked) $src"; head -4 "$dir/fk1.log"; return 1
  fi
  if [ "$ok2" -ne 0 ]; then
    echo "FAIL from-kl(whole) $src"; head -4 "$dir/fk2.log"; return 1
  fi
  if ! cmp -s "$dir/fromlink.tabc" "$dir/fromwhole.tabc"; then
    echo "FAIL fromkl-mismatch $src"
    return 1
  fi
}

units=(lib/bootstrap/driver.ta lib/*.ta lib/bootstrap/*.ta lib/native/*.ta)
pool_run "$(test_jobs)" "$OUT" check_unit "${units[@]}"

fail=0
for i in "${!units[@]}"; do
  if [ -s "$OUT/$i.log" ]; then
    cat "$OUT/$i.log"
    fail=$((fail + 1))
  fi
done

echo "kl link gate: ${#units[@]} programs (emit/link/cmp/from-kl closed loop)"
if [ "$fail" -gt 0 ]; then
  echo "KL LINK GATE FAILED: $fail program(s)"
  exit 1
fi
echo "KL LINK GATE PASSED"