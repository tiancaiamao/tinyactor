#!/usr/bin/env python3
"""check_opcode_mirrors.py — opcode 编号镜像门。

编号的唯一权威是 lib/bootstrap/compile.ta 的 `const OP_* = N`（它决定 .tabc
里写什么字节）；ta.h 的 `enum { LOP_* }` 是 C 侧的镜像副本（解释器 vm.c 的
dispatch[]、装载器 main.c 的 g_optlen[] 都按它索引）。编译器读不到 C 头、
C 读不到 .ta，所以这两份是必然的 —— 这个门就是它们之间的一致性检查。

两条规则：
  1. 同号同名：compile.ta 的 OP_X = N 必须对应 ta.h 的 LOP_X = N
     （剥前缀后完全一致）。哪边漏改都会在这里红。
  2. 留洞具名：compile.ta 跳过的编号，ta.h 只能写成 `LOP_RESERVED_<编号>`
     （如 LOP_RESERVED_28 = 退休的 OP_BUILTIN 槽）—— 洞必须是「具名的洞」，
     不是随手少一个枚举项。LOP_COUNT 是哨兵，必须等于最大 opcode + 1。

用法：python3 tools/check_opcode_mirrors.py
退出码：0 = 镜像一致；1 = 有漂移（打印明细）。
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
COMPILE_TA = ROOT / "lib" / "bootstrap" / "compile.ta"
TA_H = ROOT / "ta.h"

TA_RE = re.compile(r"^const (OP_[A-Z0-9_]+) = (\d+)$")
# 允许行尾注释（`LOP_CONST_I, /* = OP_CONST_I */`）与末项无逗号（LOP_COUNT 后是 `};`）
C_RE = re.compile(r"^(LOP_[A-Z0-9_]+)\s*(?:=\s*(\d+))?\s*,?\s*(?:/\*.*)?$")


def parse_ta():
    """compile.ta → {编号: (名字, 行号)}（权威侧，显式赋值不靠自增）。"""
    out = {}
    for line_no, line in enumerate(COMPILE_TA.read_text().splitlines(), 1):
        m = TA_RE.match(line)
        if m:
            out[int(m.group(2))] = (m.group(1), line_no)
    return out


def parse_c_enum():
    """ta.h 的 LOP_* enum → ({编号: (名字, 行号)}, LOP_COUNT 的 (值, 行号))。

    从 `LOP_CONST = 0` 那行起扫到 `}`，隐式编号按 C 规则自增（显式 `= N` 重置）。"""
    lines = TA_H.read_text().splitlines()
    start = next(
        (i for i, l in enumerate(lines) if re.match(r"\s*LOP_CONST\s*=\s*0\b", l)),
        None,
    )
    if start is None:
        raise SystemExit("check_opcode_mirrors: ta.h 里找不到 LOP_CONST = 0 的 enum")
    entries = {}
    sentinel = None
    num = 0
    for i in range(start, len(lines)):
        s = lines[i].strip()
        if s.startswith("}"):
            break
        m = C_RE.match(s)
        if not m:
            continue
        if m.group(2) is not None:
            num = int(m.group(2))
        if m.group(1) == "LOP_COUNT":
            sentinel = (num, i + 1)
        else:
            entries[num] = (m.group(1), i + 1)
        num += 1
    if sentinel is None:
        raise SystemExit("check_opcode_mirrors: ta.h 的 LOP_* enum 里没有 LOP_COUNT 哨兵")
    return entries, sentinel


def main():
    ta = parse_ta()
    c_ops, sentinel = parse_c_enum()
    errors = []

    for num in sorted(ta):
        tname, tline = ta[num]
        want = "L" + tname  # OP_X → LOP_X
        got = c_ops.get(num)
        if got is None:
            errors.append(f"编号 {num}: compile.ta:{tline} {tname} 在 ta.h 里没有同号条目")
        elif got[0] != want:
            errors.append(
                f"编号 {num}: compile.ta:{tline} {tname} vs ta.h:{got[1]} {got[0]}"
                f"（C 侧应为 {want}）"
            )

    holes = 0
    for num in sorted(c_ops):
        if num in ta:
            continue
        holes += 1
        cname, cline = c_ops[num]
        want = f"LOP_RESERVED_{num}"  # 洞必须具名，否则少一个枚举项也看不出来
        if cname != want:
            errors.append(
                f"编号 {num}: ta.h:{cline} {cname} 在 compile.ta 里没有对应 opcode，"
                f"留洞必须写成 {want}"
            )

    top = max(ta) + 1
    if sentinel[0] != top:
        errors.append(
            f"哨兵不符：ta.h:{sentinel[1]} LOP_COUNT={sentinel[0]}，"
            f"但最大 opcode 是 {top - 1}（应为 {top}）"
        )

    if errors:
        print("check_opcode_mirrors: FAIL")
        for e in errors:
            print(f"  - {e}")
        return 1
    print(
        f"check_opcode_mirrors: OK ({len(ta)} opcodes mirrored, {holes} reserved slots)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())