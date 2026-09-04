# -*- coding: utf-8 -*-
"""tc_oracle.py — L2 typecheck 双向 oracle (kernel-fuzzing DELIV-5 前半).

Implements docs/kernel-fuzzing-design.md §6.0-6.2:

  §6.0 三结局特征（实测冻结，见 FREEZE 注释块）
    accept : exit 0 且输出无 `type error(s)` 行
    reject : exit !=0 且输出含 `typecheck: N type error(s) found` 类行
    crash  : panic 文本 / 信号死亡 / ASan 报告 / build 超时（崩溃本身是 finding）

  §6.1 健全性方向（正例，每个 seed 的对照组）
    A1: gen 良构程序 P 必须被 `tinyactor build` accept（拒绝 → sound-fail finding）
    A2: 编译产物在 ASan tavm 上运行不得崩 VM（→ vm-crash finding）

  §6.2 完备性方向（负例 = 对 P 施加保类型破坏），5 类变异，全部在 gen 树上
    （复用 gen.build_program / transforms 的槽位机制，渲染一律 gen.render_tree）：

    lit_swap        字面量换型：算术/严格比较 Bin 的 int 字面量操作数 → "kernfuzz"
    fn_arg_mismatch 函数实参错配：type 子型（注 int 形参位的 Lit 实参 → 字符串）
                    或 arity 子型（顶层 helper 调用尾部追加 1 个 int 实参）
    undef_var       引用未定义变量：算术 Bin 的 int 字面量 → zz_undef_N 标识符
    ctor_field_type 构造器错配：field_type 子型（首参 → 字符串）或 arity 子型
                    （构造调用追加 1 参）
    exhaust         穷尽性（特殊类）：删除 main 中某 match 的 `_ ->` 通配臂

  断言三连（reject 方向）：exit !=0 且 输出含 `type error(s) found` 且 非 panic
  —— classify_build 三者缺一不可；exit!=0 但无该关键词归 tc-anomaly（可能是
  漏过 parse 前置校验的 parse error / 链接错误，不污染 reject 计数）。

  期望表（expectation）是 §6.0 "特征先实测再冻结" 的逐类扩展，全部来自
  2026-08-30 对当前 bootstrap（不动点已验证）的实测探针，见 FREEZE 注释块。
  冻结后行为漂移立即报警（tc-drift finding）——穷尽性类的双向守护即由此实现：
  当前编译器**没有** non-exhaustive warning（特征=accept 且无 warning），哪天
  warning 出现或 match 翻成 reject，本 oracle 会立即以 tc-drift 报警。

  变异前置校验（§6.2）：每个负例先过 parse（morph.Runner.dump / ast-dump 成功）
  ——意外 parse error 归独立 parse-reject 计数，不入 reject 断言分母。

  findings 对接：分类落盘复用 morph.record_finding / morph.load_known_signatures
  / morph.signature（同一契约，不另起炉灶）。类别（封闭枚举）：
    sound-fail | vm-crash | tc-crash | missed-reject | tc-drift | tc-anomaly

  本工具不修任何 typecheck/编译器 bug —— 全部落 findings + .pge/progress.md。

CLI:
    python3 tc_oracle.py --seeds A..B [--count N] [--out DIR]

Stdlib-only, host Python 3.  确定性：全部随机来自 prng.py（M-2 计数器流），
同参数两次运行 findings 签名集合与统计完全一致。
"""

import argparse
import os
import re
import shutil
import sys
import tempfile
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import gen                                    # noqa: E402
import prng                                   # noqa: E402
import transforms as tr                       # noqa: E402
import morph                                  # noqa: E402

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))

