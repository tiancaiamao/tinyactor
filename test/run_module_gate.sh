#!/bin/bash
# run_module_gate.sh — kernel 自足纪律门（kl-kernel 步 1）。
#
# 每个 lib/*.ta、lib/bootstrap/*.ta 单元过两半检查：
#
#   A. 独立 build：`tinyactor build <unit> <out>`，每个单元用**全新**的
#      sig-cache 目录（TA_SIG_CACHE_DIR）。新缓存是承重的：typecheck_cached
#      命中签名单元会**整体跳过 typecheck**——项目级热缓存会把「缺声明」
#      静默成绿灯。无 main 的单元照样 build（与 lib/list.ta 同语义）；
#      任一单元独立编译失败即红。
#
#   B. 直declaration 扫描：单元里出现的每个点号模块引用，必须被该单元
#      **自己的**声明覆盖——`import M`、`external fn M.*`、编译器承诺内建
#      （str 低阶原语、file.read/write/exists、int.to_float）、host C 模块
#      （vm/net）、或单元自身。只有 A 不够：resolve_imports 把传递 import
#      的单元注册进同一个全局命名空间，单元可以搭依赖 import 的便车
#      （compile.ta 的 list.append 曾搭 lower-ast 的 import list）——
#      扁平闭包的结构代价，正是这半要钉死的。
#
# A 半的每个单元是一个独立 tinyactor 进程 + 独立冷缓存，互不相干，所以摊到
# TEST_JOBS 个 worker 上并发（见 test/pool.sh）；判定仍是「一声不响 = 通过」，
# 失败才往自己的 log 里写，汇总时按单元顺序重放。
set -u
cd "$(dirname "$0")/.."
. "$(dirname "$0")/pool.sh"

fail=0
total=0

# macOS 没有 GNU timeout（CI macos-latest 踩过）：有就用，没有直接跑。
RUN="./tinyactor build"
command -v timeout >/dev/null 2>&1 && RUN="timeout 300 ./tinyactor build"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/modgate-work.XXXXXX")
OUT=$(mktemp -d "${TMPDIR:-/tmp}/modgate-out.XXXXXX")
trap 'rm -rf "$WORK" "$OUT"' EXIT

# check_unit <unit>：A 半，独立 build（冷 sig-cache）。通过 = 不输出、返回 0。
check_unit() {
  local f="$1"
  local name cache out log
  name=$(basename "$f" .ta)
  cache="$WORK/$name.cache"
  out="$WORK/$name.tabc"
  log="$WORK/$name.log"
  mkdir -p "$cache"
  if ! TA_SIG_CACHE_DIR="$cache" $RUN "$f" "$out" > "$log" 2>&1; then
    echo "FAIL build $f"
    head -6 "$log"
    return 1
  fi
}

# ---- A. 逐单元独立 build（冷 sig-cache）----
units=(lib/*.ta lib/bootstrap/*.ta)
pool_run "$(test_jobs)" "$OUT" check_unit "${units[@]}"
total=${#units[@]}
for i in "${!units[@]}"; do
  if [ -s "$OUT/$i.log" ]; then
    cat "$OUT/$i.log"
    fail=$((fail + 1))
  fi
done

# ---- B. 直declaration 扫描（python3，规则见文件头）----
scan=$(python3 tools/check_module_imports.py 2>&1) || { echo "FAIL scan: $scan"; exit 1; }
if [ -n "$scan" ]; then
  printf '%s\n' "$scan"
  fail=$((fail + 1))
fi

echo "module gate: $((total)) units built standalone + direct-declaration scan"
if [ "$fail" -ne 0 ]; then
  echo "MODULE GATE FAILED: $fail finding(s)" >&2
  exit 1
fi
echo "MODULE GATE PASSED"