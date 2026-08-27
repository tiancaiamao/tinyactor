# -*- coding: utf-8 -*-
"""sexp.py — s-expr reader for the TinyActor kernel-fuzzing golden toolchain.

Parses frozen AST dump text (test/kernfuzz-frozen/snapshots/*.sexp) into a
Python nested structure. The mapping mirrors the s-expr encoding table in
docs/kernel-fuzzing-design.md §5.3 and test/kernfuzz-frozen/ast-nodes.txt:

    int    -> Python int
    string -> Python str            (escapes decoded: \\ \\n \\t, \\xNN)
    nil    -> Symbol("nil")         (distinct from any user symbol)
    true   -> Symbol("true")
    false  -> Symbol("false")
    symbol -> Symbol(name)          (interned; a str is NEVER a symbol)
    list   -> list                  (proper list = Python list)
    dotted -> Pair                  (Pair.car / Pair.cdr; proper list is a
                                     chain ending in the NIL sentinel)

A Symbol class keeps AST symbols (incl. nil/true/false) distinct from string
literals — the TA language distinguishes symbol values from string values
everywhere (print, ==, match patterns).

Reader semantics follow Guile `read`:
  * strings may contain '(' / ')' without breaking structure
  * escapes are decoded on read (the VM print path re-encodes on dump).
    Decoded escapes: \\ \" \\x \\n \\r \\t and \\xNN (two hex digits).
    Unknown escapes keep the backslash literally (verified vs string-escapes).
"""


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


# sentinel values used by the reader ("nil" is a unique value, never a symbol)
NIL = Symbol("nil")
TRUE = Symbol("true")
FALSE = Symbol("false")


class Pair(object):
    """A TA pair (cons cell). A proper list is a chain ending in NIL; the
    dotted tail (any non-NIL value) is rendered by print as ` . `."""
    __slots__ = ("car", "cdr")

    def __init__(self, car, cdr):
        self.car = car
        self.cdr = cdr

    def __repr__(self):
        return "Pair(%r, %r)" % (self.car, self.cdr)


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
                    out.append(chr(int(body[i + 2:i + 4], 16)))
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


def tokenize(text):
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
    """Recursive-descent s-expr parser over a token list."""

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
    """Build a cons chain. items are the leading car values; tail is the final
    cdr (NIL for a proper list, any value for a dotted tail)."""
    node = NIL if tail is None else tail
    for item in reversed(items):
        node = Pair(item, node)
    return node


def parse(text):
    """Parse s-expr text -> the single top-level form (a cons chain).

    The frozen dump is exactly one s-expr (the whole program = a list of
    top-level forms). Raises ValueError on unbalanced/malformed input.
    """
    toks = tokenize(text)
    r = _Reader(toks)
    if r.idx >= len(toks):
        raise ValueError("empty s-expr")
    node = r.read_one()
    if r.idx != len(toks):
        raise ValueError("trailing tokens after s-expr")
    return node