# ---------------------------------------------------------------------------
# §6.0 特征冻结表（FREEZE）
# ---------------------------------------------------------------------------
#
# 探针记录（2026-08-30，tinyactor = lib/bootstrap.tabc 当前产物；当日
# `make bootstrap` 复建产物与库内 byte-identical，即不动点成立，下列特征
# 描述的就是当前编译器而非陈旧工具链）：
#
#   良构样例  `fn main() { let x: int = 1 + 2; print(x); }`      → accept (exit 0)
#   类型错样例 `fn f(a: int) -> int { a }  fn main() { print(f("s")); }`
#       → exit 1，stdout:
#           typecheck: 1 type error(s) found
#             [E0001] in function 'main' (line 2): arg 1 of f: cannot unify string with int
#           compile aborted: 1 type error(s)
#         stderr 仅有 `error: build failed - no output produced (...)`
#   ⇒ reject 特征钉死为 stdout 含 `type error(s) found`（driver.ta:368），
#     不依赖 stderr（bash 包装层的 stderr 文案不属于编译器契约）。
#
#   crash 特征：panic 文本 / 信号死亡(rc<0) / 非 0/1 退出码 / ASan exitcode 42
#     / 超时。exit 1 是正常失败路径（build failed），不是 crash。
#
# 逐类期望探针（各用最小样例实测，源文本保留在注释里备查）：
#   lit_swap         `print((5 + "x"))`          → REJECT（cannot unify string with int）
#                    `print((5 < "x"))`          → REJECT；注意 `==`/`!=` 接受混合操作数
#                                                  （`print((5 == "x"))` → ACCEPT），
#                                                  故站点只取算术与严格比较 op。
#   fn_arg type      `fn f(a: int)->int{a}  print(f("s"))`      → REJECT
#   fn_arg arity     `fn f(a,b)->int{a+b}  print(f(1,2,3))`     → REJECT
#                    （注意少参不查：`f(1)` 双参 fn → ACCEPT，运行时出垃圾 —— 已知洞）
#   undef_var        `print((5 + undef_var))`    → ACCEPT（typecheck 未定义名不查，洞）
#   ctor field_type  `let c = Ci(3, "four")`     → ACCEPT（ctor 字段无注解，洞）
#   ctor arity       `let c = Ci(3, 4, 9)`       → REJECT（E0001 cannot unify）
#   exhaust          `let x = match "s" { "a"->1, "b"->2 };`     → ACCEPT
#                    且 stdout/stderr 均无 `non-exhaustive match`：
#                    当前编译器**没有**穷尽性 warning（parser 把 match 脱糖成
#                    嵌套 if，缺臂的 else 就是 nil；lib/parser.ta desugar_match_arms）。
#                    设计文档 §1.1/§6.2 描述的 warning 行为与实现不符 —— 记录在案。
#                    期望冻结为 accept 且无 warning；任一方向漂移 → tc-drift。
#
# 对照探针（完备性方向之外，供声音性方向健全性 sanity）：
#   `let x: int = "hello"` → ACCEPT（let 注解不校验，运行时打 nil）——已知洞，
#   gen 不产出 let 注解，本 oracle 不为其设变异类，仅在 progress.md 记录。

TYPE_ERR_MARK = b"type error(s) found"      # reject 特征（driver.ta:368）
EXHAUST_WARN_MARK = b"non-exhaustive match"  # 当前编译器不产出；出现即漂移
PANIC_MARK = b"panic"
ASAN_EXIT = morph.ASAN_EXIT                  # 42（复用 morph 钉死值）

ARITH_OPS = ("+", "-", "*")
STRICT_CMP_OPS = ("<", "<=", ">", ">=")      # == / != 接受混合操作数（探针）
MUT_STR = "kernfuzz"                         # 注入的字符串字面量
UNDEF_BASE = "zz_undef"                      # 未定义变量名前缀

# 变异类（§6.2 五类）与其子型的期望结局。accept-hole = 已知完备性洞：
# 当前编译器实测不查，期望 accept；哪天 reject 了就是行为漂移（tc-drift）。
CLASSES = ("lit_swap", "fn_arg_mismatch", "undef_var", "ctor_field_type",
           "exhaust")
SUB_EXPECT = {
    ("lit_swap", "arith_lit"): "reject",
    ("fn_arg_mismatch", "type"): "reject",
    ("fn_arg_mismatch", "arity"): "reject",
    ("undef_var", "arith_lit"): "accept-hole",
    ("ctor_field_type", "field_type"): "accept-hole",
    ("ctor_field_type", "arity"): "reject",
    ("exhaust", "drop_wildcard"): "accept-quiet",
}

# findings 类别（封闭枚举；落盘契约复用 morph）
FINDING_CATEGORIES = ("sound-fail", "vm-crash", "tc-crash", "missed-reject",
                      "tc-drift", "tc-anomaly")


