#!/bin/sh
# check_no_codegen_closure.sh — step7b-1 闭包门：driver 的 import 闭包不得含
# codegen。backend_driver 通向 codegen 有两条边：lower-ast（step7a 拔除，
# 由 lisp-gate 的直接 import grep 看守——#269）和 driver（本 PR 拔除）。这里
# 看守 driver 这条边：driver 的中间依赖（typecheck/modsig/…）若偷偷 import
# codegen 同样红。做法：expand_imports.py 展开 driver 全闭包搜 serialize_tabc
# （codegen 独有符号）。7c 删 codegen 后，两道门随老世界一起消失。
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
python3 "$root/vm-demo/lisp/expand_imports.py" "$root/lib/bootstrap/driver.ta" "$tmp" >/dev/null
if grep -q "serialize_tabc" "$tmp"; then
  echo "错误：driver import 闭包含 codegen（step7b-1 driver 拆分被回退？）" >&2
  exit 1
fi