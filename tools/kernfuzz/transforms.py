#!/usr/bin/env python3
"""Tier A + Tier B equivalence transforms on gen program trees (Option B).

Task: task-transforms (Phase 3, Tier A T1-T8) + task-tierb (Phase 8,
Tier B T9-T12).  Authoritative rule set: docs/kernel-fuzzing-design.md
§5.2, plus extra rules kept from the early task list (X1 mul-zero
guarded, X2 negate-split).

Architecture (DEC-3): gen's expressions are flattened to opaque
fully-parenthesized source text at build time, and the task forbids
restructuring gen beyond the build_program/render_tree API split.
Transforms therefore re-parse the unambiguous fully-parenthesized text
at *typed expression slots* (positions that are int- or bool-typed by
construction), rewrite the parsed expression tree, re-render it
full-paren and splice the text back into the gen tree.  Final source is
always produced by gen.render_tree — no ast-dump round-trip.

Rules (§5.2 Tier A):
  T1  commutativity      a+b<->b+a, a*b<->b*a            (T1a/T1b)
  T2  associativity      (a+b)+c<->a+(b+c), mul analog   (T2a/T2b)
  T3  identities         x+0<->x, x*1<->x, x-x<->0,
                         (0-x)+x<->0                   (T3a..T3d)
  T4  constant folding   <lit op lit><-><value>          (T4)
  T5  if literal branch  if true {x} else {y}<->x        (T5)
  T6  let inlining       let x=e; body<->body[e/x]       (T6, stmt level)
  T7  comparison duality x<y<->y>x, <=, ==, !=           (T7)
  T8  match arm reorder  needs gen tree match metadata   (T8, stmt level)
Rules (§5.2 Tier B, λ-structure; statement/line level like T6/T8):
  T9  alpha-rename       main let (single binding) or fn param ->
                         fresh tb_K name at every reference point
  T10 beta-reduce        apply_h(fn(p){ b }, e) -> b[e/p]
                         (b is brace-free => no nested binder => the
                         substitution cannot capture; probe-verified:
                         golden + VM agree on direct lambda calls)
  T11 beta-expand        (f a) -> ((fn(g){g})(f) a) in the fn-value
                         argument of an apply_h call, and any int
                         subexpression e -> (fn(g){ g })(e) in place
  T12 eta-expand/reduce  apply_h(F, e) <-> apply_h(fn(z){ F(z) }, e)
                         and let fv = F; <-> let fv = fn(z){ F(z) };
                         F over unary int helpers / let-bound fn
                         values (n = arity = 1 in every gen position;
                         n = 0 has no gen position, unit-tested)
Tier B annotation ironclad (§5.2): the fn parameters introduced by the
wrappers (T11 g, T12 z) carry NO type annotation — the spec has no
arrow types (§5.0); typecheck infers them.
Extras (early task list, kept):
  X1  a*0 -> 0           death-guarded (one-way)
  X2  a-b <-> a+(0-b)    split/join

All rules hold in Z/2^48 (commutative ring; every operation is w48
normalized), so int48 wraparound cannot break any of them.
"""

import copy
import re
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import prng                                             # noqa: E402
import gen                                              # noqa: E402

_I48_MAX = (1 << 47) - 1
_I48_MIN = -(1 << 47)


def _w48(v):
    """Normalize to the int48 domain [-2^47, 2^47-1]."""
    return ((v + (1 << 47)) % (1 << 48)) - (1 << 47)


# ---------------------------------------------------------------------------
# expression tree (parsed from a slot's fully-parenthesized text)
# ---------------------------------------------------------------------------

class Expr(object):
    __slots__ = ()


class Lit(Expr):
    """int48 literal; negative values render in R1 form `(0 - abs)`."""
    __slots__ = ("v",)

    def __init__(self, v):
        self.v = v


class Var(Expr):
    __slots__ = ("name",)

    def __init__(self, name):
        self.name = name


class Call(Expr):
    """Call to a top-level int helper.  `frozen` marks argument indices
    whose type is NOT int (list arg of list-len/sum, fn value of apply1);
    frozen args are rendered but never traversed/transformed."""
    __slots__ = ("callee", "args", "frozen")

    def __init__(self, callee, args, frozen=()):
        self.callee = callee
        self.args = args
        self.frozen = tuple(frozen)


class Bin(Expr):
    __slots__ = ("op", "left", "right")

    def __init__(self, op, left, right):
        self.op = op
        self.left = left
        self.right = right


class Cmp(Expr):
    """bool-typed comparison `l op r`; operands are int expressions.
    op in < <= > >= == !="""

    __slots__ = ("op", "left", "right")

    def __init__(self, op, left, right):
        self.op = op
        self.left = left
        self.right = right


class If(Expr):
    """`if true { t } else { e }` — literal condition only (T5 form).
    cond is a Python bool; branches are int expressions."""

    __slots__ = ("cond", "then", "els")

    def __init__(self, cond, then, els):
        self.cond = cond
        self.then = then
        self.els = els


class Wrap(Expr):
    """T11 identity wrapper `(fn(name){ name })(sub)` — an injected
    β-redex around an int-typed subexpression (§5.2 T11, second form).
    `name` is a transform-fresh identifier and carries NO type
    annotation (Tier B annotation ironclad, §5.2)."""

    __slots__ = ("name", "sub")

    def __init__(self, name, sub):
        self.name = name
        self.sub = sub


def render_expr(e):
    """Full-parenthesis rendering (DEC-3): every composite is parenthesized."""
    if isinstance(e, Lit):
        if e.v < 0:
            return "(0 - %d)" % (-e.v)
        return str(e.v)
    if isinstance(e, Var):
        return e.name
    if isinstance(e, Call):
        return "%s(%s)" % (e.callee,
                           ", ".join(render_expr(a) for a in e.args))
    if isinstance(e, Bin):
        return "(%s %s %s)" % (render_expr(e.left), e.op,
                               render_expr(e.right))
    if isinstance(e, Cmp):
        return "(%s %s %s)" % (render_expr(e.left), e.op,
                               render_expr(e.right))
    if isinstance(e, If):
        return "if %s { %s } else { %s }" % (
            "true" if e.cond else "false",
            render_expr(e.then), render_expr(e.els))
    if isinstance(e, Wrap):
        return "(fn(%s) { %s })(%s)" % (e.name, e.name,
                                        render_expr(e.sub))
    raise TypeError("cannot render %r" % (e,))


def _children(e):
    """Transformable child list: [(selector, child), ...]."""
    if isinstance(e, (Bin, Cmp)):
        return [(0, e.left), (1, e.right)]
    if isinstance(e, If):
        return [(0, e.then), (1, e.els)]      # cond is a literal, not walked
    if isinstance(e, Wrap):
        return [(0, e.sub)]
    if isinstance(e, Call):
        return [(i, a) for i, a in enumerate(e.args) if i not in e.frozen]
    return []


def _walk(e):
    """Pre-order over transformable nodes."""
    stack = [e]
    while stack:
        n = stack.pop()
        yield n
        kids = [c for _, c in _children(n)]
        stack.extend(reversed(kids))


def _get(e, path):
    for sel in path:
        for i, c in _children(e):
            if i == sel:
                e = c
                break
        else:
            raise KeyError("bad path %r" % (path,))
    return e


def _set(e, path, new_node):
    if not path:
        return new_node
    sel, rest = path[0], path[1:]
    if isinstance(e, (Bin, Cmp)):
        if sel == 0:
            return type(e)(e.op, _set(e.left, rest, new_node), e.right)
        return type(e)(e.op, e.left, _set(e.right, rest, new_node))
    if isinstance(e, If):
        if sel == 0:
            return If(e.cond, _set(e.then, rest, new_node), e.els)
        return If(e.cond, e.then, _set(e.els, rest, new_node))
    if isinstance(e, Wrap):
        return Wrap(e.name, _set(e.sub, rest, new_node))
    if isinstance(e, Call):
        args = list(e.args)
        args[sel] = _set(args[sel], rest, new_node)
        return Call(e.callee, args, e.frozen)
    raise TypeError("cannot descend into %r" % (e,))


