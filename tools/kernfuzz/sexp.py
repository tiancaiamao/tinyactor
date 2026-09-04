# -*- coding: utf-8 -*-
"""sexp.py — shared s-expr reader/writer for the kernel-fuzzing toolchain.

Toolchain-wide version (transforms / reduce / runner all import this); the
reader semantics mirror tools/kernfuzz/golden/sexp.py (which is golden-only
and stays untouched). The writer is byte-compatible with the canonical
renderer in tools/kernfuzz/ast-dump.ta (replica of
test/compiler/parser-ast.ta's render()), so read -> write round-trips the
frozen snapshots in test/kernfuzz-frozen/snapshots/ byte-for-byte.

Python mapping (same as golden/sexp.py):
    int    -> Python int
    string -> Python str  (escapes decoded on read; one char == one byte,
                           because dumps travel through a latin-1 stdout and
                           files are read/written as latin-1 — RAW high bytes
                           (non-UTF-8) are byte-transparent)
    symbol -> Symbol(name)          (a str is NEVER a symbol)
    nil/true/false -> NIL/TRUE/FALSE sentinels
    proper list   -> Pair chain ending in NIL
    dotted pair   -> Pair chain whose final cdr is a non-NIL value, rendered
                     as `(a . b)`

Rendering (writer) rules, anchored to ast-dump.ta's escape_str():
    int    -> decimal
    symbol -> name; nil/true/false -> literals
    string -> quoted; each byte escaped as: \\n -> `\\n`, \\r -> `\\r`,
              \\t -> `\\t`, backslash -> `\\\\`, double-quote -> `\\"`,
              every other byte (including RAW high bytes and other control
              bytes) written literally
    list   -> `(a b c)`, dotted tail -> `(a . b)`
Files are read and written as latin-1 (byte-transparent, matches golden).
"""

import os

__all__ = [
    "Symbol", "Pair", "NIL", "TRUE", "FALSE",
    "sexp_read", "sexp_read_string", "sexp_write", "sexp_write_file",
    "sexp_collect_cars",
]


class Symbol(object):
    """An interned TA symbol. A str literal is NEVER a Symbol."""
    __slots__ = ("name",)

    def __init__(self, name):
        self.name = name

    def __repr__(self):
        return self.name

    def __eq__(self, other):
        return isinstance(other, Symbol) and self.name == other.name

    def __hash__(self):
        return hash(("Symbol", self.name))


NIL = Symbol("nil")
TRUE = Symbol("true")
FALSE = Symbol("false")


class Pair(object):
    """A TA pair (cons cell). A proper list is a chain ending in NIL; a
    dotted tail (any non-NIL value) is rendered as ` . `."""
    __slots__ = ("car", "cdr")

    def __init__(self, car, cdr):
        self.car = car
        self.cdr = cdr

    def __repr__(self):
        return "Pair(%r, %r)" % (self.car, self.cdr)


# ---------------------------------------------------------------------------
# Reader (mirrors golden/sexp.py: Guile-read semantics over latin-1 text)
# ---------------------------------------------------------------------------

def _decode_escape(body):
    out = []
    i = 0
    n = len(body)
    while i < n:
        c = body[i]
        if c == "\\" and i + 1 < n:
            nxt = body[i + 1]
            if nxt == "n":
                out.append("\n")
                i += 2
            elif nxt == "r":
                out.append("\r")
                i += 2
            elif nxt == "t":
                out.append("\t")
                i += 2
            elif nxt == "\\":
                out.append("\\")
                i += 2
            elif nxt == '"':
                out.append('"')
                i += 2
            elif nxt == "'":
                out.append("'")
                i += 2
            elif nxt == "x" and i + 3 < n:
                try:
                    out.append(chr(int(body[i + 2:i + 4], 16) & 0xFF))
                    i += 4
                except ValueError:
                    out.append("\\")
                    i += 1
            else:
                out.append("\\")
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _tokenize(text):
    toks = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif c in "()":
            toks.append(c)
            i += 1
        elif c == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                elif text[j] == '"':
                    break
                else:
                    j += 1
            if j >= n:
                raise ValueError("unterminated string literal")
            toks.append(text[i:j + 1])  # includes quotes; raw body
            i = j + 1
        else:
            j = i
            while j < n and not text[j].isspace() and text[j] not in "()":
                j += 1
            toks.append(text[i:j])
            i = j
    return toks


