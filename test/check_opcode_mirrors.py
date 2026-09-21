#!/usr/bin/env python3
"""Check that the hand-maintained opcode numbering mirrors agree.

An opcode number lives in five places that no compiler keeps in sync:

  1. the `OpCode` enum in ta.h                           (authoritative)
  2. the `op_*` consts in lib/bootstrap/codegen.ta        (mirror: name -> value)
  3. the computed-goto table in src/vm.c                  (entries keyed by name)
  4. the `CASE()` arms of the switch backend in src/vm.c  (handlers, keyed by name)
  5. the `instr_len` table in src/api.c                   (row order == number)

A miss in 1/2/5 is silent at compile time, and a wrong `instr_len` row makes the
multi-module rebase scan walk off the instruction stream, so renumbering has to
touch all five together. Run this from the repo root or via `make check-opcodes`.

Exit status: 0 when every mirror agrees, 1 otherwise, listing each difference.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
failures = []


def read(rel):
    return (ROOT / rel).read_text()


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


def report(section, detail, problems, ok_note=""):
    """Print one mirror check: `detail` is the observed count, `problems` the
    differences (empty when the mirror agrees, in which case `ok_note` states
    what was verified)."""
    ok = not problems
    print(f"[{'ok' if ok else 'FAIL'}] {section}: {detail}" + (f" — {ok_note}" if ok else f"; {problems}"))
    if not ok:
        failures.append(f"{section}: {detail}; {problems}")


def parse_enum():
    """The OpCode enum in ta.h -> ([(name, value)], OP_COUNT)."""
    raw = read("ta.h")
    body = re.search(r"typedef enum \{(.*?)\}\s*OpCode\s*;", raw, re.S)
    if not body:
        sys.exit("FAIL: could not locate the OpCode enum in ta.h")

    enum, counter = [], 0
    for tok in re.finditer(r"(OP_[A-Z0-9_]+)\s*(?:=\s*(\d+))?", strip_comments(body.group(1))):
        name, explicit = tok.group(1), tok.group(2)
        if name == "OP_COUNT":
            return enum, counter if explicit is None else int(explicit)
        if explicit is not None:
            counter = int(explicit)
        enum.append((name, counter))
        counter += 1
    sys.exit("FAIL: OP_COUNT not found in the ta.h enum")


# ---- 1. ta.h enum (authoritative numbering) --------------------------------
enum, op_count = parse_enum()
enum_names = [n for n, _ in enum]
enum_val = dict(enum)
values = sorted(enum_val.values())

report(
    "ta.h enum",
    f"{len(enum)} opcodes, OP_COUNT={op_count}",
    None
    if values == list(range(op_count)) and len(enum) == op_count
    else {"values": values, "expected": f"0..{op_count - 1} without holes or duplicates"},
    ok_note=f"values are exactly 0..{op_count - 1}",
)

# ---- 2. lib/bootstrap/codegen.ta consts ------------------------------------
consts = {
    name: int(value)
    for name, value in re.findall(
        r"^const (op_[a-z0-9_]+)\s*=\s*(\d+)", read("lib/bootstrap/codegen.ta"), re.M
    )
}
mirror_expected = {name.lower(): value for name, value in enum_val.items()}
problems = {
    "missing": sorted(set(mirror_expected) - set(consts)),
    "stale": sorted(set(consts) - set(mirror_expected)),
    "value mismatch [const: enum vs const]": {
        name: (mirror_expected[name], consts[name])
        for name in mirror_expected.keys() & consts.keys()
        if mirror_expected[name] != consts[name]
    },
}
report(
    "codegen.ta consts",
    f"{len(consts)} consts, {len(enum)} enum members",
    {kind: bad for kind, bad in problems.items() if bad},
    ok_note="names and values 1:1 with the enum",
)

# ---- 3. src/vm.c computed-goto table ---------------------------------------
vm = read("src/vm.c")
table = re.findall(r"\[(OP_[A-Z0-9_]+)\]\s*=\s*&&(CASE_OP_[A-Z0-9_]+)", vm)
keys = [key for key, _ in table]
problems = {
    "entry count": f"{len(table)}, expected OP_COUNT={op_count}"
    if len(table) != op_count
    else "",
    "duplicate keys": sorted({k for k in keys if keys.count(k) > 1}),
    "missing keys (NULL slot)": sorted(set(enum_names) - set(keys)),
    "unknown keys": sorted(set(keys) - set(enum_val)),
    "key/label mismatch": [(key, label) for key, label in table if label != "CASE_" + key],
}
report(
    "vm.c goto table",
    f"{len(table)} entries, OP_COUNT={op_count}",
    {kind: bad for kind, bad in problems.items() if bad},
    ok_note="1:1 with the enum, every label matches its key (no NULL slot)",
)

# ---- 4. src/vm.c switch CASE arms ------------------------------------------
arms = re.findall(r"CASE\((OP_[A-Z0-9_]+)\)", strip_comments(vm))
problems = {
    "arm count": f"{len(arms)}, expected OP_COUNT={op_count}" if len(arms) != op_count else "",
    "duplicate arms": sorted({a for a in arms if arms.count(a) > 1}),
    "missing arms": sorted(set(enum_names) - set(arms)),
    "unknown arms": sorted(set(arms) - set(enum_val)),
    "table keys without an arm": sorted(set(keys) - set(arms)),
}
report(
    "vm.c CASE arms",
    f"{len(arms)} arms, OP_COUNT={op_count}",
    {kind: bad for kind, bad in problems.items() if bad},
    ok_note="1:1 with the enum, every table key has a handler",
)

# ---- 5. src/api.c instr_len (keyed by row position) ------------------------
api = read("src/api.c")
table_body = re.search(r"instr_len\[OP_COUNT\]\s*=\s*\{(.*?)\n\};", api, re.S)
if not table_body:
    sys.exit("FAIL: could not locate the instr_len table in src/api.c")

rows, data_rows = [], 0  # rows: [(length, annotated slot, name)] in physical order
for line in table_body.group(1).splitlines():
    if re.match(r"\s*\d+\s*,", line):
        data_rows += 1
    row = re.match(r"\s*(\d+),\s*/\*\s*(\d+)\s+(OP_[A-Z0-9_]+)", line)
    if row:
        rows.append((int(row.group(1)), int(row.group(2)), row.group(3)))

# `instr_len[op]` must hold the entry for opcode number `op`, so physical row i
# has to be the row annotated `/* i OP_x */`. Comparing the annotations with the
# enum alone is not enough: a reordered table moves the rows and their comments
# together, which is exactly the silent failure this table is prone to.
problems = {
    "row count": f"{len(rows)} annotated / {data_rows} data rows, expected {op_count}"
    if data_rows != op_count or len(rows) != op_count
    else "",
    "row order [row: annotated slot, name]": [
        (i, slot, name) for i, (_, slot, name) in enumerate(rows) if slot != i
    ],
    "annotation vs enum [slot: name, enum value]": [
        (slot, name, enum_val.get(name)) for _, slot, name in rows if enum_val.get(name) != slot
    ],
}
report(
    "api.c instr_len",
    f"{len(rows)} rows, OP_COUNT={op_count}",
    {kind: bad for kind, bad in problems.items() if bad},
    ok_note="row i is opcode i, and the annotation agrees with the enum",
)

# variable-length rows store 0 as a sentinel; rebase_code must special-case them
# or its `pc += instr_len[op]` fallback stalls on a zero advance forever.
rebase = re.search(r"static void rebase_code\(.*?\n\}\n", api, re.S)
rebase_cases = set(re.findall(r"case (OP_[A-Z0-9_]+):", rebase.group(0))) if rebase else set()
variable_len = sorted(name for length, _, name in rows if length == 0)
report(
    "api.c rebase_code",
    f"variable-length rows {variable_len}",
    None
    if rebase and set(variable_len) <= rebase_cases
    else {"missing cases": sorted(set(variable_len) - rebase_cases)},
    ok_note="each has a dedicated case (a 0 advance would stall the scan)",
)

# ---- summary ---------------------------------------------------------------
print()
if failures:
    print(f"RESULT: FAIL — {len(failures)} inconsistent mirror(s)")
    for failure in failures:
        print(f"  - {failure}")
    sys.exit(1)

print(
    f"RESULT: PASS — {len(enum)} opcodes numbered 0..{op_count - 1} agree across "
    "ta.h, codegen.ta, vm.c (goto + switch) and api.c"
)