def has_death_risk(e):
    """True if the subtree can die (DIVZERO death protocol, §5.1.3):
    contains a division or remainder.  Helper calls are safe (gen R10:
    every generated program terminates).  gen v0 emits no `/`/`%`, so
    this is False on all gen-producible trees today."""
    return any(isinstance(n, Bin) and n.op in ("/", "%") for n in _walk(e))


# ---------------------------------------------------------------------------
# slot parsing
# ---------------------------------------------------------------------------

class SlotReject(Exception):
    """The text is not a transformable typed expression."""


_IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_DIGITS_RE = re.compile(r"[0-9]+")

_RESERVED = frozenset([
    "true", "false", "nil", "cons", "match", "fn", "let", "print",
    "type", "when", "str", "head", "tail", "if", "else",
])


def _tokenize(s):
    toks = []
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c.isspace():
            i += 1
            continue
        if c in "(),{}":
            toks.append((c, c))
            i += 1
            continue
        if c in "+-*":
            toks.append(("op", c))
            i += 1
            continue
        if s.startswith(("<=", ">=", "==", "!="), i):
            toks.append(("cmp", s[i:i + 2]))
            i += 2
            continue
        if c in "<>":
            toks.append(("cmp", c))
            i += 1
            continue
        if c in "/%":
            toks.append(("op", c))
            i += 1
            continue
        m = _DIGITS_RE.match(s, i)
        if m:
            toks.append(("int", int(m.group())))
            i = m.end()
            continue
        m = _IDENT_RE.match(s, i)
        if m:
            toks.append(("ident", m.group()))
            i = m.end()
            continue
        raise SlotReject("bad char %r at %d in %r" % (c, i, s))
    return toks


class _Types(object):
    """Recovered type context for one program tree.

    int_vars is the UNION over all scopes (used only to validate that
    an existing, gen-produced text is well typed).  Injection rules must
    use the SCOPE-LOCAL set (slot.int_vars): main statements may only
    reference main-scope names, helper bodies their params/template
    binders — an out-of-scope name silently changes semantics (the VM
    treats unbound identifiers as 0, golden as nil)."""

    def __init__(self, plan):
        # name -> (arity, kind); kind in int/list-len/list-sum/apply1/match
        self.helpers = dict((sig.name, (sig.arity, sig.kind))
                            for sig, _src in plan.fns)
        self.ctors = self._ctor_names(plan.type_decls)
        self.int_vars = set()      # union of all scopes (validation only)
        self.str_vars = set()      # main-scope string identifiers
        # main-scope int identifiers as ORDERED bindings: (name, key)
        # where key = (stmt_idx, line_idx) marks where the binding takes
        # effect (for multi-line match / lambda lets: end of statement,
        # so arm bodies never see the result binder).  A slot may only
        # reference bindings whose key is < the slot's own key.
        self.main_bindings = []
        self.fn_vars = {}          # fn index -> set (params)
        # match binder names are visible ONLY inside their arm body /
        # guard — kept out of main/fn scopes so injection rules cannot
        # hoist them past the match (an out-of-scope name silently
        # changes semantics: VM reads 0, golden reads nil)
        self.binder_vars = set()
        self._collect(plan)

    def scope_vars(self, sid):
        if sid[0] == "main":
            key = (sid[1], sid[2])
            return set(nm for nm, k in self.main_bindings if k < key)
        return self.fn_vars.get(sid[1], set())

    def _bind(self, name, scope):
        scope.add(name)
        self.int_vars.add(name)

    def _bind_main(self, name, key):
        """Record a main-scope binding effective at `key`."""
        self.int_vars.add(name)
        self.main_bindings.append((name, key))

    @staticmethod
    def _ctor_names(decls):
        """Variant names from `type Ty_1 { Mk_1; Mk_2(f_1) }` strings."""
        out = set()
        for src in decls:
            body = src[src.index("{") + 1: src.rindex("}")]
            for part in body.split(";"):
                part = part.strip()
                if not part:
                    continue
                m = re.match(r"([A-Za-z_]\w*)", part)
                if m:
                    out.add(m.group(1))
        return out

    def _collect(self, plan):
        for fi, (_sig, src) in enumerate(plan.fns):
            scope = self.fn_vars.setdefault(fi, set())
            m = re.search(r"fn ([A-Za-z_]\w*)\(([^)]*)\)", src)
            if m:
                for prm in m.group(2).split(","):
                    prm = prm.strip()
                    if prm.endswith(": int"):
                        nm = prm[:-len(": int")].strip()
                        scope.add(nm)
                        self.int_vars.add(nm)
            for line in src.split("\n"):
                self._scan_line(line, scope)
        for si, stmt in enumerate(plan.main_stmts):
            lines = stmt.split("\n")
            # a binding takes effect at end of statement: slots inside a
            # multi-line match / lambda never see its result binder, but
            # every later statement does
            eff = (si, 0) if len(lines) == 1 else (si, len(lines))
            # binder typing: a bare match binder is an int operand only
            # when the scrutinee is int (string / ADT matches bind
            # non-int names — arithmetic on them silently diverges)
            arm_kind = None
            m = re.match(r"\s*let [A-Za-z_]\w* = match ([A-Za-z_]\w*) \{",
                         lines[0])
            if m:
                s = m.group(1)
                intn = set(nm for nm, _k in self.main_bindings)
                arm_kind = "int" if s in intn else (
                    "str" if s in self.str_vars else None)
            for line in lines:
                self._scan_line(line, None, eff, arm_kind)

    def _scan_line(self, line, scope, eff=None, arm_kind=None):
        # lambda binding: the param is int but scoped to the lambda body
        # only (never hoistable out of it); the let name itself is a
        # function value (R8) — never an int operand, so it is NOT bound
        # into any int scope
        m = re.match(r"\s*let ([A-Za-z_]\w*) = fn\(([A-Za-z_]\w*)\) \{",
                     line)
        if m:
            self._bind(m.group(2), self.binder_vars)
            return
        # multi-line match binding: result is always int (all gen matches
        # produce int bodies)
        m = re.match(r"\s*let ([A-Za-z_]\w*) = match ", line)
        if m:
            if scope is None:
                self._bind_main(m.group(1), eff)
            else:
                self._bind(m.group(1), scope)
            return
        if " -> " in line:
            _arm_binders(line, self, arm_kind)
            return
        m = re.match(r"\s*let ([A-Za-z_]\w*) = (.+);", line)
        if m:
            r = m.group(2).strip()
            if r[:1] in ("'", '"'):
                self.str_vars.add(m.group(1))
                return
            if _classify_rhs(m.group(2), self) == "int":
                if scope is None:
                    self._bind_main(m.group(1), eff)
                else:
                    self._bind(m.group(1), scope)
            return
        # single-expression fn body that parses as an int expr: nothing
        # to record (its identifiers are params, already typed above)




def _arm_binders(line, types, kind=None):
    """Binders of `PAT -> BODY` / `PAT when (G) -> BODY`.  All binders go
    into binder_vars (validation union, never an enclosing scope).
    Returns the names usable as INT operands in this arm's body / guard
    (injection extras): a bare binder only for an int-scrutinee match
    (`kind == "int"`); cons(h, t) elements in helper templates (the only
    place gen emits cons patterns, int-list helpers).  String / ADT
    binders are never int operands — arithmetic on them silently
    diverges (VM reads 0, golden reads nil)."""
    head = re.split(r" when | -> ", line, maxsplit=1)[0].strip()
    head = head.rstrip(",")
    if head == "_" or head in ("nil", "true", "false"):
        return ()
    if head.startswith('"') or head.startswith("'"):
        return ()
    m = re.match(r"cons\((.+), (.+)\)$", head)
    if m:
        h = m.group(1).strip()
        t = m.group(2).strip()
        types._bind(h, types.binder_vars)
        types._bind(t, types.binder_vars)
        return (h, t) if kind in (None, "list") else ()
    if _IDENT_RE.fullmatch(head) and head not in types.ctors \
            and head not in _RESERVED and head not in types.helpers:
        types._bind(head, types.binder_vars)
        return (head,) if kind == "int" else ()
    return ()


