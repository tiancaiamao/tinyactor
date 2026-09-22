import sys
BASE = """PUSH_NIL PUSH_TRUE PUSH_FALSE PUSH_INT8 PUSH_INT PUSH_SYM PUSH_STRING LOAD STORE CONS CAR CDR ADD SUB MUL DIV MOD EQ LT LE IS_NIL IS_PAIR IS_INT IS_STRING IS_BYTES IS_PID JUMP JUMP_IF_FALSE POP DUP CLOSURE CALL TAIL_CALL RET SPAWN SPAWN_MAIN SPAWN_CLOS SEND RECV RECV_PEEK RECV_COMMIT SELF MONITOR HALT""".split()
NAMES = BASE + ["ENTER","CCALL_NAME","NE","PUSH_FLOAT","RECV_AFTER"]
VAR = {6, 30}
ARG = {3:1, 4:8, 5:4, 7:4, 8:4, 26:4, 27:4, 30:8, 31:4, 32:4, 34:4, 35:4, 44:4, 45:5, 47:"str"}

def parse(path):
    data = open(path, "rb").read()
    u32 = lambda o: int.from_bytes(data[o:o+4], "little")
    version, n_sym, n_fn, top_fn, code_len = u32(4), u32(8), u32(12), u32(16), u32(20)
    pos = 24; syms = []
    for _ in range(n_sym):
        l = u32(pos); syms.append(data[pos+4:pos+4+l].decode()); pos += 4 + l
    fns = [u32(pos + 4*i) for i in range(n_fn)]
    pos += 4*n_fn
    if version >= 2:
        for _ in range(n_fn):
            l = u32(pos); pos += 4 + l   # skip fn names
    code = data[pos:pos+code_len]
    return syms, fns, code

def disasm(syms, code, start, end):
    pc = start; out = []
    while pc < end:
        op = code[pc]; name = NAMES[op] if op < len(NAMES) else f"OP{op}"
        if op in VAR:
            slen = int.from_bytes(code[pc+1:pc+5], "little")
            txt = code[pc+5:pc+5+slen][:20]
            out.append(f"  {pc:4d} {name} len={slen} {txt!r}")
            pc += 5 + slen
        else:
            n = ARG.get(op, 0)
            operand = int.from_bytes(code[pc+1:pc+1+n], "little", signed=(n==4))
            extra = ""
            if op == 5 and 0 <= operand < len(syms): extra = "  ;" + syms[operand]
            out.append(f"  {pc:4d} {name} {operand}{extra}")
            pc += 1 + n
    return out

syms, fns, code = parse(sys.argv[1])
print(f"== {sys.argv[1]}: {len(syms)} syms, {len(fns)} fns, code {len(code)}; fn offsets {fns}")
lo, hi = int(sys.argv[2]), int(sys.argv[3]) if len(sys.argv) > 3 else len(code)
print("\n".join(disasm(syms, code, lo, min(hi, len(code)))))
