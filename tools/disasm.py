#!/usr/bin/env python3
"""
.tabc bytecode disassembler.

Usage:
    tools/disasm.py <file>.tabc
    tools/disasm.py <file>.ta   # disassemble the .tabc next to it
    tools/disasm.py -d <file>.ta  # compile + disassemble (if tinyactor is built)
"""

import struct
import sys
import os

# ── Opcode info ──────────────────────────────────────────────
# (name, has_imm, imm_desc)
# has_imm: 0=none, 1=u32, 2=i8, 3=i64, 4=str(len+data), 5=u32*2, 6=u32+u8

OPCODES = {
    0:  ("PUSH_NIL",     0, ""),
    1:  ("PUSH_TRUE",    0, ""),
    2:  ("PUSH_FALSE",   0, ""),
    3:  ("PUSH_INT8",    2, "i8"),
    4:  ("PUSH_INT",     3, "i64"),
    5:  ("PUSH_SYM",     1, "sym"),
    6:  ("PUSH_STRING",  4, "str"),
    7:  ("LOAD",         1, "slot"),
    8:  ("STORE",        1, "slot"),
    9:  ("CONS",         0, ""),
    10: ("CAR",          0, ""),
    11: ("CDR",          0, ""),
    12: ("ADD",          0, ""),
    13: ("SUB",          0, ""),
    14: ("MUL",          0, ""),
    15: ("DIV",          0, ""),
    16: ("MOD",          0, ""),
    17: ("EQ",           0, ""),
    18: ("LT",           0, ""),
    19: ("LE",           0, ""),
    20: ("IS_NIL",       0, ""),
    21: ("IS_PAIR",      0, ""),
    22: ("IS_INT",       0, ""),
    23: ("IS_STRING",    0, ""),
    24: ("IS_BYTES",     0, ""),
    25: ("IS_PID",       0, ""),
    26: ("JUMP",         1, "addr"),
    27: ("JUMP_IF_FALSE",1, "addr"),
    28: ("POP",          0, ""),
    29: ("DUP",          0, ""),
    30: ("CLOSURE",      5, "fn_id, nfree, [slots...]"),  # u32 fn_id, u32 nfree, nfree*u32 slots
    31: ("CALL",         1, "nargs"),
    32: ("TAIL_CALL",    1, "nargs"),
    33: ("RET",          0, ""),
    34: ("SPAWN",        1, "fn_id"),
    35: ("SPAWN_MAIN",   1, "fn_id"),
    36: ("SPAWN_CLOS",   0, ""),
    37: ("SEND",         0, ""),
    38: ("RECV",         0, ""),
    39: ("RECV_PEEK",    0, ""),
    40: ("RECV_COMMIT",  0, ""),
    41: ("SELF",         0, ""),
    42: ("MONITOR",      0, ""),
    43: ("PRINT",        0, ""),
    44: ("HALT",         0, ""),
    45: ("MATCH_INT",    3, "i64"),
    46: ("MATCH_SYM",    1, "sym"),
    47: ("MATCH_NIL",    0, ""),
    48: ("MATCH_PAIR",   0, ""),
    49: ("MATCH_JUMP",   1, "addr"),
    50: ("STR_LEN",      0, ""),
    51: ("STR_CONCAT",   0, ""),
    52: ("STR_SLICE",    0, ""),
    53: ("STR_EQ",       0, ""),
    54: ("CCALL",        6, "cfunc_idx, nargs"),
    55: ("ENTER",        1, "nlocals"),
}

def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]

def i32(data, offset):
    return struct.unpack_from('<i', data, offset)[0]

def i64(data, offset):
    return struct.unpack_from('<q', data, offset)[0]

def i8(data, offset):
    return struct.unpack_from('<b', data, offset)[0]

def disasm(code, syms, code_base=0):
    """Disassemble bytecode. Returns list of (pc, text, details)."""
    lines = []
    i = 0
    n = len(code)
    while i < n:
        op = code[i]
        i += 1
        info = OPCODES.get(op)
        if info is None:
            lines.append((i - 1, f"  ??? 0x{op:02x}", {}))
            continue
        name, imm_type, imm_desc = info
        start = i - 1

        if imm_type == 0:
            lines.append((start, f"  {name}", {}))
        elif imm_type == 1:  # u32
            val = u32(code, i)
            i += 4
            if name in ("JUMP", "JUMP_IF_FALSE", "MATCH_JUMP"):
                target = code_base + val
                lines.append((start, f"  {name} ->{val:5d}  (pc={target})", {"target": target}))
            else:
                lines.append((start, f"  {name} {val}", {"val": val}))
        elif imm_type == 2:  # i8
            val = i8(code, i)
            i += 1
            lines.append((start, f"  {name} {val}", {"val": val}))
        elif imm_type == 3:  # i64
            val = i64(code, i)
            i += 8
            lines.append((start, f"  {name} {val}", {"val": val}))
        elif imm_type == 4:  # string (len + data)
            slen = u32(code, i)
            i += 4
            s = code[i:i+slen].decode('utf-8', errors='replace')
            i += slen
            lines.append((start, f"  {name} \"{s}\"", {"s": s}))
        elif imm_type == 5:  # CLOSURE: fn_id, nfree, nfree*slots
            fn_id = u32(code, i)
            nfree = u32(code, i + 4)
            i += 8
            slots = []
            for _ in range(nfree):
                slot = u32(code, i)
                slots.append(slot)
                i += 4
            slot_str = ", ".join(str(s) for s in slots) if slots else ""
            lines.append((start, f"  CLOSURE fn#{fn_id} nfree={nfree} [{slot_str}]", {"fn_id": fn_id, "nfree": nfree, "slots": slots}))
        elif imm_type == 6:  # CCALL: cfunc_idx (u32), nargs (u8)
            cfunc = u32(code, i)
            nargs = code[i + 4]
            i += 5
            lines.append((start, f"  CCALL cfunc#{cfunc} nargs={nargs}", {"cfunc": cfunc, "nargs": nargs}))
        else:
            lines.append((start, f"  {name} ???", {}))

    return lines