class OracleError(Exception):
    """Infrastructure error — aborts the batch (mirrors morph.MorphError)."""


# ---------------------------------------------------------------------------
# §6.0 三结局分类器
# ---------------------------------------------------------------------------

def classify_build(res):
    """Classify one `tinyactor build` result per §6.0 frozen features.

    Returns "accept" | "reject" | "crash" | "anomaly".
      reject  = exit !=0 且 输出含 type error(s) found（断言三连的 2/3）
      crash   = panic 文本 / 信号 / 退出码 ∉ {0,1} / ASan 42 / 超时
      anomaly = exit !=0 但无 reject 关键词（漏网 parse error 等）
                或 exit 0 却带 type error 文案
    """
    combined = res.out + res.err
    if res.timed_out:
        return "crash"                       # build 超时 = finding，不静默
    if (res.rc is None or res.rc < 0 or res.rc > 1 or res.rc == ASAN_EXIT
            or PANIC_MARK in combined):
        return "crash"
    has_mark = TYPE_ERR_MARK in combined
    if res.rc != 0:
        return "reject" if has_mark else "anomaly"
    return "anomaly" if has_mark else "accept"


def classify_exhaust(res):
    """Exhaustiveness special class: (build classification, warning seen?)."""
    return classify_build(res), EXHAUST_WARN_MARK in (res.out + res.err)


# ---------------------------------------------------------------------------
# 树上变异（复用 gen.build_program 树 + transforms 槽位/路径机制）
# ---------------------------------------------------------------------------

class StrLit(tr.Expr):
    """A string-literal leaf for mutation injection (renders quoted)."""

    __slots__ = ()


def _render(e):
    """transforms.render_expr 的复刻，唯一扩展：StrLit 叶子。
    （不能直接委托 tr.render_expr：StrLit 会嵌在树中层，非根位置。）"""
    if isinstance(e, StrLit):
        return '"%s"' % MUT_STR
    if isinstance(e, tr.Lit):
        return tr.render_expr(e)
    if isinstance(e, tr.Var):
        return e.name
    if isinstance(e, tr.Call):
        return "%s(%s)" % (e.callee,
                           ", ".join(_render(a) for a in e.args))
    if isinstance(e, (tr.Bin, tr.Cmp)):
        return "(%s %s %s)" % (_render(e.left), e.op, _render(e.right))
    if isinstance(e, tr.If):
        return "if %s { %s } else { %s }" % (
            "true" if e.cond else "false",
            _render(e.then), _render(e.els))
    return tr.render_expr(e)


def _paths(root):
    """All pre-order paths (tuples of child selectors) of a slot tree."""
    out = []

    def rec(e, p):
        out.append(p)
        for sel, c in tr._children(e):
            rec(c, p + (sel,))

    rec(root, ())
    return out


def _splice(plan, slot, new_root):
    """transforms._splice 的复刻，渲染走 _render（支持 StrLit）。"""
    new_text = _render(new_root)
    new_line = slot.line[:slot.start] + new_text + slot.line[slot.end:]
    np = tr._copy_plan(plan)
    kind, idx, li = slot.sid[0], slot.sid[1], slot.sid[2]
    if kind == "main":
        lines = np.main_stmts[idx].split("\n")
        lines[li] = new_line
        np.main_stmts[idx] = "\n".join(lines)
    else:
        sig, src = np.fns[idx]
        lines = src.split("\n")
        lines[li] = new_line
        np.fns[idx] = (sig, "\n".join(lines))
    return np


def _splice_main_line(plan, stmt_idx, line_idx, new_line):
    np = tr._copy_plan(plan)
    lines = np.main_stmts[stmt_idx].split("\n")
    lines[line_idx] = new_line
    np.main_stmts[stmt_idx] = "\n".join(lines)
    return np


def _arith_lit_sites(plan):
    """(slot, path) 列表：算术/严格比较 Bin 的 int 字面量操作数。
    == / != 接受混合操作数（探针 c3），绝不入站点池。"""
    sites = []
    for slot in tr.find_int_slots(plan):
        for path in _paths(slot.root):
            n = tr._get(slot.root, path)
            if isinstance(n, (tr.Bin, tr.Cmp)):
                ok = (n.op in ARITH_OPS if isinstance(n, tr.Bin)
                      else n.op in STRICT_CMP_OPS)
                if not ok:
                    continue
                for side, ch in ((0, n.left), (1, n.right)):
                    if isinstance(ch, tr.Lit):
                        sites.append((slot, path + (side,)))
    return sites


