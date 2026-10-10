#!/usr/bin/env python3
"""check_opcode_mirrors.py — 后端 opcode 镜像守卫。

lib/bootstrap/compile.ta 的 `const OP_* = N` 是编码唯一权威；
src/tavm.c 的 `enum { LOP_* }` 是镜像副本，两边必须逐条同号同名。
名字规则：剥掉前缀后完全一致（OP_CONST ↔ LOP_CONST）。
LOP_COUNT 是哨兵不参与逐条比对，但其值必须等于 OP 常量个数。

用法：python3 tools/check_opcode_mirrors.py
退出码：0 = 镜像一致；1 = 有漂移（打印明细）。
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
COMPILE_TA = ROOT / "lib" / "bootstrap" / "compile.ta"
TAVM_C = ROOT / "src" / "tavm.c"

TA_RE = re.compile(r"^const (OP_[A-Z0-9_]+) = (\d+)$")
C_RE = re.compile(r"^\s*(LOP_[A-Z0-9_]+)\s*(?:=\s*(\d+))?\s*,?\s*$")


def parse_ta(path):
    """compile.ta：返回 [(name, num, lineno)]，按出现顺序。"""
    ops = []
    for i, line in enumerate(path.read_text().splitlines(), 1):
        m = TA_RE.match(line)
        if m:
            ops.append((m.group(1), int(m.group(2)), i))
    return ops


def parse_c_enum(path):
    """tavm.c：找含 LOP_CONST 的 enum 块，逐条累计编号（隐式自增）。"""
    lines = path.read_text().splitlines()
    starts = [i for i, l in enumerate(lines) if re.match(r"\s*enum\b.*\{", l)]
    for start in starts:
        block = []
        for j in range(start + 1, len(lines)):
            if lines[j].strip().startswith("}"):
                break
            block.append((j, lines[j]))
        if not any(re.match(r"\s*LOP_CONST\b", l) for _, l in block):
            continue
        entries = []
        num = 0
        for j, l in block:
            m = C_RE.match(l)
            if not m:
                continue
            name = m.group(1)
            if m.group(2) is not None:
                num = int(m.group(2))
            entries.append((name, num, j + 1))
            num += 1
        return entries
    raise SystemExit(f"check_opcode_mirrors: {TAVM_C}: 未找到 LOP enum")


def main():
    ta = parse_ta(COMPILE_TA)
    c = parse_c_enum(TAVM_C)

    errors = []
    sentinel = [e for e in c if e[0] == "LOP_COUNT"]
    c_ops = [e for e in c if e[0] != "LOP_COUNT"]

    if len(ta) != len(c_ops):
        errors.append(
            f"条目数不等：compile.ta {len(ta)} 个 OP_* vs "
            f"tavm.c {len(c_ops)} 个 LOP_*（不含 LOP_COUNT）"
        )

    for (tname, tnum, tline), (cname, cnum, cline) in zip(ta, c_ops):
        want = "L" + tname  # OP_CONST -> LOP_CONST
        if cname != want:
            errors.append(
                f"名字不镜像：compile.ta:{tline} {tname}({tnum}) "
                f"vs tavm.c:{cline} {cname}({cnum})"
            )
        elif tnum != cnum:
            errors.append(
                f"编号不镜像：compile.ta:{tline} {tname}={tnum} "
                f"vs tavm.c:{cline} {cname}={cnum}"
            )

    if sentinel:
        _, count, sline = sentinel[0]
        if count != len(ta):
            errors.append(
                f"哨兵不符：tavm.c:{sline} LOP_COUNT={count} "
                f"但 compile.ta 有 {len(ta)} 个 OP_*"
            )
    else:
        errors.append("tavm.c 的 enum 里没有 LOP_COUNT 哨兵")

    if errors:
        print("check_opcode_mirrors: FAIL")
        for e in errors:
            print(f"  - {e}")
        return 1
    print(f"check_opcode_mirrors: OK ({len(ta)} opcodes mirrored)")
    return 0


if __name__ == "__main__":
    sys.exit(main())