def load_tabc(path):
    """Load .tabc file, return (syms, fn_table, code, code_len, n_fns, top_fn_id)."""
    with open(path, 'rb') as f:
        data = f.read()

    if data[:4] != b'TABC':
        raise ValueError(f"Bad magic: {data[:4]!r}")

    off = 4
    version = u32(data, off); off += 4
    n_symbols = u32(data, off); off += 4
    total_fns = u32(data, off); off += 4
    top_fn_id = u32(data, off); off += 4
    code_len = u32(data, off); off += 4

    # Read symbols
    syms = []
    for i in range(n_symbols):
        slen = u32(data, off); off += 4
        sym = data[off:off+slen].decode('utf-8', errors='replace')
        off += slen
        syms.append(sym)

    # Read fn table
    fn_table = []
    for i in range(total_fns):
        fn_off = u32(data, off); off += 4
        fn_table.append(fn_off)

    # Read code
    code = data[off:off+code_len]

    return syms, fn_table, code, code_len, total_fns, top_fn_id


def find_fn_boundaries(code, fn_table):
    """Return list of (fn_id, start_pc, end_pc) for each function."""
    boundaries = []
    sorted_fns = sorted(enumerate(fn_table), key=lambda x: x[1])
    for idx, (fn_id, start) in enumerate(sorted_fns):
        if idx + 1 < len(sorted_fns):
            end = sorted_fns[idx + 1][1]
        else:
            end = len(code)
        boundaries.append((fn_id, start, end))
    return sorted(boundaries, key=lambda x: x[0])


def format_pc(pc, code_len):
    """Format a PC offset with nice alignment."""
    return f"[{pc:4d}]"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    path = sys.argv[1]
    do_compile = False

    if path == '-d':
        # Compile first
        if len(sys.argv) < 3:
            print("Usage: tools/disasm.py -d <file>.ta")
            sys.exit(1)
        ta_path = sys.argv[2]
        out_path = ta_path.replace('.ta', '.tabc') if ta_path.endswith('.ta') else ta_path + '.tabc'
        cmd = f"./tinyactor '{ta_path}' '{out_path}'"
        print(f"$ {cmd}")
        ret = os.system(cmd)
        if ret != 0:
            print("Compilation failed")
            sys.exit(1)
        print()
        path = out_path

    syms, fn_table, code, code_len, n_fns, top_fn_id = load_tabc(path)

    print(f"Module: {path}")
    print(f"  Symbols:   {len(syms)}")
    print(f"  Fns:       {n_fns}")
    print(f"  Top fn:    #{top_fn_id}")
    print(f"  Code size: {code_len} bytes")
    print()

    # Print symbol table
    print("── Symbol table ──")
    for i, sym in enumerate(syms):
        marker = " *" if i == top_fn_id else ""
        print(f"  sym[{i:3d}] = {sym}{marker}")
    print()

    # Print function table
    print("── Function table ──")
    for i, off in enumerate(fn_table):
        marker = " (top)" if i == top_fn_id else ""
        print(f"  fn[{i:3d}] @ offset {off:5d}{marker}")
    print()

    # Find function boundaries
    fn_boundaries = find_fn_boundaries(code, fn_table)

    # Disassemble per function
    print("── Bytecode ──")
    for fn_id, fn_start, fn_end in fn_boundaries:
        fn_code = code[fn_start:fn_end]
        marker = " (top)" if fn_id == top_fn_id else ""
        print(f"\n── fn[{fn_id}] at offset {fn_start}..{fn_end-1} ({fn_end-fn_start} bytes){marker} ──")
        lines = disasm(fn_code, syms, code_base=fn_start)
        for pc, text, _ in lines:
            print(f"  [{fn_start + pc:4d}] {text}")
    print()


if __name__ == '__main__':
    main()