def apply_lit_swap(plan, rng):
    """变异 1 字面量换型：int 字面量操作数 → "kernfuzz"（期望 reject）。"""
    sites = _arith_lit_sites(plan)
    if not sites:
        return None, {"sub": "arith_lit", "reason": "no-arith-literal-site"}
    slot, path = sites[prng.prng_next_range(rng, len(sites))]
    np = _splice(plan, slot, tr._set(slot.root, path, StrLit()))
    return np, {"sub": "arith_lit", "why": "int literal -> %r" % MUT_STR}


def _fn_param_int_positions(plan):
    """顶层 helper 名 → 渲染参数表中注 `: int` 的下标集合。"""
    out = {}
    for sig, src in plan.fns:
        m = re.search(r"fn %s\(([^)]*)\)" % re.escape(sig.name), src)
        if not m:
            continue
        pos = set()
        for i, p in enumerate(m.group(1).split(",")):
            if p.strip().endswith(": int"):
                pos.add(i)
        out[sig.name] = pos
    return out


def apply_fn_arg_mismatch(plan, rng):
    """变异 2 函数实参错配。优先 type 子型（注 int 形参位的 Lit 实参 →
    字符串），无则 arity 子型（顶层 helper 调用尾部 +1 int 实参）。
    两子型当前编译器均 REJECT（冻结探针）。"""
    types = tr._Types(plan)
    annot = _fn_param_int_positions(plan)
    type_sites = []
    arity_sites = []
    for slot in tr.find_int_slots(plan):
        for path in _paths(slot.root):
            n = tr._get(slot.root, path)
            if not isinstance(n, tr.Call):
                continue
            if n.callee in annot:
                for i, a in enumerate(n.args):
                    if i in annot[n.callee] and isinstance(a, tr.Lit):
                        type_sites.append((slot, path + (i,)))
                        break
            if n.callee in types.helpers:
                arity_sites.append((slot, path))
    if type_sites:
        slot, path = type_sites[prng.prng_next_range(rng, len(type_sites))]
        np = _splice(plan, slot, tr._set(slot.root, path, StrLit()))
        return np, {"sub": "type", "why": "annotated-int arg -> %r" % MUT_STR}
    if arity_sites:
        slot, path = arity_sites[prng.prng_next_range(rng, len(arity_sites))]
        call = tr._get(slot.root, path)
        np = _splice(plan, slot,
                     tr._set(slot.root, path,
                             tr.Call(call.callee,
                                     list(call.args) + [tr.Lit(7)],
                                     call.frozen)))
        return np, {"sub": "arity", "why": "helper call +1 int arg"}
    return None, {"sub": None, "reason": "no-helper-call-site"}


def _fresh_undef_name(plan):
    src = gen.render_tree(plan)
    i = 0
    while re.search(r"\b%s_%d\b" % (UNDEF_BASE, i), src):
        i += 1
    return "%s_%d" % (UNDEF_BASE, i)


def apply_undef_var(plan, rng):
    """变异 3 引用未定义变量：int 字面量操作数 → 未定义标识符。
    当前编译器 ACCEPT（冻结的完备性洞）→ expect accept-hole。"""
    sites = _arith_lit_sites(plan)
    if not sites:
        return None, {"sub": "arith_lit", "reason": "no-arith-literal-site"}
    slot, path = sites[prng.prng_next_range(rng, len(sites))]
    name = _fresh_undef_name(plan)
    np = _splice(plan, slot, tr._set(slot.root, path, tr.Var(name)))
    return np, {"sub": "arith_lit", "why": "literal -> undefined %s" % name}


_LET_CTOR_RE = re.compile(r"^(\s*let [A-Za-z_]\w* = )"
                          r"([A-Za-z_]\w*)(\((.*)\))?(;.*)?$")


