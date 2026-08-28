# -*- coding: utf-8 -*-
"""gen.py — typed random TA program generator (DELIV-3 core).

Consumes a seed, produces ONE deterministic TinyActor source program on
stdout (or --count files into --out-dir).  Written for the kernel-fuzzing
toolchain; the design contract is docs/kernel-fuzzing-design.md:

  * §5.0  subset definition v0 (the ONLY constructs generated live here),
  * §5.1.1 program skeleton (helper fns + main with print statements),
  * DEC-3 full-parens rendering: the generator renders TA SOURCE TEXT from
    its own tree with every binary operation parenthesized — no internal
    AST dump / re-render, so operator precedence bugs cannot leak in,
  * §5.2 generator provisions: function-value positions (lambda literals +
    bare top-level fn names as expressions) for the Tier B transforms.

Generation rules (all probed against the working-tree parser/typecheck/VM
on 2026-08-29, see .pge/progress.md; each rule cites its probe):

  R1  negative ints render as `(0 - N)`; a bare `-N` literal is NEVER
      emitted (TA has no unary minus in expression position).
  R2  int literal domain: |v| <= 2^47 - 1 (bigger literals silently wrap
      inside the parser, misplacing the intended value).
  R3  type annotations: lowercase base types only (`int`, `string`,
      `bool`); ADT names allowed on fn params; a binding whose type is a
      FUNCTION type gets NO annotation (the spec has no arrow types).
  R4  ADT constructor params are declared WITHOUT field annotations
      (`type Sh { Pt; Ci(x, r) }`); `Ci(x: int, r: int)` is rejected by
      typecheck (probe /tmp/probe68).
  R5  match always carries a trailing `_ ->` wildcard arm (no
      non-exhaustive warning noise; T8 relies on the marker).
  R6  bool literal patterns (`true`/`false` as patterns) are NEVER
      generated: VM and golden DIVERGE on them (VM matches the bool
      literal arm for ANY scrutinee — real divergence, recorded in
      .pge/progress.md; gen avoids the class entirely).
  R7  `[h, ..t]` rest patterns do not exist in the parser (kernfuzz-facts
      f2); list decomposition uses `cons(h, t)` patterns only.
  R8  lambdas carry no annotations at all; a named fn whose return type
      is a function type carries no return annotation (typecheck rejects
      the arrow annotation; probe /tmp/probe16).
  R9  `&&`/`||` operands are bool-typed (typecheck rejects int operands).
  R10 every generated program terminates: helpers recurse on a structurally
      decreasing list/ctor param only; no self-referential let values.
  R11 every print statement sits on its own line (multi-line layout rule;
      the reducer's whole-line deletion strategy depends on it).

TODO (v1 iterations, not blockers for this delivery):
  * `%` and `/` are excluded from random binops (v0: the test executability
    gate requires exit 0, and a divzero death exits 1).  Add them together
    with the §5.1.3 DIVZERO death-protocol comparison path in test_gen.
  * ADT constructor sub-patterns of arity >= 2 are excluded from match
    (probe61: guard/binder evaluation semantics differed from golden).
  * Match-arm bodies are single expressions only (no begin blocks).
  * One constructive list-recursion helper template exists; general
    recursive helpers with structural-decrease checking (R10) are TODO.
  * T8 disjointness flags on match arms are recorded (Match.arms) but not
    yet consumed by any transform.

Determinism: every random draw goes through tools/kernfuzz/prng.py
(counter-based sha256 stream, M-2).  Same seed -> byte-identical program.

CLI:
    python3 tools/kernfuzz/gen.py --seed N              # stdout
    python3 tools/kernfuzz/gen.py --seed N --max-depth D
    python3 tools/kernfuzz/gen.py --count K --out-dir D # p_<seed>.ta files
"""

import argparse
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import prng                       # noqa: E402


# ---------------------------------------------------------------------------
# constants (§5.0)
# ---------------------------------------------------------------------------

_TWO47 = 1 << 47
_I48_MAX = _TWO47 - 1          # largest representable int48 magnitude
_I48_MIN = -_TWO47             # smallest int48 (-2^47)

# int48 boundary literals (all within the |v| <= 2^47-1 hard invariant).
BOUNDARY_INTS = (0, 1, _I48_MAX, _I48_MAX - 1, 2, 3, 7, 10)

# small symbol-name pool for symbol literals / ctor-less symbol values
SYMBOL_NAMES = ("red", "green", "blue", "yes", "no", "up", "down")