def _classify_rhs(rhs, types):
    """Coarse type of a `let` RHS: 'int' | 'other'.  Function-value
    bindings (lambda literal / bare fn name) are NEVER int operands —
    they are passed only to apply-kind helpers.  A parsed Cmp root is
    bool ('other'); a parsed If root is int."""
    r = rhs.strip()
    if r.startswith(('"', "'", "[", "fn(")):
        return "other"
    if r in ("true", "false"):
        return "other"
    if r.startswith("cons("):
        return "other"
    if r in types.helpers:            # bare top-level fn name (fn value)
        return "other"
    try:
        root = _parse_int_expr(rhs, types)
        return "other" if isinstance(root, Cmp) else "int"
    except SlotReject:
        pass
    if r in types.int_vars:
        return "int"
    return "other"


class _Parser(object):
    """Recursive-descent parser for fully-parenthesized typed expressions.

    Grammar (gen output is fully parenthesized, so precedence is
    belt-and-braces):
      cmp   := expr (CMPOP expr)?          -- non-associative, bool result
      expr  := term (('+'|'-') term)*
      term  := factor (('*'|'/'|'%') factor)*
      factor: INT | ident | ident(args) | '(' cmp ')'
            | 'if' ('true'|'false') '{' cmp '}' 'else' '{' cmp '}'

    Every operand is validated against the type context: literals must
    be in the int48 literal domain (gen R2), variables must be typed
    int, calls must target a known top-level helper with exact arity and
    per-argument types.  bool-typed subexpressions (Cmp) may only appear
    as the root or inside if-conditions (never generated — literal
    conditions only), never as an operand of Bin/Call/Cmp/If.
    """

    def __init__(self, text, types):
        self.toks = _tokenize(text)
        self.pos = 0
        self.types = types

    def _peek(self):
        return self.toks[self.pos] if self.pos < len(self.toks) else (None,
                                                                      None)

    def _next(self):
        t = self._peek()
        self.pos += 1
        return t

    def parse(self):
        e = self.cmp()
        if self.pos != len(self.toks):
            raise SlotReject("trailing tokens")
        _validate_no_bool_operand(e)
        return e

    def cmp(self):
        e = self.expr()
        k, v = self._peek()
        if k == "cmp":
            self._next()
            r = self.expr()
            return Cmp(v, e, r)
        return e

    def expr(self):
        e = self.term()
        while self._peek() == ("op", "+") or self._peek() == ("op", "-"):
            op = self._next()[1]
            e = Bin(op, e, self.term())
        return e

    def term(self):
        e = self.factor()
        while self._peek()[0] == "op" \
                and self._peek()[1] in ("*", "/", "%"):
            op = self._next()[1]
            e = Bin(op, e, self.factor())
        return e

    def factor(self):
        kind, val = self._next()
        if kind == "int":
            if not (0 <= val <= _I48_MAX):
                raise SlotReject("literal out of int48 domain: %d" % val)
            return Lit(val)
        if kind == "ident":
            if val == "if":
                return self._if_form()
            if val in _RESERVED or val in self.types.ctors:
                raise SlotReject("reserved/ctor identifier %r" % val)
            nk, _nv = self._peek()
            if nk == "(":
                self._next()
                return self._call(val)
            if val in self.types.helpers:
                raise SlotReject("bare helper name %r in operand position"
                                 % val)
            if val not in self.types.int_vars:
                raise SlotReject("non-int identifier %r" % val)
            return Var(val)
        if kind == "(":
            e = self.cmp()
            if self._next()[0] != ")":
                raise SlotReject("expected )")
            return e
        raise SlotReject("unexpected token %r" % (val,))

    def _if_form(self):
        k, v = self._next()
        if k != "ident" or v not in ("true", "false"):
            raise SlotReject("if condition must be true/false literal")
        cond = (v == "true")
        if self._next()[0] != "{":
            raise SlotReject("expected { after if condition")
        t = self.cmp()
        if self._next()[0] != "}":
            raise SlotReject("expected } after if-then branch")
        k, v = self._next()
        if k != "ident" or v != "else":
            raise SlotReject("expected else")
        if self._next()[0] != "{":
            raise SlotReject("expected { after else")
        e = self.cmp()
        if self._next()[0] != "}":
            raise SlotReject("expected } after if-else branch")
        return If(cond, t, e)

    def _call(self, callee):
        spec = self.types.helpers.get(callee)
        if spec is None:
            raise SlotReject("call to non-helper %r" % callee)
        arity, kind = spec
        args = []
        frozen = []
        if self._peek()[0] != ")":
            while True:
                idx = len(args)
                is_frozen = ((kind in ("list-len", "list-sum") and idx == 0)
                             or (kind == "apply1" and idx == 0))
                if is_frozen:
                    # list-typed / fn-value-typed argument: accept a bare
                    # literal-or-identifier atom without int validation;
                    # frozen args are rendered but never transformed.
                    args.append(self._frozen_atom())
                    frozen.append(idx)
                else:
                    args.append(self.expr())
                nk, _nv = self._next()
                if nk == ",":
                    continue
                if nk == ")":
                    break
                raise SlotReject("expected , or ) in call")
        else:
            self._next()
        if len(args) != arity:
            raise SlotReject("arity mismatch calling %r" % callee)
        return Call(callee, args, frozen)

    def _frozen_atom(self):
        kind, val = self._next()
        if kind == "int":
            if not (0 <= val <= _I48_MAX):
                raise SlotReject("literal out of int48 domain: %d" % val)
            return Lit(val)
        if kind == "ident":
            if val in _RESERVED or val in self.types.ctors:
                raise SlotReject("reserved/ctor in frozen arg: %r" % val)
            return Var(val)               # opaque: frozen, never walked
        raise SlotReject("bad frozen argument token %r" % (val,))


def _validate_no_bool_operand(e):
    """A Cmp (bool) must never sit in operand position of Bin/Call/Cmp
    or an If branch (int positions)."""
    for n in _walk(e):
        kids = [c for _, c in _children(n)]
        if isinstance(n, Cmp):
            bad = kids                       # operands must be int
        else:
            bad = [c for c in kids if isinstance(c, Cmp)]
        for c in bad:
            if not (isinstance(n, Cmp) and c is n):
                if isinstance(c, Cmp):
                    raise SlotReject("bool expression in int position")


def _parse_int_expr(text, types):
    return _Parser(text, types).parse()


# ---------------------------------------------------------------------------
# slot discovery
# ---------------------------------------------------------------------------

class Slot(object):
    """One transformable typed-expression position in the program tree."""

    __slots__ = ("sid", "line", "start", "end", "text", "root", "roottype",
                 "int_vars")

    def __init__(self, sid, line, start, end, text, root, roottype,
                 int_vars):
        self.sid = sid          # ("main", stmt_i, line_i) | ("fn", fn_i, line_i)
        self.line = line        # the full source line containing the slot
        self.start = start      # slot span within line
        self.end = end
        self.text = text
        self.root = root        # parsed Expr
        self.roottype = roottype        # "int" | "bool"
        self.int_vars = int_vars        # frozenset of in-scope int names

    def __repr__(self):
        return "Slot(%r, %r)" % (self.sid, self.text)


_LET_RE = re.compile(r"^(\s*)let ([A-Za-z_]\w*) = (.+);(\s*//.*)?$")
_LAMBDA_RE = re.compile(r"^(\s*)let [A-Za-z_]\w* = fn\(([A-Za-z_]\w*)\) "
                        r"\{ (.*) \};$")
_PRINT_RE = re.compile(r"^(\s*)print\((.*)\);$")