def _ctor_lines(plan):
    """main 中 `let x = Ctor(...)` / 零字段裸名 `let x = Ctor` 的行。"""
    types = tr._Types(plan)
    out = []
    for idx, stmt in enumerate(plan.main_stmts):
        for li, line in enumerate(stmt.split("\n")):
            m = _LET_CTOR_RE.match(line)
            if m and m.group(2) in types.ctors:
                out.append((idx, li, m))
    return out


def _split_top_args(s):
    """按顶层逗号切参（括号深度感知；空串 → []）。"""
    if not s.strip():
        return []
    parts, depth, cur = [], 0, []
    for ch in s:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur).strip())
    return parts


def apply_ctor_field_type(plan, rng):
    """变异 4 构造器错配。field_type 子型（首实参 → "kernfuzz"，期望
    accept-hole）或 arity 子型（+1 实参，期望 reject，无字段型站点时的
    严格可校验代理）。"""
    lines = _ctor_lines(plan)
    if not lines:
        return None, {"sub": None, "reason": "no-ctor-call"}
    idx, li, m = lines[prng.prng_next_range(rng, len(lines))]
    prefix, name, paren, args_s, suffix = (m.group(1), m.group(2),
                                           m.group(3), m.group(4),
                                           m.group(5) or "")
    if paren and _split_top_args(args_s):
        args = _split_top_args(args_s)
        if prng.prng_next_range(rng, 2) == 0:
            args[0] = '"%s"' % MUT_STR
            sub = "field_type"
        else:
            args.append("7")
            sub = "arity"
        new_line = "%s%s(%s)%s" % (prefix, name, ", ".join(args), suffix)
    elif prng.prng_next_range(rng, 2) == 0:
        # 零字段裸名构造器没有字段可错配 → 只能走 arity 子型
        new_line = "%s%s(7)%s" % (prefix, name, suffix)
        sub = "arity"
    else:
        return None, {"sub": None, "reason": "no-ctor-field-site"}
    return _splice_main_line(plan, idx, li, new_line), {
        "sub": sub, "why": "ctor %s mismatch" % sub}


_WILDCARD_RE = re.compile(r"^\s*_\s*->")


def apply_exhaust(plan, rng):
    """变异 5（特殊类）穷尽性：删除 main 中某 match 块的 `_ ->` 通配臂。
    gen R5 保证每个 match 都有通配臂。期望 accept-quiet（冻结）。"""
    meta = list(getattr(plan, "match_meta", []) or [])
    if not meta:
        return None, {"sub": "drop_wildcard", "reason": "no-match-stmt"}
    stmt_i = meta[prng.prng_next_range(rng, len(meta))][0]
    lines = plan.main_stmts[stmt_i].split("\n")
    widx = [k for k, ln in enumerate(lines) if _WILDCARD_RE.match(ln)]
    if not widx:
        return None, {"sub": "drop_wildcard", "reason": "no-wildcard-arm"}
    np = tr._copy_plan(plan)
    new_lines = list(lines)
    del new_lines[widx[-1]]
    np.main_stmts[stmt_i] = "\n".join(new_lines)
    return np, {"sub": "drop_wildcard",
                "why": "dropped `_ ->` arm (stmt %d)" % stmt_i}


MUTATORS = {
    "lit_swap": apply_lit_swap,
    "fn_arg_mismatch": apply_fn_arg_mismatch,
    "undef_var": apply_undef_var,
    "ctor_field_type": apply_ctor_field_type,
    "exhaust": apply_exhaust,
}


# ---------------------------------------------------------------------------
# runner 集成（build/run/dump 全部复用 morph.Runner）
# ---------------------------------------------------------------------------

def _write_src(runner, text, tag):
    sp = os.path.join(runner.workdir, "src_%s.ta" % tag)
    ap = os.path.join(runner.workdir, "src_%s.tabc" % tag)
    with open(sp, "wb") as f:
        f.write(text.encode("latin-1"))
    return sp, ap


def parse_precheck(runner, src_text, tag):
    """§6.2 变异前置校验：mutant 必须先过 parse（ast-dump 成功）。
    Returns (True, None) 或 (False, dump RunResult)。"""
    sp, _ap = _write_src(runner, src_text, tag)
    res = runner.dump(sp)
    if (res.timed_out or res.rc != 0 or b"AST-DUMP-ERROR" in res.out
            or not res.out):
        return False, res
    return True, res