# long-string pool: no backslash escapes are ever emitted (§5.0: "gen 不
# 产出含反斜杠的字面量"); printable ASCII only.  Stored UNQUOTED; _str()
# adds the double quotes at render time.
_STRING_ATOMS = (
    "a", "b", "abc", "hello world", "TinyActor", "kernfuzz", "seed corpus",
    "0123456789", "!@# $%^ &*()", "the quick brown fox jumps over",
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "x", "zz", "lead-in ",
)


def _str(rng):
    """A quoted string literal: '"%s"' % atom (TA string tokens)."""
    return '"%s"' % _STRING_ATOMS[prng.prng_next_range(rng,
                                                       len(_STRING_ATOMS))]


def w48(n):
    """int48 normalization (two's-complement 48-bit), mirroring golden.w48."""
    m = n % (1 << 48)
    return m - (1 << 48) if m >= _TWO47 else m


# ---------------------------------------------------------------------------
# rendering tree
# ---------------------------------------------------------------------------

class Atom(object):
    """An opaque source-text leaf (already fully parenthesized if needed)."""
    __slots__ = ("text", "postulate")

    def __init__(self, text, postulate=None):
        self.text = text
        self.postulate = postulate


def M(text, postulate=None):
    """Shorthand Atom constructor."""
    return Atom(text, postulate)


def render(e, indent=0):
    """Render an expression tree node to TA source text.

    Match nodes expand to multiple lines; everything else stays on one
    line (already fully parenthesized).
    """
    pad = "  " * indent
    if isinstance(e, Match):
        return e.render(indent)
    if isinstance(e, Atom):
        return pad + e.text
    raise TypeError("cannot render %r" % (e,))


def _render_maybe_multi(e, indent):
    """Render sub-expression: multi-line nodes keep their lines, atoms are
    inlined without extra indentation."""
    if isinstance(e, Match):
        return e.render(indent)
    return e.text


class Match(object):
    """A generated match expression.

    arms: list of (pattern_src, guard_src_or_None, body_expr)
    R5 invariant: last arm is always the `_` wildcard (set by the
    generators that build Match values; _final_wildcard enforces it).
    """

    __slots__ = ("scrutinee", "arms")

    def __init__(self, scrutinee, arms):
        self.scrutinee = scrutinee          # Atom (single-line expr)
        self.arms = arms

    def render(self, indent=0):
        pad = "  " * indent
        out = ["match %s {" % self.scrutinee.text]
        for pat, guard, body in self.arms:
            head = pat
            if guard is not None:
                head += " when (%s)" % guard
            body_text = _render_maybe_multi(body, indent + 2)
            out.append("%s  %s -> %s" % (pad, head, body_text))
        out.append("%s}" % pad)
        return "\n".join(out)


def _final_wildcard(arms):
    """R5: force a trailing `_ ->` arm."""
    if arms and arms[-1][0] == "_":
        return arms
    # wildcard body: reuse the last arm's body value (any expr works)
    fallback = arms[-1][2] if arms else M("0")
    return arms + [("_", None, fallback)]


# ---------------------------------------------------------------------------
# fresh name allocation
# ---------------------------------------------------------------------------

class Namer(object):
    """Deterministic fresh-name allocator (var_1, fn_1, ...)."""

    def __init__(self):
        self._used = set(["main", "print"])     # never collide with these

    def fresh(self, base, rng):
        i = 1
        while "%s_%d" % (base, i) in self._used:
            i += 1
        name = "%s_%d" % (base, i)
        self._used.add(name)
        return name

    def reserve(self, name):
        self._used.add(name)


# ---------------------------------------------------------------------------
# typed expression generation
# ---------------------------------------------------------------------------

def _pick_int_literal(rng):
    """Boundary-weighted int literal (R1/R2)."""
    if prng.prng_next_range(rng, 10) < 6:
        return BOUNDARY_INTS[prng.prng_next_range(rng, len(BOUNDARY_INTS))]
    return prng.prng_next_range(rng, 1000)


def int_lit(rng):
    v = _pick_int_literal(rng)
    if v == 0:
        return M("0", "int-lit")
    if prng.prng_next_range(rng, 10) < 4:
        # negative via the R1 form (nested to stay fully parenthesized)
        return M("(%d - %d)" % (-v if False else 0, v), "int-lit-neg")
    return M(str(v), "int-lit")