def _atom_to_value(atom):
    if atom == "nil":
        return NIL
    if atom == "true":
        return TRUE
    if atom == "false":
        return FALSE
    try:
        return int(atom, 10)
    except ValueError:
        pass
    return Symbol(atom)


class _Reader(object):
    def __init__(self, toks):
        self.toks = toks
        self.idx = 0

    def read_one(self):
        if self.idx >= len(self.toks):
            raise ValueError("unexpected end of input")
        t = self.toks[self.idx]
        if t == "(":
            self.idx += 1
            return self._read_list()
        if t == ")":
            raise ValueError("unbalanced ')'")
        if t.startswith('"'):
            self.idx += 1
            return _decode_escape(t[1:-1])
        self.idx += 1
        return _atom_to_value(t)

    def _read_list(self):
        items = []
        tail = None
        while True:
            if self.idx >= len(self.toks):
                raise ValueError("unbalanced '('")
            if self.toks[self.idx] == ")":
                self.idx += 1
                break
            if self.toks[self.idx] == ".":
                self.idx += 1
                tail = self.read_one()
                if self.idx >= len(self.toks) or self.toks[self.idx] != ")":
                    raise ValueError("dotted pair not terminated by ')'")
                self.idx += 1
                break
            items.append(self.read_one())
        return _build_list(items, tail)


def _build_list(items, tail):
    node = NIL if tail is None else tail
    for item in reversed(items):
        node = Pair(item, node)
    return node


def sexp_read_string(s):
    """Parse s-expr text (latin-1 str) -> the single top-level cons tree."""
    toks = _tokenize(s)
    r = _Reader(toks)
    if r.idx >= len(toks):
        raise ValueError("empty s-expr")
    node = r.read_one()
    if r.idx != len(toks):
        raise ValueError("trailing tokens after s-expr")
    return node


def sexp_read(path):
    """Read an s-expr file (latin-1, byte-transparent) -> cons tree."""
    with open(path, "rb") as f:
        return sexp_read_string(f.read().decode("latin-1"))


# ---------------------------------------------------------------------------
# Writer (byte-compatible with tools/kernfuzz/ast-dump.ta's render())
# ---------------------------------------------------------------------------

_ESCAPES = {"\n": "\\n", "\r": "\\r", "\t": "\\t", "\\": "\\\\", '"': '\\"'}


def _render_string(s):
    parts = ['"']
    for ch in s:
        parts.append(_ESCAPES.get(ch, ch))
    parts.append('"')
    return "".join(parts)


def _render(v):
    if v is NIL:
        return "nil"
    if v is TRUE:
        return "true"
    if v is FALSE:
        return "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, str):
        return _render_string(v)
    if isinstance(v, Symbol):
        return v.name
    if isinstance(v, Pair):
        items = [_render(v.car)]
        rest = v.cdr
        while isinstance(rest, Pair):
            items.append(_render(rest.car))
            rest = rest.cdr
        if rest is not NIL:
            items.append(".")
            items.append(_render(rest))
        return "(" + " ".join(items) + ")"
    raise TypeError("cannot render %r" % (v,))


def sexp_write(tree):
    """Render a cons tree -> canonical s-expr text (ast-dump compatible)."""
    return _render(tree)


def sexp_write_file(tree, path):
    """Render and write to path, latin-1 encoded (byte-transparent)."""
    with open(path, "wb") as f:
        f.write(_render(tree).encode("latin-1"))


# ---------------------------------------------------------------------------
# Traversal helper
# ---------------------------------------------------------------------------

def sexp_collect_cars(tree):
    """Walk the whole tree; for every list node (Pair) whose car is a Symbol,
    collect that symbol. Traverses both car and cdr sides. Returns a Python
    list of Symbols in encounter order (duplicates preserved)."""
    out = []
    seen = set()

    def walk(v):
        if isinstance(v, Pair):
            if id(v) in seen:
                return
            seen.add(id(v))
            if isinstance(v.car, Symbol):
                out.append(v.car)
            walk(v.car)
            walk(v.cdr)

    walk(tree)
    return out