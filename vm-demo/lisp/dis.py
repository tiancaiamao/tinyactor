#!/usr/bin/env python3
"""Minimal disassembler for lispvm .bc files.

Opcode order and word lengths mirror lispvm.c's enum + g_optlen[].
"""
import sys

OPS = ["CONST", "LOAD", "STORE", "LOADF", "PUSH", "ADD", "SUB", "MUL", "DIV",
       "MOD", "LT", "LE", "GT", "GE", "EQ", "PAIRP", "SYMP", "CONS", "CAR",
       "CDR", "JIF", "JUMP", "CALL", "TCALL", "RET", "MAKECLOS", "GLOBAL",
       "RESERVE"]

# g_optlen[]: TOTAL words per instruction (opcode word included).
OPTW = {"CONST": 2, "LOAD": 2, "STORE": 2, "LOADF": 2, "PUSH": 1,
        "ADD": 1, "SUB": 1, "MUL": 1, "DIV": 1, "MOD": 1,
        "LT": 1, "LE": 1, "GT": 1, "GE": 1, "EQ": 1,
        "PAIRP": 1, "SYMP": 1, "CONS": 1, "CAR": 1, "CDR": 1,
        "JIF": 3, "JUMP": 2, "CALL": 2, "TCALL": 2, "RET": 1,
        "MAKECLOS": 3, "GLOBAL": 2, "RESERVE": 2}


def disasm(code):
    out = []
    j = 0
    while j < len(code):
        op = code[j]
        nm = OPS[op] if op < len(OPS) else "?%d" % op
        nw = OPTW.get(nm, 1)
        ops = code[j + 1:j + nw]
        if nm == "JIF":
            out.append("  %4d: JIF      t=%s f=%s" % (j, ops[0], ops[1]))
        elif nw > 1:
            out.append("  %4d: %-8s %s" % (j, nm, " ".join(str(o) for o in ops)))
        else:
            out.append("  %4d: %s" % (j, nm))
        j += nw
    return out


def consts(words, start):
    k = start
    ci = 0
    out = []
    while k < len(words):
        kind = words[k]
        if kind == 1:
            out.append("  [%d] nil" % ci)
            k += 1
        elif kind == 0:
            out.append("  [%d] int %d" % (ci, words[k + 1]))
            k += 2
        elif kind == 4:
            n = words[k + 1]
            s = "".join(chr(c) for c in words[k + 2:k + 2 + n])
            out.append("  [%d] sym %r" % (ci, s))
            k += 2 + n
        else:
            out.append("  [%d] ?kind=%d" % (ci, kind))
            k += 1
        ci += 1
    return out


def main(path):
    words = [int(x) for x in open(path).read().split()]
    nf, nc = words[0], words[1]
    print("fns=%d consts=%d" % (nf, nc))
    p = 2
    for i in range(nf):
        nameidx, entry, nargs, maxd, clen = words[p:p + 5]
        p += 5
        code = words[p:p + clen]
        p += clen
        print("")
        print("fn#%d nameidx=%d entry=%d nargs=%d maxd=%d codelen=%d"
              % (i, nameidx, entry, nargs, maxd, clen))
        for line in disasm(code):
            print(line)
    print("")
    print("consts:")
    for line in consts(words, p):
        print(line)


main(sys.argv[1])