def _paren(op, a, b, postulate):
    return M("(%s %s %s)" % (a.text, op, b.text), postulate)


def gen_int_expr(ctx, depth):
    """Generate an int-typed expression (single-line, fully parenthesized).

    Returns (Atom, used_fn_bool).  used_fn flags whether a top-level
    helper was called (postulate bookkeeping for the driver).
    """
    rng = ctx.rng
    choices = ["lit", "lit"]
    if depth > 0:
        choices += ["binop", "binop", "binop"]
        if ctx.fn_pool:
            choices.append("call")
    if ctx.int_vars:
        choices.append("var")
    kind = choices[prng.prng_next_range(rng, len(choices))]

    if kind == "lit":
        return int_lit(rng), False
    if kind == "var" and ctx.int_vars:
        v = ctx.int_vars[prng.prng_next_range(rng, len(ctx.int_vars))]
        return M(v, "int-var"), False
    if kind == "call" and ctx.fn_pool:
        f = ctx.fn_pool[prng.prng_next_range(rng, len(ctx.fn_pool))]
        args = []
        for _ in range(f.arity):
            a, _u = gen_int_expr(ctx, depth - 1)
            args.append(a.text)
        return M("%s(%s)" % (f.name, ", ".join(args)), "call"), True
    # binop
    op = ("+", "-", "*", "+", "-")[prng.prng_next_range(rng, 5)]
    a, u1 = gen_int_expr(ctx, depth - 1)
    b, u2 = gen_int_expr(ctx, depth - 1)
    return _paren(op, a, b, "int-bin"), (u1 or u2)


def gen_bool_expr(ctx, depth):
    """bool-typed expression: comparisons (R9-safe), && / || of bools."""
    rng = ctx.rng
    kind = prng.prng_next_range(rng, 10)
    if kind < 6 or depth <= 0:
        # comparison of same-typed operands; < <= > >= are int-only in
        # typecheck (probe /tmp/probe59: string < string rejected)
        if ctx.int_vars and prng.prng_next_range(rng, 10) < 8:
            va = ctx.int_vars[prng.prng_next_range(rng, len(ctx.int_vars))]
            vb = ctx.int_vars[prng.prng_next_range(rng, len(ctx.int_vars))]
            op = ("<", "<=", ">", ">=", "==", "!=")[
                prng.prng_next_range(rng, 6)]
            return M("(%s %s %s)" % (va, op, vb), "bool-cmp")
        a, _ = gen_int_expr(ctx, 0)
        b, _ = gen_int_expr(ctx, 0)
        op = ("==", "!=")[prng.prng_next_range(rng, 2)]
        return M("(%s %s %s)" % (a.text, op, b.text), "bool-cmp")
    op = ("&&", "||")[prng.prng_next_range(rng, 2)]
    a = gen_bool_expr(ctx, depth - 1)
    b = gen_bool_expr(ctx, depth - 1)
    return _paren(op, a, b, "bool-logic")


def gen_list_expr(ctx, depth, mixed):
    """list-typed expression: literal, nested cons, or empty list."""
    rng = ctx.rng
    kind = prng.prng_next_range(rng, 10)
    if kind == 0:
        return M("[]", "list-empty")
    n = 1 + prng.prng_next_range(rng, 3)
    items = []
    for _ in range(n):
        if mixed:
            items.append(gen_mixed_item(ctx, depth - 1))
        else:
            items.append(gen_int_expr(ctx, depth - 1)[0].text)
    return M("[%s]" % ", ".join(items), "list-lit")


def gen_mixed_item(ctx, depth):
    """One element of a mixed list: int / bool / string / nil / nested list."""
    rng = ctx.rng
    k = prng.prng_next_range(rng, 6)
    if k == 0:
        return "true" if prng.prng_next_range(rng, 2) == 0 else "false"
    if k == 1:
        return "nil"
    if k == 2:
        return _str(rng)
    if k == 3:
        return gen_list_expr(ctx, depth, True).text
    a, _ = gen_int_expr(ctx, depth)
    return a.text


def gen_pair_expr(ctx, depth):
    """Dotted pair value via cons (the (a . b) print form is a must-test)."""
    a, _ = gen_int_expr(ctx, depth - 1)
    if prng.prng_next_range(ctx.rng, 2) == 0:
        b, _ = gen_int_expr(ctx, depth - 1)
        return M("cons(%s, %s)" % (a.text, b.text), "pair-lit")
    lst = gen_list_expr(ctx, depth - 1, False)
    return M("cons(%s, %s)" % (a.text, lst.text), "pair-lit")