def _record(out_dir, category, programs, seed, dedup, findings):
    """morph.record_finding 薄封装；dedup 命中返回 False。"""
    ok = morph.record_finding(out_dir, category,
                              programs[0]["src_text"], seed, seed, 1, [],
                              programs, {}, dedup)
    if ok:
        findings[category] += 1
    return ok


def run_seed(runner, seed, out_dir, dedup, skips, findings, classes, log):
    """一个 seed = 对照组健全性（A1/A2）+ 5 类完备性负例。
    Returns "ok" | "skip:<reason>" | "finding:<category>". """
    plan = gen.build_program(seed)
    src0 = gen.render_tree(plan)

    # ---- §6.1 A1: 对照组必须 accept（防生成器垃圾造成假绿灯）------------
    sp0, ap0 = _write_src(runner, src0, "P")
    bp0 = runner.build(sp0, ap0)
    prog0 = [{"tag": "P", "src_text": src0, "res": bp0,
              "build_err": (bp0.out + bp0.err) if bp0.rc != 0 else b""}]
    cat0 = classify_build(bp0)
    if cat0 != "accept":
        rec = "tc-crash" if cat0 == "crash" else "sound-fail"
        skips.append((seed, "control:%s" % cat0))
        _record(out_dir, rec, prog0, seed, dedup, findings)
        return "finding:" + rec

    # ---- §6.1 A2: 编译+运行不得崩 VM ------------------------------------
    rp0 = runner.run(ap0)
    if morph.classify_run(rp0) is not None:
        prog0 = [{"tag": "P", "src_text": src0, "res": rp0, "build_err": b""}]
        skips.append((seed, "control:vm-crash"))
        _record(out_dir, "vm-crash", prog0, seed, dedup, findings)
        return "finding:vm-crash"

    # ---- §6.2 五类负例 ---------------------------------------------------
    for ci, cls in enumerate(CLASSES):
        rng = prng.make_prng(prng.derive_seed(seed, 7000 + ci))
        bucket = classes[cls]
        # 类内重 roll：主 P 无该类站点时，换子 seed 派生的良构 P' 找站点
        #（每个 P' 都先过自己的 A1 对照 build；≤3 次重 roll）。
        mplan = minfo = None
        for k in range(4):
            cand = plan if k == 0 else gen.build_program(
                prng.derive_seed(seed, 9000 + ci * 10 + k))
            mplan, minfo = MUTATORS[cls](cand, rng)
            if mplan is not None:
                if k:
                    csrc = gen.render_tree(cand)
                    csp, cap = _write_src(runner, csrc, "c%d_%d" % (ci, k))
                    cbp = runner.build(csp, cap)
                    ccat = classify_build(cbp)
                    if ccat != "accept":
                        cprog = [{"tag": "c%d_%d" % (ci, k),
                                  "src_text": csrc, "res": cbp,
                                  "build_err": (cbp.out + cbp.err)
                                  if cbp.rc != 0 else b""}]
                        rec = "tc-crash" if ccat == "crash" else "sound-fail"
                        _record(out_dir, rec, cprog, seed, dedup, findings)
                        mplan = None
                        break
                break
        if mplan is None:
            bucket["skip"] += 1
            skips.append((seed, "%s:no-site-after-reroll" % cls))
            continue
        msrc = gen.render_tree(mplan)
        ok, _dres = parse_precheck(runner, msrc, "m%d" % ci)
        if not ok:
            # 意外 parse error：独立类别，不入 reject 断言分母（§6.2）。
            # instances 只统计真正走到 typecheck 断言的变异体。
            bucket["parse-reject"] += 1
            continue
        bucket["instances"] += 1
        spm, apm = _write_src(runner, msrc, "m%d" % ci)
        mbp = runner.build(spm, apm)
        cat = classify_build(mbp)
        prog = [{"tag": "m%d_%s" % (ci, minfo.get("sub")), "src_text": msrc,
                 "res": mbp,
                 "build_err": (mbp.out + mbp.err) if mbp.rc != 0 else b""}]
        expect = SUB_EXPECT[(cls, minfo["sub"])]
        if cat == "crash":
            _record(out_dir, "tc-crash", prog, seed, dedup, findings)
        elif cat == "anomaly":
            _record(out_dir, "tc-anomaly", prog, seed, dedup, findings)
        elif expect == "reject":
            if cat == "reject":
                bucket["rejected"] += 1
            else:                        # 期望 reject 却 accept → 假阴性洞
                _record(out_dir, "missed-reject", prog, seed, dedup,
                        findings)
        elif expect == "accept-hole":
            if cat == "reject":
                # 冻结的洞突然被查了 → 行为漂移报警（双向守护）
                _record(out_dir, "tc-drift", prog, seed, dedup, findings)
            else:
                bucket["known-hole"] += 1
        else:                            # accept-quiet（exhaust 特殊类）
            _cat, warned = classify_exhaust(mbp)
            if _cat != "accept" or warned:
                # 变 reject 或 warning 出现 → 行为漂移立即报警
                _record(out_dir, "tc-drift", prog, seed, dedup, findings)
            else:
                bucket["exhaust-ok"] += 1
    return "ok"