def _mk_slot(sid, line, start, end, types, extra=()):
    text = line[start:end]
    try:
        root = _parse_int_expr(text, types)
    except SlotReject:
        return None
    rt = "bool" if isinstance(root, Cmp) else "int"
    names = types.scope_vars(sid) | set(extra)
    return Slot(sid, line, start, end, text, root, rt, frozenset(names))


def find_int_slots(plan):
    """All transformable typed-expression positions in a program tree
    (int slots plus bool slots: comparison roots usable by T7)."""
    types = _Types(plan)
    return _find_slots(plan, types)


def _find_slots(plan, types):
    slots = []

    def scan_lines(sid_prefix, lines, arm_kind=None):
        for li, line in enumerate(lines):
            m = _LET_RE.match(line)
            if m and "match " not in m.group(3) and "{ }" not in line:
                rhs = m.group(3)
                lam = _LAMBDA_RE.match(line)
                if lam:
                    start = line.index("{ ") + 2
                    end = len(line) - len(" };") \
                        if line.endswith(" };") else line.rindex(" }")
                    s = _mk_slot(sid_prefix + (li,), line, start, end,
                                 types,
                                 extra=(lam.group(2),))
                    if s:
                        slots.append(s)
                    continue
                if "=" in rhs and "->" in rhs:
                    continue                    # match binding (multiline)
                eq = line.index(" = ") + 3
                semi = line.rindex(";")
                s = _mk_slot(sid_prefix + (li,), line, eq, semi, types)
                if s:
                    slots.append(s)
                continue
            m = _PRINT_RE.match(line)
            if m:
                s = _mk_slot(sid_prefix + (li,), line,
                             line.index("(") + 1, line.rindex(");"),
                             types)
                if s:
                    slots.append(s)
                continue
            if " -> " in line:
                # match arm: transform only the BODY (after the last
                # "->").  Guard (between `when (` and `) ->`) becomes a
                # separate bool slot (T7 comparison duality); the
                # PATTERN is never touched.
                widx = line.find(" when (")
                binders = _arm_binders(line, types, arm_kind)
                if widx >= 0:
                    gstart = widx + len(" when (")
                    gend = line.find(") -> ", gstart)
                    if gend > gstart:
                        s = _mk_slot(sid_prefix + (li,), line, gstart,
                                     gend, types, extra=binders)
                        if s:
                            slots.append(s)
                idx = line.rindex(" -> ") + len(" -> ")
                core = line[idx:].rstrip()
                if core.endswith(",") or core.endswith(";"):
                    core = core[:-1]
                start = idx
                end = idx + len(core)
                s = _mk_slot(sid_prefix + (li,), line, start, end, types,
                             extra=binders)
                if s:
                    slots.append(s)
                continue
            # single-expression helper body (indent, no keyword, no brace)
            st = line.strip()
            if st and not st.startswith(("fn ", "}", "{", "match ", "let ",
                                         "print(")) and "{" not in st \
                    and "}" not in st:
                start = line.index(st)
                s = _mk_slot(sid_prefix + (li,), line, start,
                             start + len(st), types)
                if s:
                    slots.append(s)

    int_names = set(nm for nm, _k in types.main_bindings)
    for si, stmt in enumerate(plan.main_stmts):
        lines = stmt.split("\n")
        kind = None
        m = re.match(r"\s*let [A-Za-z_]\w* = match ([A-Za-z_]\w*) \{",
                     lines[0])
        if m:
            s = m.group(1)
            kind = "int" if s in int_names else (
                "str" if s in types.str_vars else None)
        scan_lines(("main", si), lines, kind)
    for fi, (_sig, src) in enumerate(plan.fns):
        scan_lines(("fn", fi), src.split("\n"), None)
    return slots


# ---------------------------------------------------------------------------
# the rules (§5.2 Tier A T1-T8 + extras X1/X2)
# ---------------------------------------------------------------------------

RULE_NAMES = {
    "T1a": "add-commute",       # a+b <=> b+a
    "T1b": "mul-commute",       # a*b <=> b*a
    "T2a": "add-assoc",         # (a+b)+c <=> a+(b+c)
    "T2b": "mul-assoc",         # (a*b)*c <=> a*(b*c)
    "T3a": "add-identity",      # x+0 <=> x
    "T3b": "mul-identity",      # x*1 <=> x
    "T3c": "sub-self",          # x-x <=> 0
    "T3d": "neg-cancel",        # (0-x)+x <=> 0
    "T4": "const-fold",         # <lit op lit> <=> <value>
    "T5": "if-literal",         # if true {x} else {y} <=> x (false sym)
    "T6": "let-inline",         # let x=e; body <=> body[e/x]
    "T7": "cmp-dual",           # x<y <=> y>x (and <=, ==, !=)
    "T8": "match-reorder",      # match arm permutation
        "X1": "mul-zero",           # a*0 -> 0   (extra, death-guarded)
    "X2": "negate-split",       # a-b <=> a+(0-b)   (extra)
    "T9": "alpha-rename",       # let/fn-param -> fresh tb_K name
    "T10": "beta-reduce",       # apply_h(fn(p){b}, e) -> b[e/p]
    "T11": "beta-expand",       # identity wrapper (inject a redex)
    "T12": "eta-expand",        # f <-> fn(z){ f(z) } (+ reverse)
}

# tier bookkeeping (§5.2): Tier A arithmetic algebra / Tier B λ-structure
TIER_OF_RULE = {}
for _r in ("T1a", "T1b", "T2a", "T2b", "T3a", "T3b", "T3c", "T3d",
           "T4", "T5", "T6", "T7", "T8", "X1", "X2"):
    TIER_OF_RULE[_r] = "A"
for _r in ("T9", "T10", "T11", "T12"):
    TIER_OF_RULE[_r] = "B"

# rule subsets per tier ("all" = every rule; morph --tier consumes this)
TIER_RULES = {
    "A": ("T1a", "T1b", "T2a", "T2b", "T3a", "T3b", "T3c", "T3d",
          "T4", "T5", "T6", "T7", "T8", "X1", "X2"),
    "B": ("T9", "T10", "T11", "T12"),
}

_T4_EXPAND_SPLITS = (1, 2, 3, 7, 100)


def _is_lit(e, v):
    return isinstance(e, Lit) and e.v == v


def _int_var_picks(slot):
    """Deterministic small operand picks for 0-expansion rules."""
    picks = sorted(slot.int_vars)[:2] if slot is not None else []
    return [Var(nm) for nm in picks] + [Lit(0)]


def _t1a_dirs(n):
    if isinstance(n, Bin) and n.op == "+":
        return [("swap", Bin("+", n.right, n.left))]
    return []


def _t1b_dirs(n):
    if isinstance(n, Bin) and n.op == "*":
        return [("swap", Bin("*", n.right, n.left))]
    return []


def _t2a_dirs(n):
    """(a+b)+c <=> a+(b+c)."""
    if isinstance(n, Bin) and n.op == "+":
        if isinstance(n.left, Bin) and n.left.op == "+":
            a, b, c = n.left.left, n.left.right, n.right
            return [("right-assoc", Bin("+", a, Bin("+", b, c)))]
        if isinstance(n.right, Bin) and n.right.op == "+":
            a, b, c = n.left, n.right.left, n.right.right
            return [("left-assoc", Bin("+", Bin("+", a, b), c))]
    return []


def _t2b_dirs(n):
    if isinstance(n, Bin) and n.op == "*":
        if isinstance(n.left, Bin) and n.left.op == "*":
            a, b, c = n.left.left, n.left.right, n.right
            return [("right-assoc", Bin("*", a, Bin("*", b, c)))]
        if isinstance(n.right, Bin) and n.right.op == "*":
            a, b, c = n.left, n.right.left, n.right.right
            return [("left-assoc", Bin("*", Bin("*", a, b), c))]
    return []


def _t3a_dirs(n):
    """x+0 <=> x, both directions (elim + inject)."""
    out = []
    if isinstance(n, Bin) and n.op == "+":
        if _is_lit(n.right, 0):
            out.append(("elim", n.left))
        if _is_lit(n.left, 0):
            out.append(("elim", n.right))
    if not isinstance(n, Cmp):
        out.append(("inject", Bin("+", n, Lit(0))))
    return out