def gen_string_expr(ctx):
    return M(_str(ctx.rng), "string-lit")


def gen_symbol_expr(ctx):
    return M("'%s" % SYMBOL_NAMES[
        prng.prng_next_range(ctx.rng, len(SYMBOL_NAMES))], "symbol-lit")


def gen_value_for_type(ctx, ty, depth):
    """Value of the declared type ty (int/string/bool/any-list/pair)."""
    if ty == "int":
        return gen_int_expr(ctx, depth)[0]
    if ty == "string":
        return gen_string_expr(ctx)
    if ty == "bool":
        return M("true" if prng.prng_next_range(ctx.rng, 2) == 0 else "false",
                 "bool-lit")
    if ty == "list":
        return gen_list_expr(ctx, depth, True)
    if ty == "pair":
        return gen_pair_expr(ctx, depth)
    raise ValueError("no generator for type %r" % ty)


# ---------------------------------------------------------------------------
# context
# ---------------------------------------------------------------------------

class FnSig(object):
    __slots__ = ("name", "arity", "kind", "postulates")

    def __init__(self, name, arity, kind, postulates):
        self.name = name
        self.arity = arity          # number of int params (fn-value params excluded)
        self.kind = kind            # 'int' | 'list-len' | 'list-sum' | 'apply1' | 'match'
        self.postulates = postulates


class Ctx(object):
    """Everything the expression generators may draw from."""

    def __init__(self, rng, namer):
        self.rng = rng
        self.namer = namer
        self.int_vars = []          # in-scope int-typed variable names
        self.string_vars = []
        self.bool_vars = []
        self.list_vars = []
        self.any_vars = []          # (name, kind) kinds: list/mixed/pair/symbol
        self.adt_vars = []          # (var_name, type_name)
        self.fn_vars = []           # function-value variable names
        self.fn_pool = []           # FnSig of int -> int helpers
        self.list_fns = []          # FnSig of list -> int helpers
        self.adt_fns = []           # (FnSig-like dict) match over adt
        self.apply_fns = []         # FnSig apply1(f, n)
        self.adt_types = []         # (type_name, [variant names])
        self.adt_decls = []         # (type_name, variant_name, [fields])


# ---------------------------------------------------------------------------
# match plan generation
# ---------------------------------------------------------------------------

def gen_match_on(ctx, scrut_var, scrut_ty, depth):
    """MatchPlan over an in-scope variable.

    scrut_ty: 'int' | 'string' | adt type name.
    R6: bool literal patterns are never generated.  R5: wildcard last.
    Returns Match (multi-line renderable).
    """
    rng = ctx.rng
    arms = []
    binders = []

    def fresh_binder(base):
        n = ctx.namer.fresh(base, rng)
        binders.append(n)
        return n

    if scrut_ty == "int":
        # int literal arms + guard arm + binder arm + wildcard
        for _ in range(1 + prng.prng_next_range(rng, 2)):
            v = _pick_int_literal(rng)
            body, _u = gen_int_expr(ctx, depth)
            arms.append((str(v), None, body))
        b = fresh_binder("m")
        op = ("<", ">")[prng.prng_next_range(rng, 2)]
        lit = int_lit(rng)
        guard = "%s %s %s" % (b, op, lit.text)
        body, _u = gen_int_expr(ctx, depth)
        arms.append((b, guard, body))
        body, _u = gen_int_expr(ctx, depth)
        arms.append((b, None, body))
    elif scrut_ty == "string":
        for _ in range(1 + prng.prng_next_range(rng, 2)):
            body, _u = gen_int_expr(ctx, depth)
            arms.append((_str(rng), None, body))
        b = fresh_binder("m")
        body, _u = gen_int_expr(ctx, depth)
        arms.append((b, None, body))
    else:
        # ADT match: DECLARED ctor names as patterns (R6-adjacent rule: a
        # ctor sub-pattern of arity >= 2 is never generated — probe
        # /tmp/probe61, guard evaluation consumes binders unevenly).
        _type_name, variants = scrut_ty
        for _ in range(1 + prng.prng_next_range(rng, 2)):
            vname = variants[prng.prng_next_range(rng, len(variants))]
            body, _u = gen_int_expr(ctx, depth)
            arms.append((vname, None, body))
        body, _u = gen_int_expr(ctx, depth)
        arms.append(("_", None, body))
    return Match(M(scrut_var), _final_wildcard(arms))