def fuzz_batch(runner, seeds, out_dir, log=None):
    """主循环。Returns stats dict（确定性：同参数逐字段一致，除 elapsed）。"""
    if log is None:
        log = lambda msg: None                  # noqa: E731
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    dedup = morph.load_known_signatures(out_dir)
    findings = dict((c, 0) for c in FINDING_CATEGORIES)
    classes = dict((c, {"instances": 0, "skip": 0, "parse-reject": 0,
                        "rejected": 0, "known-hole": 0, "exhaust-ok": 0})
                   for c in CLASSES)
    skips = []
    positives = 0
    t0 = time.time()
    for seed in seeds:
        outcome = run_seed(runner, seed, out_dir, dedup, skips, findings,
                           classes, log)
        if outcome == "ok":
            positives += 1
        log("seed %d -> %s" % (seed, outcome))
    with open(os.path.join(out_dir, "skips.log"), "a",
              encoding="latin-1") as f:
        for seed, reason in skips:
            f.write("seed=%d reason=%s\n" % (seed, reason))
    return {
        "seeds": len(seeds),
        "positives": positives,
        "skips": skips,
        "classes": classes,
        "findings": findings,
        "elapsed": time.time() - t0,
    }


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_seeds(spec):
    """'N' → [N];  'A..B' → list(range(A, B+1)) (inclusive).  复用 morph 语义."""
    return morph.parse_seeds(spec)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="L2 typecheck 双向 oracle (kernel-fuzzing §6, DELIV-5 前半)")
    ap.add_argument("--seeds", required=True,
                    help="single seed N or inclusive range A..B")
    ap.add_argument("--count", type=int, default=None,
                    help="only run the first N seeds of the range")
    ap.add_argument("--out", default=os.path.join(_HERE, "build",
                                                  "tc-findings"),
                    help="findings output dir (default gitignored)")
    args = ap.parse_args(argv)

    seeds = parse_seeds(args.seeds)
    if args.count is not None:
        seeds = seeds[:args.count]

    workdir = tempfile.mkdtemp(prefix="tc-oracle-run-")
    try:
        runner = morph.Runner(workdir)
        stats = fuzz_batch(runner, seeds, args.out,
                           log=lambda m: sys.stderr.write(m + "\n"))
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    fk = stats["findings"]
    cls = stats["classes"]
    inst = sum(b["instances"] for b in cls.values())
    rej = sum(b["rejected"] for b in cls.values())
    hole = sum(b["known-hole"] for b in cls.values())
    prj = sum(b["parse-reject"] for b in cls.values())
    sys.stderr.write(
        "tc_oracle summary: seeds=%d positives(A1+A2)=%d seed_skips=%d "
        "negatives=%d (rejected=%d parse-reject=%d known-hole=%d) "
        "findings=%d {%s} elapsed=%.1fs\n"
        % (stats["seeds"], stats["positives"], len(stats["skips"]), inst,
           rej, prj, hole, sum(fk.values()),
           ", ".join("%s=%d" % (c, fk[c]) for c in FINDING_CATEGORIES),
           stats["elapsed"]))
    for c in CLASSES:
        sys.stderr.write("  %-16s %s\n" % (c, cls[c]))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except morph.MorphError as e:
        sys.stderr.write("tc_oracle: error: %s\n" % e)
        sys.exit(2)