def _t3b_dirs(n):
    out = []
    if isinstance(n, Bin) and n.op == "*":
        if _is_lit(n.right, 1):
            out.append(("elim", n.left))
        if _is_lit(n.left, 1):
            out.append(("elim", n.right))
    if not isinstance(n, Cmp):
        out.append(("inject", Bin("*", n, Lit(1))))
    return out


def _t3c_dirs(n, slot):
    """x-x <=> 0.  Inject expands a literal 0 into (v - v)."""
    out = []
    if isinstance(n, Bin) and n.op == "-" \
            and render_expr(n.left) == render_expr(n.right):
        out.append(("elim", Lit(0)))
    if isinstance(n, Lit) and n.v == 0:
        for v in _int_var_picks(slot):
            out.append(("inject", Bin("-", v, copy.deepcopy(v))))
    return out


def _t3d_dirs(n, slot):
    """(0-x)+x <=> 0 (both operand orders)."""
    out = []
    if isinstance(n, Bin) and n.op == "+":
        if isinstance(n.left, Bin) and n.left.op == "-" \
                and _is_lit(n.left.left, 0) \
                and render_expr(n.left.right) == render_expr(n.right):
            out.append(("elim", Lit(0)))
        if isinstance(n.right, Bin) and n.right.op == "-" \
                and _is_lit(n.right.left, 0) \
                and render_expr(n.right.right) == render_expr(n.left):
            out.append(("elim", Lit(0)))
    if isinstance(n, Lit) and n.v == 0:
        for v in _int_var_picks(slot):
            out.append(("inject", Bin("+", Bin("-", Lit(0),
                                              copy.deepcopy(v)),
                                      copy.deepcopy(v))))
    return out


def _t4_dirs(n):
    """<lit op lit> <=> <value>.  Fold per int48; result -2^47 has no
    renderable literal (R2 domain ends at |v| <= 2^47-1), so skip it.
    Expand direction: value -> (lit + lit) at fixed split points."""
    out = []
    if isinstance(n, Bin) and n.op in ("+", "-", "*") \
            and isinstance(n.left, Lit) and isinstance(n.right, Lit):
        a, b = n.left.v, n.right.v
        v = _w48(a + b if n.op == "+" else
                 a - b if n.op == "-" else a * b)
        if v != _I48_MIN:                 # renderable as (0 - abs) form
            out.append(("fold", Lit(v)))
    if isinstance(n, Lit):
        for a in _T4_EXPAND_SPLITS:
            b = n.v - a                   # exact split, no wraparound
            if -_I48_MAX <= b <= _I48_MAX and b != 0:
                out.append(("expand", Bin("+", Lit(a), Lit(b))))
    return out


def _t5_dirs(n):
    """if true {x} else {y} <=> x (false symmetric), elim + inject."""
    out = []
    if isinstance(n, If):
        out.append(("elim-true", n.then) if n.cond
                   else ("elim-false", n.els))
    if not isinstance(n, Cmp):
        out.append(("inject-true", If(True, n, Lit(0))))
        out.append(("inject-false", If(False, Lit(0), n)))
    return out


def _t7_dirs(n):
    """Comparison duality: (x<y) <=> (y>x), <=, ==, != analogues."""
    dual = {"<": ">", "<=": ">=", "==": "==", "!=": "!="}
    if isinstance(n, Cmp) and n.op in dual:
        return [("dual", Cmp(dual[n.op], n.right, n.left))]
    return []


def _x1_dirs(n):
    """`a * 0 -> 0`, one-way, guarded against death subexpressions."""
    out = []
    if isinstance(n, Bin) and n.op == "*":
        if _is_lit(n.right, 0) and not has_death_risk(n.left):
            out.append(("elim", Lit(0)))
        if _is_lit(n.left, 0) and not has_death_risk(n.right):
            out.append(("elim", Lit(0)))
    return out


def _x2_dirs(n):
    """a-b <=> a+(0-b): split under '-', join under '+' (the join
    pattern is `a+(0-b) -> a-b`; a `(0-x)-y` form has NO join partner)."""
    out = []
    if isinstance(n, Bin) and n.op == "-":
        out.append(("split", Bin("+", n.left,
                                 Bin("-", Lit(0), n.right))))
    if isinstance(n, Bin) and n.op == "+":
        if isinstance(n.right, Bin) and n.right.op == "-" \
                and _is_lit(n.right.left, 0):
            out.append(("join", Bin("-", n.left, n.right.right)))
        if isinstance(n.left, Bin) and n.left.op == "-" \
                and _is_lit(n.left.left, 0):
            out.append(("join", Bin("-", n.right, n.left.right)))
    return out


# expr rule -> node[, slot] -> [(direction, replacement)]
_EXPR_RULES = {
    "T1a": _t1a_dirs, "T1b": _t1b_dirs,
    "T2a": _t2a_dirs, "T2b": _t2b_dirs,
    "T3a": _t3a_dirs, "T3b": _t3b_dirs,
    "T3c": _t3c_dirs, "T3d": _t3d_dirs,
    "T4": _t4_dirs, "T5": _t5_dirs, "T7": _t7_dirs,
    "X1": _x1_dirs, "X2": _x2_dirs,
}
_STMT_RULES = ("T6", "T8")


# ---------------------------------------------------------------------------
# application
# ---------------------------------------------------------------------------

def _expr_rule_dirs(rule, node, slot):
    fn = _EXPR_RULES[rule]
    if rule in ("T3c", "T3d"):
        return fn(node, slot)
    return fn(node)


def _preorder_paths(root):
    out = []
    stack = [(())]
    nodes = [root]
    while stack:
        path = stack.pop()
        node = nodes.pop()
        out.append((path, node))
        kids = _children(node)
        for sel, child in reversed(kids):
            stack.append(path + (sel,))
            nodes.append(child)
    return out


def _copy_plan(plan):
    new = copy.copy(plan)
    new.main_stmts = list(plan.main_stmts)
    new.fns = list(plan.fns)
    new.match_meta = list(getattr(plan, "match_meta", []) or [])
    return new


def _splice(plan, slot, new_root):
    """Return a new program tree with `slot` replaced by new_root."""
    new_text = render_expr(new_root)
    new_line = slot.line[:slot.start] + new_text + slot.line[slot.end:]
    new_plan = _copy_plan(plan)
    kind, idx, li = slot.sid[0], slot.sid[1], slot.sid[2]
    if kind == "main":
        lines = new_plan.main_stmts[idx].split("\n")
        lines[li] = new_line
        new_plan.main_stmts[idx] = "\n".join(lines)
    else:
        sig, src = new_plan.fns[idx]
        lines = src.split("\n")
        lines[li] = new_line
        new_plan.fns[idx] = (sig, "\n".join(lines))
    return new_plan