# ---------------------------------------------------------------------------
# helper fn generation
# ---------------------------------------------------------------------------

def gen_helper(ctx, max_depth):
    """One top-level helper fn; registers its postulates in ctx."""
    rng = ctx.rng
    kind = ("int", "int", "list-len", "list-sum", "apply1", "match")[
        prng.prng_next_range(rng, 6)]
    name = ctx.namer.fresh("helper", rng)

    if kind == "int":
        p1 = ctx.namer.fresh("a", rng)
        p2 = ctx.namer.fresh("b", rng)
        two = prng.prng_next_range(rng, 2) == 0
        params = [p1, p2] if two else [p1]
        ctx.int_vars.extend(params)
        body, _u = gen_int_expr(ctx, max_depth - 1)
        for p in params:
            ctx.int_vars.remove(p)
        sig = FnSig(name, len(params), "int", ["call"])
        src = "fn %s(%s) -> int {\n%s\n}" % (
            name, ", ".join("%s: int" % p for p in params),
            render(body, 1))
        return sig, src

    if kind in ("list-len", "list-sum"):
        l = ctx.namer.fresh("l", rng)
        h = ctx.namer.fresh("h", rng)
        t = ctx.namer.fresh("t", rng)
        # R10: structurally decreasing recursion on t
        base = "0"
        rec_op = "1 +" if kind == "list-len" else "%s +" % h
        body = ("match %s {\n"
                "  nil -> %s,\n"
                "  cons(%s, %s) -> (%s %s(%s))\n"
                "}" % (l, base, h, t, rec_op, name, t))
        sig = FnSig(name, 1, kind, ["list-fn"])
        ctx.list_fns.append(sig)
        src = "fn %s(%s) -> int {\n%s\n}" % (name, l, body)
        return sig, src

    if kind == "apply1":
        # function-value param WITHOUT annotation (R3/R8); int param typed
        f = ctx.namer.fresh("f", rng)
        x = ctx.namer.fresh("x", rng)
        sig = FnSig(name, 1, "apply1", ["apply"])
        ctx.apply_fns.append(sig)
        src = ("fn %s(%s, %s: int) -> int {\n  %s(%s)\n}"
               % (name, f, x, f, x))
        return sig, src

    # kind == "match": classify an int param through a MatchPlan
    v = ctx.namer.fresh("v", rng)
    ctx.int_vars.append(v)
    mp = gen_match_on(ctx, v, "int", max_depth - 1)
    ctx.int_vars.remove(v)
    sig = FnSig(name, 1, "match", ["call"])
    ctx.fn_pool.append(sig)
    src = "fn %s(%s: int) -> int {\n%s\n}" % (name, v, mp.render(1))
    return sig, src


def gen_adt_type(ctx):
    """One ADT declaration; returns source text.  R4: untyped ctor fields."""
    rng = ctx.rng
    tname = ctx.namer.fresh("Ty", rng).capitalize()
    nvars = 2 + prng.prng_next_range(rng, 2)
    variants = []
    for _ in range(nvars):
        vname = ctx.namer.fresh("Mk", rng).capitalize()
        # at most ONE field per variant: ctor sub-patterns of arity >= 2 are
        # excluded from match (probe /tmp/probe61); see TODO at file head.
        arity = prng.prng_next_range(rng, 2)     # 0..1 field
        fields = [ctx.namer.fresh("f", rng) for _ in range(arity)]
        variants.append((vname, arity))
        ctx.adt_decls.append((tname, vname, fields))
    ctx.adt_types.append((tname, [v for v, a in variants]))
    parts = []
    for _t, vname, fields in ctx.adt_decls:
        if _t != tname:
            continue
        if not fields:
            parts.append(vname)
        else:
            parts.append("%s(%s)" % (vname, ", ".join(fields)))
    return "type %s { %s }" % (tname, "; ".join(parts))


