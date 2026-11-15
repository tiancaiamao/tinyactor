#!/bin/sh
# cons-probe.sh — 量「把 cons/car/cdr 收紧成列表操作」还剩多少站点没迁移。
#
# 用途（docs/cons-retirement-design.md §5–§6）：迁移期 cons 仍是宽松签名，
# 本脚本临时把 lib/bootstrap/typecheck.ta 的三行内建换成列表版、self-host 一遍、
# 数错误，然后把源码与 lib/bootstrap.tabc 原样恢复 —— 工作树不留任何改动。
#
#   scripts/cons-probe.sh            # 打印总数 + 按模块的错误站点数
#
# 输出 0 = 迁移完成（可以落地 cons : a -> List a -> List a）。
set -e

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

ty=lib/bootstrap/typecheck.ta
tabc=lib/bootstrap.tabc
bak=$(mktemp -d)
cp "$ty" "$bak/typecheck.ta"
cp "$tabc" "$bak/bootstrap.tabc"
trap 'cp "$bak/typecheck.ta" "$ty"; cp "$bak/bootstrap.tabc" "$tabc"; rm -rf "$bak"' EXIT

python3 - "$ty" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
old = """  // car: forall(a b, a -> b) — heterogeneous pairs are legal in TA;
  // the element type is unconstrained (matches null?/pair? permissiveness)
  let car_t = t_forall(cons(0, cons(1, nil)), t_arrow(t_var(0, 0), t_var(1, 0)))
  let e1 = extend(nil, 'car, car_t)
  // cdr: forall(a, a -> a)
  let e2 = extend(e1, 'cdr, car_t)
  // cons: forall(a b, a -> b -> b)
  let cons_t = t_forall(cons(0, cons(1, nil)),
  t_arrow(t_var(0, 0), t_arrow(t_var(1, 0), t_var(1, 0))))
  let e3 = extend(e2, 'cons, cons_t)"""
new = """  // PROBE: 列表版签名（scripts/cons-probe.sh 临时注入）
  let car_t = t_forall(cons(0, nil), t_arrow(t_app('List, cons(t_var(0, 0), nil)), t_var(0, 0)))
  let e1 = extend(nil, 'car, car_t)
  let cdr_t = t_forall(cons(0, nil), t_arrow(t_app('List, cons(t_var(0, 0), nil)), t_app('List, cons(t_var(0, 0), nil))))
  let e2 = extend(e1, 'cdr, cdr_t)
  let cons_t = t_forall(cons(0, nil),
  t_arrow(t_var(0, 0), t_arrow(t_app('List, cons(t_var(0, 0), nil)), t_app('List, cons(t_var(0, 0), nil)))))
  let e3 = extend(e2, 'cons, cons_t)"""
if old not in s:
    sys.exit("cons-probe: make_builtin_env 的 car/cdr/cons 段落已变，请同步本脚本")
open(p, 'w', encoding='utf-8').write(s.replace(old, new))
PY

# 第一趟：旧种子编出新编译器（必然成功 —— 做检查的是老检查器）
make bootstrap > /dev/null 2>&1

# 第二趟：新编译器 self-host，这一趟才报错
log=$(mktemp)
set +e
make bootstrap > "$log" 2>&1
set -e

total=$(grep -c '^  \[E[0-9]*\]' "$log" 2>/dev/null || true)
[ -z "$total" ] && total=0
echo "错误站点总数: $total"
if [ "$total" != 0 ]; then
    python3 - "$log" <<'PY'
import collections, re, sys
log = open(sys.argv[1], encoding='utf-8').read()
pref = collections.Counter()
for fn, n in collections.Counter(re.findall(r"in function '([^']+)'", log)).items():
    m = re.match(r"([A-Za-z0-9_]+)~\d+\$", fn)
    pref[m.group(1) if m else '(driver)' ] += n
for f, c in pref.most_common():
    print(f"{c:6d}  {f}")
PY
fi
rm -f "$log"