def _inline_candidates(plan, types, slots):
    """T6 inline direction: `let x = e;` in main with x bound exactly
    once (no shadow chain — that IS the alpha-renaming-safe form; gen
    only ever rebinds via the explicit shadow chain, which is excluded
    here), all later references inside int-slot spans, and the doc
    condition: e contains no / %  OR  x referenced >= 1 time."""
    cands = []
    stmts = plan.main_stmts
    slot_by_key = {}
    for s in slots:
        if s.sid[0] == "main":
            slot_by_key.setdefault((s.sid[1], s.sid[2]), []).append(s)
    for i, stmt in enumerate(stmts):
        lines = stmt.split("\n")
        if len(lines) != 1:
            continue
        m = _LET_RE.match(lines[0])
        if not m or _LAMBDA_RE.match(lines[0]):
            continue
        x, rhs = m.group(2), m.group(3)
        try:
            e = _parse_int_expr(rhs, types)
        except SlotReject:
            continue
        if isinstance(e, Cmp):
            continue
        bind_re = re.compile(r"\blet %s =" % re.escape(x))
        if sum(len(bind_re.findall(s2)) for s2 in stmts) != 1:
            continue                      # shadow chain: not inlinable
        word_re = re.compile(r"\b%s\b" % re.escape(x))
        refs = []                          # (j, li, start, end)
        ok = True
        for j in range(i + 1, len(stmts)):
            for li, ln in enumerate(stmts[j].split("\n")):
                spans = slot_by_key.get((j, li), [])
                for m2 in word_re.finditer(ln):
                    if not any(s.start <= m2.start() < s.end
                               for s in spans):
                        ok = False
                        break
                    refs.append((j, li, m2.start(), m2.end()))
                if not ok:
                    break
            if not ok:
                break
        if not ok:
            continue                       # ref outside known slot: skip
        if not refs and has_death_risk(e):
            continue                       # doc T6 death condition
        e_text = render_expr(e)

        def applier(plan=plan, i=i, refs=tuple(refs), e_text=e_text):
            new = _copy_plan(plan)
            by_line = {}
            for j, li, a, b in refs:
                by_line.setdefault((j, li), []).append((a, b))
            for (j, li), spans in by_line.items():
                lines2 = new.main_stmts[j].split("\n")
                ln = lines2[li]
                for a, b in sorted(spans, reverse=True):
                    ln = ln[:a] + e_text + ln[b:]
                lines2[li] = ln
                new.main_stmts[j] = "\n".join(lines2)
            del new.main_stmts[i]
            # match_meta stmt indices shift past the deleted let
            new.match_meta = [(a - 1 if a > i else a, d)
                              for a, d in plan.match_meta]
            return new

        cands.append(("T6", "inline", applier, "let %s" % x,
                      "inline (%d refs)" % len(refs), None, None))
    return cands


_INL_NAME_RE = re.compile(r"\binl_(\d+)\b")


def _next_inl_k(plan):
    mx = 0
    texts = list(plan.main_stmts) + [src for _s, src in plan.fns]
    for t in texts:
        for m in _INL_NAME_RE.finditer(t):
            mx = max(mx, int(m.group(1)))
    return mx + 1


def _hoist_candidates(plan, slots):
    """T6 hoist direction (inject): extract one main-slot subexpression
    into a fresh `let inl_K = e;` placed before its statement."""
    cands = []
    for slot in slots:
        if slot.sid[0] != "main":
            continue                        # helper vars not in main scope
        if _LAMBDA_RE.match(slot.line):
            continue                        # lambda body has a separate scope
        for path, node in _preorder_paths(slot.root):
            if not isinstance(node, (Bin, Call, If)):
                continue
            text = render_expr(node)

            def applier(plan=plan, slot=slot, path=path, text=text):
                new = _copy_plan(plan)
                name = "inl_%d" % _next_inl_k(plan)
                kind_, idx, li = slot.sid
                lines = new.main_stmts[idx].split("\n")
                lines[li] = slot.line[:slot.start] + render_expr(
                    _set(slot.root, path, Var(name))) + slot.line[slot.end:]
                new.main_stmts[idx] = "\n".join(lines)
                new.main_stmts.insert(idx, "  let %s = %s;" % (name, text))
                new.match_meta = [(a + 1 if a >= idx else a, d)
                                  for a, d in plan.match_meta]
                return new

            cands.append(("T6", "hoist", applier, text, "hoist",
                          path, slot.sid))
    return cands


def _arm_pattern(line):
    return re.split(r" when | -> ", line, maxsplit=1)[0].strip().rstrip(",")


def _pattern_info(p, types):
    p = p.strip()
    if p == "_":
        return ("other", None)
    if _DIGITS_RE.fullmatch(p):
        return ("int", int(p))
    if p.startswith('"'):
        return ("str", p)
    name = p.split("(")[0].strip()
    if name in ("nil", "cons") or name in types.ctors:
        return ("ctor", name)
    return ("other", None)                 # binder: overlaps everything


def _patterns_disjoint(pa, pb, types):
    ka, va = _pattern_info(pa, types)
    kb, vb = _pattern_info(pb, types)
    if ka == "other" or kb == "other":
        return False
    if ka != kb:
        return True                        # int vs str vs ctor: disjoint
    return va != vb


def _t8_candidates(plan, types):
    """T8: match arm reorder.  Doc rule: with a wildcard arm it must be
    last and only non-wildcard arms may swap; without a wildcard, gen's
    pairwise-disjoint metadata must be set.  Safety refinement (doc gap,
    see progress.md): even with a wildcard we only swap two arms whose
    PATTERNS are pairwise disjoint — swapping e.g. `5 -> A` with
    `m when (m<10) -> B` changes the result for scrutinee 5."""
    cands = []
    for stmt_idx, disj in (getattr(plan, "match_meta", None) or []):
        if stmt_idx >= len(plan.main_stmts):
            continue
        lines = plan.main_stmts[stmt_idx].split("\n")
        arm_idx = [k for k, l in enumerate(lines) if " -> " in l]
        if len(arm_idx) < 2:
            continue
        pats = dict((k, _arm_pattern(lines[k])) for k in arm_idx)
        wildcard_last = pats[arm_idx[-1]] == "_"
        if not wildcard_last and not disj:
            continue                       # doc: no wildcard => need meta
        for ai in range(len(arm_idx)):
            for bi in range(ai + 1, len(arm_idx)):
                ka, kb = arm_idx[ai], arm_idx[bi]
                if pats[ka] == "_" or pats[kb] == "_":
                    continue               # wildcard never moves
                if not _patterns_disjoint(pats[ka], pats[kb], types):
                    continue
                cands.append(("T8", "swap",
                              _mk_swap_applier(plan, stmt_idx, ka, kb),
                              "arms %d/%d" % (ka, bi), "swap",
                              None, ("main", stmt_idx)))
    return cands


def _mk_swap_applier(plan, stmt_idx, la, lb):
    def applier(plan=plan, stmt_idx=stmt_idx, la=la, lb=lb):
        new = _copy_plan(plan)
        lines = new.main_stmts[stmt_idx].split("\n")
        lines[la], lines[lb] = lines[lb], lines[la]
        # normalize arm separators: "," on every arm except the last
        arm_lines = [k for k, l in enumerate(lines) if " -> " in l]
        for k in arm_lines:
            core = lines[k].rstrip()
            if core.endswith(","):
                core = core[:-1]
            if k != arm_lines[-1]:
                core += ","
            lines[k] = core
        new.main_stmts[stmt_idx] = "\n".join(lines)
        return new

    return applier


# ---------------------------------------------------------------------------
# Tier B candidates (§5.2 T9-T12; line-level like T6/T8)
# ---------------------------------------------------------------------------

_FNLET_RE = re.compile(r"^(\s*)let ([A-Za-z_]\w*) = (.+);(\s*//.*)?$")
_LAMBDA_ATOM_RE = re.compile(r"fn\(([A-Za-z_]\w*)\) \{ ([^{}]*) \}")
_TB_NAME_RE = re.compile(r"\btb_(\d+)\b")


def _main_lines(plan):
    """[(stmt_idx, line_idx, line)] over every main statement line."""
    for si, stmt in enumerate(plan.main_stmts):
        for li, line in enumerate(stmt.split("\n")):
            yield si, li, line


def _apply1_helpers(types):
    """Names of the apply-kind helpers (`fn h(f, x: int) { f(x) }`)."""
    return [nm for nm, (_arity, kind) in sorted(types.helpers.items())
            if kind == "apply1"]


def _unary_int_helpers(types):
    """Unary int->int helper names (kind int/match, arity 1) — the bare
    top-level fn names gen produces in fn-value positions."""
    return set(nm for nm, (arity, kind) in types.helpers.items()
               if kind in ("int", "match") and arity == 1)


def _fn_value_lets(plan, types):
    """Main-scope let names bound to a function value (lambda literal
    or bare unary int helper name) — gen's let-bound fn values."""
    unary = _unary_int_helpers(types)
    out = set()
    for _si, _li, line in _main_lines(plan):
        m = _FNLET_RE.match(line)
        if not m:
            continue
        rhs = m.group(3).strip()
        if rhs.startswith("fn(") or rhs in unary:
            out.add(m.group(2))
    return out