def gen_ctor_call(ctx, tname, depth):
    """Constructor call of a DECLARED variant, arity-correct (probe
    /tmp/probe_adt5/6: an undeclared ctor name or a wrong arg count is a
    silent runtime pair (not rejected) or a typecheck error)."""
    rng = ctx.rng
    _t, variants = [t for t in ctx.adt_types if t[0] == tname][0]
    vname = variants[prng.prng_next_range(rng, len(variants))]
    nfields = 1 if "(" in vname else 0    # not used; declared arity below
    args = []
    # declared arity: recomputed from the declaration's source rendering
    decl = [d for d in ctx.adt_decls if d[0] == tname and d[1] == vname]
    arity = len(decl[0][2]) if decl else nfields
    for _ in range(arity):
        a, _u = gen_int_expr(ctx, depth - 1)
        args.append(a.text)
    if not args:
        return M(vname, "ctor-call")
    return M("%s(%s)" % (vname, ", ".join(args)), "ctor-call")


# ---------------------------------------------------------------------------
# lambda / function-value generation
# ---------------------------------------------------------------------------

def gen_lambda(ctx, depth):
    """Anonymous closure literal (unannotated, R8).  int -> int shape."""
    rng = ctx.rng
    p = ctx.namer.fresh("k", rng)
    ctx.int_vars.append(p)
    body, _u = gen_int_expr(ctx, depth - 1)
    ctx.int_vars.remove(p)
    return M("fn(%s) { %s }" % (p, body.text), "lambda-lit")


def gen_fn_value_expr(ctx, depth):
    """Function-value position expression: lambda literal or bare top-level
    fn name (§5.2 generator provision for Tier B)."""
    rng = ctx.rng
    unary = [f for f in ctx.fn_pool if f.arity == 1]
    if unary and prng.prng_next_range(rng, 3) == 0:
        f = unary[prng.prng_next_range(rng, len(unary))]
        return M(f.name, "fn-name")
    return gen_lambda(ctx, depth)


# ---------------------------------------------------------------------------
# program assembly
# ---------------------------------------------------------------------------

