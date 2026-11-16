#!/bin/bash
# test/pool.sh — 并发跑独立用例的最小调度器，test/lib.sh 与需要自己循环的
# 测试脚本（run_kl_link_gate.sh …）共用这一份。
#
# 为什么要有它：每个用例都是**独立进程**（tinyactor run 各自编译 + 跑，kl 门
# 各自编一个单元），串行跑等于把多核晾着——整套 make test 的 user 时间与 real
# 时间几乎相等（并行度 0.92）。把用例摊到 TEST_JOBS 个 worker 上是最直接的
# 提速，且不改变任何一个用例的语义。
#
# 判据仍由用例自己出（run_test 打一行 ✅/❌，kl 门写自己的 status 文件）：
# 这里只管调度与落盘，不解析结论——记账只有一处，不引入第二套。

# test_jobs: worker 数。TEST_JOBS 显式给了就用（=1 退回串行，调试单个用例
# 时不用改脚本）；否则取核数并封顶 8（CI runner 4 核，本机 8 核就够；再多
# worker 只是让每个编译进程抢缓存）。
test_jobs() {
  local n="${TEST_JOBS:-}"
  if [ -z "$n" ]; then
    n=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)
    [ "$n" -gt 8 ] && n=8
    [ "$n" -lt 1 ] && n=1
  fi
  printf '%s\n' "$n"
}

# pool_run <jobs> <out_dir> <cmd> <item>...
#   每个 item 起一个后台 job 跑 `<cmd> <item>`，输出（stdout+stderr）写
#   <out_dir>/<序号>.log；跑满 jobs 个就等一个空位。序号 = item 在参数里的
#   下标，调用方按同一个顺序重放这些文件，输出的时序因此与串行一致。
#   返回值恒为 0：结论由调用方从 log/status 文件里读。
pool_run() {
  local jobs="$1" out_dir="$2" cmd="$3"
  shift 3
  local -a items=("$@")
  local i item
  for i in "${!items[@]}"; do
    # bash 3.2（macOS 自带）没有 wait -n，只能轮询在跑的 job 数
    while [ "$(jobs -pr | wc -l)" -ge "$jobs" ]; do sleep 0.05; done
    "$cmd" "${items[$i]}" > "$out_dir/$i.log" 2>&1 &
  done
  wait
}