def _next_tb_k(plan):
    """Lowest free tb_K suffix over all program text (collision-free
    fresh names; mirrors the inl_ scheme used by T6 hoist)."""
    mx = 0
    texts = list(plan.main_stmts) + [src for _s, src in plan.fns]
    for t in texts:
        for m in _TB_NAME_RE.finditer(t):
            mx = max(mx, int(m.group(1)))
    return mx + 1


def _fresh_namer(plan):
    """Stateful fresh-name factory yielding distinct tb_K identifiers
    guaranteed absent from the program (Tier B wrappers/renames)."""
    k = _next_tb_k(plan)

    def fresh():
        nonlocal k
        name = "tb_%d" % k
        k += 1
        return name

    return fresh


def _sub_word(text, old, new):
    """Word-boundary replacement of `old` with `new`, skipping
    double-quoted string spans (gen strings carry no escapes, so quote
    parity identifies them; comments carry no quotes)."""
    pat = re.compile(r"\b%s\b" % re.escape(old))
    out = []
    pos = 0
    for m in pat.finditer(text):
        if text.count('"', 0, m.start()) % 2 == 1:
            continue               # inside a string literal
        out.append(text[pos:m.start()])
        out.append(new)
        pos = m.end()
    out.append(text[pos:])
    return "".join(out)


def _lines_applier(plan, changes):
    """Applier replacing whole main lines: changes = [(si, li, line)]."""
    def applier(plan=plan, changes=tuple(changes)):
        new = _copy_plan(plan)
        by_stmt = {}
        for si, li, ln in changes:
            by_stmt.setdefault(si, {})[li] = ln
        for si, upd in by_stmt.items():
            lines = new.main_stmts[si].split("\n")
            for li, ln in upd.items():
                lines[li] = ln
            new.main_stmts[si] = "\n".join(lines)
        return new

    return applier


def _t9_candidates(plan, types):
    """T9 alpha-rename (§5.2): a main `let` binding or a helper fn
    parameter is renamed to a fresh tb_K at EVERY reference point.

    Hygiene/scope discipline (the T6-hoist lesson, state.md #7):
      * only single-binding main lets are renamed — a shadow-chain
        member would need chain-aware reference splitting;
      * fn params are renamed strictly WITHIN their own fn body (gen
        helper bodies carry no string literals and no binder that can
        equal a param — match/cons binders are namer-fresh);
      * main renames skip string-literal spans via _sub_word."""
    cands = []
    fresh = _fresh_namer(plan)
    # (a) main lets bound exactly once
    bind_counts = {}
    for _si, _li, line in _main_lines(plan):
        m = _FNLET_RE.match(line)
        if m:
            bind_counts[m.group(2)] = bind_counts.get(m.group(2), 0) + 1
    for name, cnt in sorted(bind_counts.items()):
        if cnt != 1 or name in types.helpers:
            continue
        new_name = fresh()
        changes = [(si, li, _sub_word(line, name, new_name))
                   for si, li, line in _main_lines(plan)]
        cands.append(("T9", "let",
                      _lines_applier(plan, changes),
                      "let %s" % name, "%s -> %s" % (name, new_name),
                      None, None))
    # (b) helper fn params
    for fi, (_sig, src) in enumerate(plan.fns):
        m = re.search(r"^fn ([A-Za-z_]\w*)\(([^)]*)\)", src, re.M)
        if not m:
            continue
        for prm in m.group(2).split(","):
            prm = prm.strip()
            if prm.endswith(": int"):
                prm = prm[:-len(": int")].strip()
            if not _IDENT_RE.fullmatch(prm):
                continue
            new_name = fresh()
            new_src = _sub_word(src, prm, new_name)

            def applier(plan=plan, fi=fi, new_src=new_src):
                new = _copy_plan(plan)
                new.fns = list(plan.fns)
                sig = new.fns[fi][0]
                new.fns[fi] = (sig, new_src)
                return new

            cands.append(("T9", "param", applier,
                          "%s(%s)" % (m.group(1), prm),
                          "%s -> %s" % (prm, new_name), None, None))
    return cands


def _t10_candidates(plan, types):
    """T10 beta-reduction (§5.2): `apply_h(fn(p){ b }, e)` ->
    `b[e/p]` — the direct-call/binding-path consistency probe.

    Capture safety (先 α 防捕获): the line regex accepts only a
    brace-free lambda body, so b contains NO nested binder — a nested
    `fn(`/`match` body is rejected, and with no inner binder the
    substitution of e (whose free identifiers live in the outer scope,
    checked below) cannot capture.  Evaluation-count parity: e is
    evaluated once per reference of p in b — pure + terminating (gen
    R10), same value; refused when b references p more than once AND e
    is death-capable (gen v0 emits no / %, guard kept for synthetic
    trees)."""
    cands = []
    for si, li, line in _main_lines(plan):
        if len(plan.main_stmts[si].split("\n")) != 1:
            continue
        scope = types.scope_vars(("main", si, 0))
        for h in _apply1_helpers(types):
            pat = re.compile(r"^(\s*)print\(%s\(fn\(([A-Za-z_]\w*)\) \{ "
                             r"([^{}]*) \}, (.*)\)\);$" % re.escape(h))
            m = pat.match(line)
            if not m:
                continue
            _indent, p, b, e = m.group(1), m.group(2), m.group(3), \
                m.group(4)
            try:
                e_root = _parse_int_expr(e, types)
            except SlotReject:
                continue
            n_refs = len(re.findall(r"\b%s\b" % re.escape(p), b))
            if n_refs > 1 and has_death_risk(e_root):
                continue
            free = set(_IDENT_RE.findall(b)) - set([p])
            if not free <= scope:
                continue
            b2 = _sub_word(b, p, "(%s)" % e)
            newline = "%sprint(%s);" % (_indent, b2)
            cands.append(("T10", "reduce",
                          _lines_applier(plan, [(si, li, newline)]),
                          line.strip(), newline.strip(),
                          None, ("main", si)))
    return cands


def _t11_wrap_call_candidates(plan, types):
    """T11 first form (§5.2): `apply_h(F, e)` ->
    `apply_h((fn(G){ G })(F), e)` — identity wrapper around the
    fn-value argument (probe: call convention / stack frame).  Wrapper
    binder G carries NO annotation (Tier B ironclad)."""
    cands = []
    fresh = _fresh_namer(plan)
    unary = _unary_int_helpers(types)
    fnvars = _fn_value_lets(plan, types)
    for si, li, line in _main_lines(plan):
        if len(plan.main_stmts[si].split("\n")) != 1:
            continue
        for h in _apply1_helpers(types):
            head = "print(%s(" % h
            start = line.find(head)
            if start < 0:
                continue
            rest = line[start + len(head):]
            m = _LAMBDA_ATOM_RE.match(rest)
            if m:
                arg, tail, ok = m.group(0), rest[m.end():], True
            else:
                im = re.match(r"([A-Za-z_]\w*)", rest)
                if not im:
                    continue
                arg, tail = im.group(1), rest[im.end():]
                ok = arg in unary or arg in fnvars
            if not ok or not tail.startswith(", ") \
                    or not tail.endswith("));"):
                continue
            g = fresh()
            new_arg = "(fn(%s) { %s })(%s)" % (g, g, arg)
            newline = line[:start + len(head)] + new_arg + tail
            cands.append(("T11", "wrap-call",
                          _lines_applier(plan, [(si, li, newline)]),
                          line.strip(), newline.strip(),
                          None, ("main", si)))
    return cands