class ProgramPlan(object):
    def __init__(self, seed, max_depth):
        self.seed = seed
        self.max_depth = max_depth
        self.rng = prng.make_prng(seed)
        self.namer = Namer()
        self.ctx = Ctx(self.rng, self.namer)
        self.fns = []               # (sig, src)
        self.type_decls = []
        self.main_stmts = []        # rendered lines

    # -- top-level lets -----------------------------------------------------

    def _top_let(self, name, value_atom, comment=None):
        line = "  let %s = %s;" % (name, value_atom.text)
        if comment:
            line += "  // %s" % comment
        self.main_stmts.append(line)

    def gen_top_lets(self):
        rng = self.ctx.rng
        n = 2 + prng.prng_next_range(rng, 6)
        shadow_name = None
        for _ in range(n):
            k = prng.prng_next_range(rng, 8)
            if k == 0:
                # int + shadow chain probe (R10-safe: pure arithmetic)
                base = self.ctx.namer.fresh("n", rng)
                v, _u = gen_int_expr(self.ctx, 1)
                self._top_let(base, v)
                self.ctx.int_vars.append(base)
                shadow_name = base
            elif k == 1 and shadow_name is not None:
                # same-name sequential rebinding (the only shadowing form)
                prev = shadow_name
                lit = int_lit(self.rng)
                op = ("+", "*")[prng.prng_next_range(self.rng, 2)]
                self._top_let(
                    prev, _paren(op, M(prev), lit, "shadow"),
                    "shadow chain")
            elif k == 2:
                nm = self.ctx.namer.fresh("s", rng)
                v = gen_string_expr(self.ctx)
                self._top_let(nm, v)
                self.ctx.string_vars.append(nm)
            elif k == 3:
                nm = self.ctx.namer.fresh("b", rng)
                v = gen_bool_expr(self.ctx, 1)
                self._top_let(nm, v)
                self.ctx.bool_vars.append(nm)
            elif k == 4:
                nm = self.ctx.namer.fresh("l", rng)
                v = gen_list_expr(self.ctx, 1,
                                  prng.prng_next_range(rng, 2) == 0)
                self._top_let(nm, v)
                self.ctx.list_vars.append(nm)
                self.ctx.any_vars.append((nm, "list"))
            elif k == 5:
                nm = self.ctx.namer.fresh("p", rng)
                v = gen_pair_expr(self.ctx, 1)
                self._top_let(nm, v)
                self.ctx.any_vars.append((nm, "pair"))
            elif k == 6:
                # function-value let binding (Tier B provision)
                nm = self.ctx.namer.fresh("fv", rng)
                v = gen_fn_value_expr(self.ctx, self.max_depth)
                self._top_let(nm, v)
                self.ctx.fn_vars.append(nm)
            else:
                nm = self.ctx.namer.fresh("y", rng)
                v = gen_symbol_expr(self.ctx)
                self._top_let(nm, v)
                self.ctx.any_vars.append((nm, "symbol"))

    # -- adt setup ----------------------------------------------------------

    def maybe_gen_adt(self):
        rng = self.ctx.rng
        if prng.prng_next_range(rng, 2) == 0:
            return
        src = gen_adt_type(self.ctx)
        self.type_decls.append(src)
        tname, variants = self.ctx.adt_types[-1]
        nm = self.ctx.namer.fresh("ad", rng)
        v = gen_ctor_call(self.ctx, tname, self.max_depth - 1)
        self._top_let(nm, v)
        self.ctx.adt_vars.append((nm, (tname, variants)))

    # -- print statements ---------------------------------------------------

    def gen_print(self):
        rng = self.ctx.rng
        ctx = self.ctx
        opts = []
        for v in ctx.int_vars:
            opts.append(("int-var", v))
        for v in ctx.string_vars:
            opts.append(("str-var", v))
        for v in ctx.bool_vars:
            opts.append(("bool-var", v))
        for v in ctx.list_vars:
            opts.append(("list-var", v))
        for name, kind in ctx.any_vars:
            opts.append((kind + "-var", name))
        for v in ctx.adt_vars:
            opts.append(("adt-var", v[0]))
        if ctx.fn_pool:
            opts.append(("int-call", None))
        if ctx.list_fns and ctx.list_vars:
            opts.append(("list-call", None))
        if ctx.apply_fns and (ctx.fn_vars or ctx.fn_pool):
            opts.append(("apply-call", None))
        if ctx.int_vars or True:
            opts.append(("int-expr", None))
            opts.append(("bool-expr", None))
        if len(ctx.string_vars) >= 2:
            opts.append(("str-concat", None))
        if ctx.int_vars:
            opts.append(("match-int", None))
        if ctx.string_vars:
            opts.append(("match-str", None))
        for (vn, ty) in ctx.adt_vars:
            opts.append(("match-adt", vn))
        if not opts:
            self.main_stmts.append('  print(%d);' % _pick_int_literal(rng))
            return
        kind, arg = opts[prng.prng_next_range(rng, len(opts))]

        if kind == "int-var":
            self.main_stmts.append("  print(%s);" % arg)
        elif kind == "str-var":
            self.main_stmts.append('  print(%s);' % arg)
        elif kind == "bool-var":
            self.main_stmts.append("  print(%s);" % arg)
        elif kind in ("list-var", "mixed-var", "pair-var", "symbol-var",
                      "adt-var"):
            self.main_stmts.append("  print(%s);" % arg)
        elif kind == "int-call":
            f = ctx.fn_pool[prng.prng_next_range(rng, len(ctx.fn_pool))]
            args = [gen_int_expr(ctx, 0)[0].text for _ in range(f.arity)]
            self.main_stmts.append("  print(%s(%s));" % (f.name,
                                                         ", ".join(args)))
        elif kind == "list-call":
            f = ctx.list_fns[prng.prng_next_range(rng, len(ctx.list_fns))]
            lv = ctx.list_vars[prng.prng_next_range(rng, len(ctx.list_vars))]
            self.main_stmts.append("  print(%s(%s));" % (f.name, lv))
        elif kind == "apply-call":
            f = ctx.apply_fns[prng.prng_next_range(rng, len(ctx.apply_fns))]
            if ctx.fn_vars:
                fv = ctx.fn_vars[prng.prng_next_range(rng, len(ctx.fn_vars))]
            else:
                unary = [g for g in ctx.fn_pool if g.arity == 1]
                fv = unary[prng.prng_next_range(rng, len(unary))].name \
                    if unary else str(_pick_int_literal(rng))
            iv = (ctx.int_vars[prng.prng_next_range(rng, len(ctx.int_vars))]
                  if ctx.int_vars else str(_pick_int_literal(rng)))
            self.main_stmts.append("  print(%s(%s, %s));" % (f.name, fv, iv))
        elif kind == "int-expr":
            e, _u = gen_int_expr(ctx, self.max_depth - 1)
            self.main_stmts.append("  print(%s);" % e.text)
        elif kind == "bool-expr":
            e = gen_bool_expr(ctx, self.max_depth - 1)
            self.main_stmts.append("  print(%s);" % e.text)
        elif kind == "str-concat":
            a = ctx.string_vars[prng.prng_next_range(rng,
                                                     len(ctx.string_vars))]
            b = ctx.string_vars[prng.prng_next_range(rng,
                                                     len(ctx.string_vars))]
            self.main_stmts.append('  print(str.concat(%s, %s));' % (a, b))
        elif kind == "match-int":
            v = ctx.int_vars[prng.prng_next_range(rng, len(ctx.int_vars))]
            mp = gen_match_on(ctx, v, "int", self.max_depth - 1)
            tmp = ctx.namer.fresh("mr", rng)
            self.main_stmts.append("  let %s = %s" % (tmp, mp.render(1)))
            self.main_stmts[-1] = self.main_stmts[-1].rstrip() + ";"
            self.main_stmts.append("  print(%s);" % tmp)
        elif kind == "match-str":
            v = ctx.string_vars[
                prng.prng_next_range(rng, len(ctx.string_vars))]
            mp = gen_match_on(ctx, v, "string", self.max_depth - 1)
            tmp = ctx.namer.fresh("mr", rng)
            self.main_stmts.append("  let %s = %s;" % (tmp, mp.render(1)))
            self.main_stmts.append("  print(%s);" % tmp)
        else:  # match-adt
            mp = gen_match_on(ctx, arg, self._adt_ty_of(arg),
                              self.max_depth - 1)
            tmp = ctx.namer.fresh("mr", rng)
            self.main_stmts.append("  let %s = %s;" % (tmp, mp.render(1)))
            self.main_stmts.append("  print(%s);" % tmp)

    def _adt_ty_of(self, var):
        for vn, ty in self.ctx.adt_vars:
            if vn == var:
                return ty
        raise KeyError(var)

    # -- assembly -----------------------------------------------------------

    def build(self):
        rng = self.ctx.rng
        # helper fns first (fn_pool feeds expression generation)
        n_fns = prng.prng_next_range(rng, 4)
        for _ in range(n_fns):
            sig, src = gen_helper(self.ctx, self.max_depth)
            self.fns.append((sig, src))
        self.maybe_gen_adt()
        self.gen_top_lets()
        n_prints = 4 + prng.prng_next_range(rng, 6)
        for _ in range(n_prints):
            self.gen_print()
        # guaranteed int48 boundary literal coverage in every program
        self.main_stmts.append("  print(%d);" % _I48_MAX)
        self.main_stmts.append("  print((0 - %d) - 1);" % _I48_MAX)

    def render(self):
        out = []
        out.append("// generated by tools/kernfuzz/gen.py --seed %d"
                   % self.seed)
        out.append("// subset v0 (docs/kernel-fuzzing-design.md §5.0); "
                   "full-parens per DEC-3")
        for src in self.type_decls:
            out.append(src)
            out.append("")
        for _sig, src in self.fns:
            out.append(src)
            out.append("")
        out.append("fn main() {")
        out.extend(self.main_stmts)
        out.append("}")
        return "\n".join(out) + "\n"


def gen_program(seed, max_depth=4):
    """Generate one program: returns the TA source text (deterministic)."""
    plan = ProgramPlan(seed, max_depth)
    plan.build()
    return plan.render()


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="typed TA corpus generator (kernel-fuzzing DELIV-3)")
    ap.add_argument("--seed", type=int, default=0,
                    help="PRNG seed (deterministic; default 0)")
    ap.add_argument("--max-depth", type=int, default=4,
                    help="expression depth budget (default 4)")
    ap.add_argument("--count", type=int, default=None,
                    help="generate K programs into --out-dir")
    ap.add_argument("--out-dir", type=str, default=None,
                    help="output directory for --count batch mode")
    args = ap.parse_args(argv)

    if args.count is not None:
        if not args.out_dir:
            ap.error("--count requires --out-dir")
        if not os.path.isdir(args.out_dir):
            os.makedirs(args.out_dir)
        for i in range(args.count):
            seed = prng.derive_seed(args.seed, i)
            text = gen_program(seed, args.max_depth)
            path = os.path.join(args.out_dir, "p_%d.ta" % seed)
            with open(path, "w", encoding="latin-1") as f:
                f.write(text)
            print(path)
        return 0

    sys.stdout.write(gen_program(args.seed, args.max_depth))
    return 0


if __name__ == "__main__":
    sys.exit(main())