def _t11_wrap_expr_candidates(plan, types, slots):
    """T11 second form (§5.2): any int-typed subexpression e in a
    typed slot becomes `(fn(G){ G })(e)` — IN PLACE.  The wrapper is
    never hoisted across a binding boundary (the T6 hoist lesson,
    state.md #7): the redex sits exactly where e sat, so every free
    identifier of e keeps its binder.  Bool-typed (Cmp) nodes are
    skipped: the wrapper stays int-typed."""
    cands = []
    fresh = _fresh_namer(plan)
    for slot in slots:
        for path, node in _preorder_paths(slot.root):
            if isinstance(node, Cmp):
                continue
            g = fresh()
            repl = Wrap(g, copy.deepcopy(node))
            new_root = _set(slot.root, path, repl)
            cands.append((
                "T11", "wrap-expr",
                (lambda pl=plan, sl=slot, nr=new_root:
                 _splice(pl, sl, nr)),
                slot.text, render_expr(new_root), path, slot.sid))
    return cands


def _t12_candidates(plan, types):
    """T12 η-expansion / η-reduction (§5.2):
      apply position:  apply_h(F, e) <-> apply_h(fn(Z){ F(Z) }, e)
      let position:    let fv = F;  <->  let fv = fn(Z){ F(Z) };
    F ranges over unary int->int helpers and let-bound fn values (the
    only fn values gen produces; list/apply1-kind helpers excluded —
    passing them through apply_h would be ill typed and is never
    generated).  n = arity = 1 in every gen position; the n = 0 form
    `fn(){ f() }` has no implementation path here because gen emits no
    0-ary fns (§5.2 lists it only for completeness).  TCO note (§5.2): the wrapper
    body call F(Z) is itself a tail call, so deep recursion through an
    η-wrapped fn value stays stack-bounded (directed test)."""
    cands = []
    fresh = _fresh_namer(plan)
    unary = _unary_int_helpers(types)
    fnvars = _fn_value_lets(plan, types)
    eta_let_rhs = re.compile(
        r"fn\(([A-Za-z_]\w*)\) \{ ([A-Za-z_]\w*)\(\1\) \}$")
    for si, li, line in _main_lines(plan):
        single = len(plan.main_stmts[si].split("\n")) == 1
        mlet = _FNLET_RE.match(line)
        # --- let position ---
        if mlet:
            indent, fv, rhs = mlet.group(1), mlet.group(2), \
                mlet.group(3).strip()
            if rhs in unary:
                g = fresh()
                newline = ("%slet %s = fn(%s) { %s(%s) };"
                           % (indent, fv, g, rhs, g))
                cands.append(("T12", "eta",
                              _lines_applier(plan, [(si, li, newline)]),
                              line.strip(), newline.strip(),
                              None, ("main", si)))
            mr = eta_let_rhs.match(rhs)
            if mr and mr.group(2) != mr.group(1):
                newline = ("%slet %s = %s;"
                           % (indent, fv, mr.group(2)))
                cands.append(("T12", "eta-reduce",
                              _lines_applier(plan, [(si, li, newline)]),
                              line.strip(), newline.strip(),
                              None, ("main", si)))
            continue
        if not single:
            continue
        # --- apply position ---
        for h in _apply1_helpers(types):
            m = re.match(r"^(\s*)print\(%s\(([A-Za-z_]\w*), (.*)\)\);$"
                         % re.escape(h), line)
            if m and (m.group(2) in unary or m.group(2) in fnvars):
                g = fresh()
                arg = "fn(%s) { %s(%s) }" % (g, m.group(2), g)
                newline = ("%sprint(%s(%s, %s));"
                           % (m.group(1), h, arg, m.group(3)))
                cands.append(("T12", "eta",
                              _lines_applier(plan, [(si, li, newline)]),
                              line.strip(), newline.strip(),
                              None, ("main", si)))
                continue
            m2 = re.match(r"^(\s*)print\(%s\(fn\(([A-Za-z_]\w*)\) \{ "
                          r"([A-Za-z_]\w*)\(\2\) \}, (.*)\)\);$"
                          % re.escape(h), line)
            if m2 and m2.group(3) != m2.group(2):
                newline = ("%sprint(%s(%s, %s));"
                           % (m2.group(1), h, m2.group(3), m2.group(4)))
                cands.append(("T12", "eta-reduce",
                              _lines_applier(plan, [(si, li, newline)]),
                              line.strip(), newline.strip(),
                              None, ("main", si)))
    return cands


def _collect_candidates(plan, slots, types, rules):
    """[(rule, direction, applier, before, after)] over every applicable
    (rule, direction, position); expr positions whose replacement
    renders identically are dropped (no fake application / no-op
    bloat).  Applier: () -> new program tree."""
    cands = []
    rules = set(rules)
    for slot in slots:
        for path, node in _preorder_paths(slot.root):
            for rule in rules & set(_EXPR_RULES):
                if isinstance(node, Cmp) and rule != "T7":
                    continue               # bool root: no int injections
                for direction, repl in _expr_rule_dirs(rule, node, slot):
                    new_root = _set(slot.root, path, repl)
                    if render_expr(new_root) == slot.text:
                        continue           # textual no-op (e.g. a+a swap)
                    cands.append((
                        rule, direction,
                        (lambda pl=plan, sl=slot, nr=new_root:
                         _splice(pl, sl, nr)),
                        slot.text, render_expr(new_root),
                        path, slot.sid))
    if "T6" in rules:
        cands.extend(_inline_candidates(plan, types, slots))
        cands.extend(_hoist_candidates(plan, slots))
    if "T8" in rules:
        cands.extend(_t8_candidates(plan, types))
    if "T9" in rules:
        cands.extend(_t9_candidates(plan, types))
    if "T10" in rules:
        cands.extend(_t10_candidates(plan, types))
    if "T11" in rules:
        cands.extend(_t11_wrap_call_candidates(plan, types))
        cands.extend(_t11_wrap_expr_candidates(plan, types, slots))
    if "T12" in rules:
        cands.extend(_t12_candidates(plan, types))
    return cands


def apply_one(tree, rng, rules=None, exclude_slots=()):
    """Apply exactly ONE rule at one random legal position.

    Returns (new_tree, info); info is None when no rule has a legal
    position (tree returned unchanged).  `rules` restricts the rule set
    (test hook); `exclude_slots` hides slots already transformed by the
    enclosing apply_random call (anti-bloat: a slot is transformed at
    most once per apply_random)."""
    rules = tuple(RULE_NAMES) if rules is None else tuple(rules)
    types = _Types(tree)
    slots = [s for s in _find_slots(tree, types)
             if s.sid not in exclude_slots]
    cands = _collect_candidates(tree, slots, types, rules)
    if not cands:
        return tree, None
    by_rule = {}
    for c in cands:
        by_rule.setdefault(c[0], []).append(c)
    rule = list(by_rule)[prng.prng_next_range(rng, len(by_rule))]
    pool = by_rule[rule]
    by_dir = {}
    for c in pool:
        by_dir.setdefault(c[1], []).append(c)
    direction = list(by_dir)[prng.prng_next_range(rng, len(by_dir))]
    pool = by_dir[direction]
    _rule, _dir, applier, before, after, path, sid = pool[
        prng.prng_next_range(rng, len(pool))]
    new_tree = applier()
    return new_tree, {
        "rule": rule,
        "rule_name": RULE_NAMES[rule],
        "direction": direction,
        "before": before,
        "after": after,
        "path": path,
        "slot": sid,
    }


def apply_random(tree, k, prng_rng, rules=None):
    """Apply up to k transforms; each slot at most once per call.
    Returns (tree, infos); stops early when no legal position remains."""
    infos = []
    used = set()
    for _ in range(k):
        tree, info = apply_one(tree, prng_rng, rules=rules,
                               exclude_slots=used)
        if info is None:
            break
        if info.get("slot") is not None:
            used.add(info["slot"])
        infos.append(info)
    return tree, infos


def apply_rule(tree, rule, rng):
    """Targeted single-rule application (test/diagnostic hook)."""
    return apply_one(tree, rng, rules=(rule,))


def rule_sites(rule, node, slot=None):
    """[(direction, replacement)] applicable at `node` (test hook; also
    backs the death-guard unit tests on synthetic division nodes)."""
    return _expr_rule_dirs(rule, node, slot)