#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""cps.py — Tier C CPS whole-program transformer (kernel-fuzzing DELIV-11).

Task: .pge/tasks/task-cps-transformer.md; authoritative design:
docs/kernel-fuzzing-design.md §5.2「Tier C：CPS 全程序变换」.

What it does
------------
Takes ONE L1-subset TA program (as the ast-dump.ta s-expr tree — Option B
architecture: dump-AST -> sexp.py reader, per the frozen ast-nodes.txt
encoding) and transforms the WHOLE program into continuation-passing style:

  * every top-level `fn` and every `lambda` literal gains ONE extra
    trailing parameter (the continuation `k`);
  * every call to a transformed function (named top-level fn, fn-typed
    parameter, let-bound lambda, chained call) appends the current
    continuation;
  * every non-tail computation is rewritten so its result flows into an
    explicit continuation closure instead of the call stack.

The output is legal TA source (full-parens per DEC-3).  Identity target
(§5.2): `./tinyactor run` output of the transformed program is line-by-line
identical to the original, print order included.  Left-to-right operand
evaluation order is preserved (trans_args folds continuations so the
leftmost operand is evaluated outermost).

Free rider property (§5.2): after the transform ALL calls are tail calls,
so deep recursion must stay stack-bounded on tavm (OP_TAIL_CALL) — checked
by --tco (10^5 levels).

Annotation iron rule (§5.2 Tier B/C): CPS intermediate functions NEVER
carry type annotations — params and the k continuation are unannotated,
typecheck infers.  The transform therefore drops all `type-sig` forms
(their arity changed anyway) and emits parameter lists without `: type`.

Supported L1 subset (explicit whitelist — anything else is REJECTED with
an Unsupported exception naming the construct, never silently mangled):

  top level    (define (name params...) body), (type Name variants...)
               (type-sig is consumed and dropped)
  values       int literals (incl. the R1 (- 0 N) negative form), strings,
               true/false/nil, 'symbol / nullary ctor, list literal
               (cons chain), ctor call, lambda literal
  operators    + - * / %   == != < <= > >=   && || (see caveat)
  control      let (threaded (let x v body) and 2-arg statement form),
               if (else optional), match (int/string literal, ctor, cons,
               binder, _ wildcard patterns; `when` guards), begin blocks
  calls        user top-level fns (recursive ok), chained calls f(x)(y),
               lambdas via fn-typed params / let bindings, and the
               builtins print, cons, car, cdr
  caveat       && / || are only supported with PURE operands: CPS without
               a short-circuit protocol would evaluate both operands
               eagerly, changing evaluation-order/divergence semantics.
               `when` guards must be pure for the same reason.

Unsupported (explicitly rejected): receive/actor forms, float, use/bind,
const, import, external_fn, define_pub, string/other module APIs (dotted
call heads), bool-literal and quoted-symbol patterns, impure &&/||/guard
operands, let in strict expression position.

Transformer-bug isolation (task constraint: the transformer's own bugs
would fabricate positives).  The golden side-channel separates the three
parties WITHOUT trusting the transformer:

  A  = norm(run(original))          G  = norm(golden(dump(original)))
  R  = norm(run(cps(original)))     GC = norm(golden(dump(cps(original))))

  A != G   -> anchor-diverge      (pre-existing VM/golden divergence, NOT
                                   a CPS finding; outside the gate count)
  GC != G  -> transformer-mismatch (golden proves the original semantics;
                                   the CPS program means something else)
  R  != GC -> suspect-vm          (the CPS program is semantically correct
                                   per golden; tavm disagrees)
  else     -> consistent

CLI
---
    python3 tools/kernfuzz/cps.py --emit FILE        # print CPS'd source
    python3 tools/kernfuzz/cps.py --file FILE        # 4-way check, one file
    python3 tools/kernfuzz/cps.py --corpus N [--start A]   # gen seeds A..A+N-1
    python3 tools/kernfuzz/cps.py --tco              # 10^5 deep recursion
Exit code: 0 = all checks consistent (unsupported counts as an explicit
rejection, not a finding), 1 = any finding (transformer-mismatch /
suspect-vm / cps-build-fail / cps-dump-fail) or TCO failure.

Stdlib-only, host Python 3.  Requires ./tinyactor, ./tavm_asan,
tools/kernfuzz/ast-dump.ta and golden/ for the runner-backed modes (unit
tests skip gracefully without them).
"""

import argparse
import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import sexp                                     # noqa: E402
from sexp import Symbol, Pair, NIL, TRUE, FALSE  # noqa: E402

_REPO = os.path.normpath(os.path.join(_HERE, "..", ".."))

__all__ = [
    "Unsupported", "CPSTransformer", "cps_transform",
    "dump_tree", "check_program", "tco_check", "TCO_SOURCE",
]


class Unsupported(Exception):
    """The program uses a construct outside the transformer's L1 subset.

    The message names the construct (acceptance #4: explicit rejection,
    never a crash and never a silently wrong transform)."""


# ---------------------------------------------------------------------------
# binary operator table (AST symbol -> TA source operator)
# ---------------------------------------------------------------------------

_BINOPS = {
    "+": "+", "-": "-", "*": "*", "/": "/", "%": "%",
    "=": "==", "!=": "!=", "<": "<", "<=": "<=", ">": ">", ">=": ">=",
    "and": "&&", "or": "||",
}

_ARITY1 = ("car", "cdr")


def _quote_wrap(name):
    return Symbol("quote"), name


def _is_quote(node):
    return isinstance(node, Pair) and node.car == Symbol("quote") \
        and isinstance(node.cdr, Pair) and _sym(node.cdr.car) \
        and node.cdr.cdr is NIL


# ---------------------------------------------------------------------------
# small helpers over the sexp tree
# ---------------------------------------------------------------------------

def _sym(x):
    return isinstance(x, Symbol)


_FLOAT_RE = None  # compiled lazily


def _looks_like_float(name):
    """TA has no float literals, but a symbol-shaped `1.5` atom (only
    reachable via a hand-built tree) is rejected explicitly."""
    global _FLOAT_RE
    if _FLOAT_RE is None:
        import re
        _FLOAT_RE = re.compile(r"\d+\.\d+$")
    return bool(_FLOAT_RE.match(name))


def _list_items(node):
    """Proper list (Pair chain ending NIL) -> Python list; else None."""
    items = []
    while isinstance(node, Pair):
        items.append(node.car)
        node = node.cdr
    if node is NIL:
        return items
    return None


def _collect_identifiers(tree, out):
    """Reserve every identifier occurring anywhere in the tree (hygiene:
    fresh transformer names must never collide with program names — gen
    even uses `k_N` for its own lambda params)."""
    if isinstance(tree, Symbol):
        out.add(tree.name)
    elif isinstance(tree, Pair):
        _collect_identifiers(tree.car, out)
        _collect_identifiers(tree.cdr, out)


def _render_string(s):
    """TA string literal; escape set matches ast-dump.ta's escape_str()."""
    esc = {"\n": "\\n", "\r": "\\r", "\t": "\\t", "\\": "\\\\", '"': '\\"'}
    return '"%s"' % "".join(esc.get(c, c) for c in s)


def _is_ctor_head(node):
    """(quote Name) — the ctor tag that heads a ctor-call cons chain."""
    return _is_quote(node)


# ---------------------------------------------------------------------------
# output AST (the transformed program) + renderer
# ---------------------------------------------------------------------------

class ONode(object):
    __slots__ = ()


class OAtom(ONode):
    """Pre-rendered pure TA source text (single line, or a multi-line
    lambda literal).  Used for every value position."""
    __slots__ = ("text",)

    def __init__(self, text):
        self.text = text


class OCall(ONode):
    """`callee(args...)`.  callee is a str (plain name) or an ONode
    (chained call: rendered `(<callee>)(args)`).  Args are pure."""
    __slots__ = ("callee", "args")

    def __init__(self, callee, args):
        self.callee = callee
        self.args = args


class OIf(ONode):
    __slots__ = ("cond", "then", "els")     # cond is an OAtom (pure)

    def __init__(self, cond, then, els):
        self.cond = cond
        self.then = then
        self.els = els


class OLet(ONode):
    """Statement let: renders `let name = val; body` (body continues the
    block).  val is pure; body is any ONode."""
    __slots__ = ("name", "val", "body")

    def __init__(self, name, val, body):
        self.name = name
        self.val = val
        self.body = body


class OMatch(ONode):
    """arms: list of (pat_src, guard_src_or_None, body).  scrut is pure."""
    __slots__ = ("scrut", "arms")

    def __init__(self, scrut, arms):
        self.scrut = scrut
        self.arms = arms


class OLambda(ONode):
    """`fn(params...) { body }` — a VALUE (unannotated params, iron rule)."""
    __slots__ = ("params", "body")

    def __init__(self, params, body):
        self.params = params
        self.body = body


class OBegin(ONode):
    """`begin <items; one per line> <final>` — items are simple statement
    nodes (print calls / OLet), final is the block's value expression."""
    __slots__ = ("items", "final")

    def __init__(self, items, final):
        self.items = items
        self.final = final


class Renderer(object):
    """Render the output AST to full-parens TA source (DEC-3)."""

    def program(self, type_lines, fn_defs):
        parts = []
        for line in type_lines:
            parts.append(line)
            parts.append("")
        for src in fn_defs:
            parts.append(src)
            parts.append("")
        return "\n".join(parts)

    def define(self, name, params, body):
        head = "fn %s(%s) {" % (name, ", ".join(params))
        return head + "\n" + "\n".join(self._block(body, 1)) + "\n}"

    # -- statement position --------------------------------------------------

    def _block(self, node, ind):
        """Lines for a block body: flatten statement lets, final expr last."""
        pad = "  " * ind
        if isinstance(node, OLet):
            head = "%slet %s = %s;" % (pad, node.name,
                                       self._inline(node.val, ind))
            return [head] + self._block(node.body, ind)
        return [pad + self._inline(node, ind)]

    # -- expression (inline) position -----------------------------------------

    def _inline(self, node, ind):
        if isinstance(node, OAtom):
            return node.text
        if isinstance(node, OCall):
            if isinstance(node.callee, OAtom):
                callee = node.callee.text
            elif isinstance(node.callee, str):
                callee = node.callee
            else:
                callee = "(%s)" % self._inline(node.callee, ind)
            return "%s(%s)" % (callee, ", ".join(
                self._inline(a, ind) for a in node.args))
        if isinstance(node, OIf):
            pad = "  " * ind
            cond = self._inline(node.cond, ind)
            # TA grammar requires if (cond); binops already render with
            # their own parens — avoid doubling them
            if not (cond.startswith("(") and cond.endswith(")")):
                cond = "(%s)" % cond
            then_lines = self._block(node.then, ind + 1)
            els_lines = self._block(node.els, ind + 1)
            return ("if %s {\n%s\n%s} else {\n%s\n%s}"
                    % (cond,
                       "\n".join(then_lines), pad,
                       "\n".join(els_lines), pad))
        if isinstance(node, OMatch):
            pad = "  " * ind
            lines = ["match %s {" % self._inline(node.scrut, ind)]
            for pat, guard, body in node.arms:
                head = pat
                if guard is not None:
                    head += " when (%s)" % guard
                lines.append("%s  %s -> %s"
                             % (pad, head, self._inline(body, ind + 1)))
            lines.append("%s}" % pad)
            return "\n".join(lines)
        if isinstance(node, OLambda):
            pad = "  " * ind
            return ("fn(%s) {\n%s\n%s}"
                    % (", ".join(node.params),
                       "\n".join(self._block(node.body, ind + 1)), pad))
        if isinstance(node, OBegin):
            pad = "  " * ind
            lines = ["begin"]
            items = list(node.items)
            final = node.final
            # flatten statement-let chains that ended up in final position
            while isinstance(final, OLet):
                items.append(final)
                final = final.body
            for item in items:
                if isinstance(item, OLet):
                    lines.append("%s  let %s = %s;"
                                 % (pad, item.name,
                                    self._inline(item.val, ind)))
                else:
                    lines.append("%s  %s;" % (pad, self._inline(item, ind)))
            lines.append("%s  %s" % (pad, self._inline(final, ind)))
            return "\n".join(lines)
        raise TypeError("cannot render %r" % (node,))


# ---------------------------------------------------------------------------
# the transformer
# ---------------------------------------------------------------------------

class CPSTransformer(object):
    """Transform one whole program (sexp tree from ast-dump.ta) to CPS.

    The continuation argument `k` threaded through trans() is an ONode
    denoting a function VALUE: either OAtom(name) (a variable — reusable
    in several branches at no cost) or an OLambda (used exactly once, or
    hoisted into a let by the if/match cases).
    """

    def __init__(self):
        self.ctors = set()          # declared constructor names
        self.top_fns = set()        # defined top-level fn names
        self.local_fns = set()      # fn-valued locals of the current fn
        self.used = set()           # every identifier in the source
        self._counter = 0

    # -- fresh names -----------------------------------------------------------

    def fresh(self, base):
        while True:
            self._counter += 1
            name = "%s_%d" % (base, self._counter)
            if name not in self.used:
                self.used.add(name)
                return name

    # -- entry -------------------------------------------------------------------

    def transform(self, tree):
        """tree: the whole-file sexp (a proper list of top-level forms).
        Returns the transformed TA source text."""
        forms = _list_items(tree)
        if forms is None:
            raise Unsupported("top level is not a list of forms")
        type_lines = []
        defines = []
        for form in forms:
            if not isinstance(form, Pair) or not _sym(form.car):
                raise Unsupported("unrecognized top-level form")
            head = form.car.name
            if head == "type":
                type_lines.append(self._render_type(form))
            elif head == "type-sig":
                continue            # dropped: CPS fns are unannotated
            elif head == "define":
                defines.append(form)
            else:
                raise Unsupported("top-level %s" % head)
        # pass 1: collect names (call targets must be known before bodies
        # are transformed; hygiene needs all identifiers)
        for form in defines:
            header = form.cdr.car
            name = header.car if isinstance(header, Pair) else NIL
            if not _sym(name) or name is NIL:
                raise Unsupported("anonymous top-level fn (define (nil) ...)")
            self.top_fns.add(name.name)
        _collect_identifiers(tree, self.used)
        # pass 2: transform bodies
        fn_defs = []
        for form in defines:
            fn_defs.append(self._transform_define(form))
        return Renderer().program(type_lines, fn_defs)

    # -- top-level forms ------------------------------------------------------

    def _render_type(self, form):
        # (type Name params (quote Nullary) (Ctor f1 f2) ...)
        rest = _list_items(form.cdr)
        if rest is None or len(rest) < 2 or not _sym(rest[0]):
            raise Unsupported("malformed type declaration")
        tname = rest[0].name
        variants = []
        for v in rest[2:]:
            items = _list_items(v) if isinstance(v, Pair) else None
            if items is None:
                raise Unsupported("malformed variant in type %s" % tname)
            if len(items) == 2 and items[0] == Symbol("quote") \
                    and _sym(items[1]):
                variants.append(items[1].name)   # (quote Nullary)
                self.ctors.add(items[1].name)
            elif _sym(items[0]):
                vname = items[0].name              # (Ctor f1 f2)
                fields = []
                for f in items[1:]:
                    if not _sym(f):
                        raise Unsupported(
                            "annotated ctor field in type %s" % tname)
                    fields.append(f.name)
                variants.append("%s(%s)" % (vname, ", ".join(fields)))
                self.ctors.add(vname)
            else:
                raise Unsupported("malformed variant in type %s" % tname)
        return "type %s { %s }" % (tname, "; ".join(variants))

    def _transform_define(self, form):
        header, body = form.cdr.car, form.cdr.cdr.car
        name = header.car.name
        params = _list_items(header.cdr) or []
        pnames = []
        for p in params:
            if not _sym(p):
                raise Unsupported("non-symbol parameter in fn %s" % name)
            pnames.append(p.name)
        kp = self.fresh("k")
        # unannotated params are fn-value candidates (gen's apply1 shape:
        # `fn apply1(f, x: int) { f(x) }` — the dump strips annotations)
        self.local_fns = set(pnames)
        if name == "main":
            # entry point: stays 0-arity; its body runs under a terminal
            # continuation that discards the final value (both tavm and
            # golden invoke main() with no arguments)
            d = self.fresh("d")
            body_node = self.trans(body, OLambda([d], OAtom("nil")))
            return Renderer().define(name, pnames, body_node)
        body_node = self.trans(body, OAtom(kp))
        return Renderer().define(name, pnames + [kp], body_node)

    # -- pattern rendering --------------------------------------------------------

    def _render_pattern(self, p):
        if isinstance(p, int):
            return str(p)
        if isinstance(p, str):
            return _render_string(p)
        if p is NIL:
            return "nil"
        if p is TRUE or p is FALSE:
            raise Unsupported("bool literal pattern (R6 divergence class)")
        if _sym(p):
            if p.name in self.ctors:
                return p.name
            if p.name == "_":
                return "_"
            return p.name               # binder
        items = _list_items(p) if isinstance(p, Pair) else None
        if items is None:
            raise Unsupported("pattern %s" % sexp.sexp_write(p))
        # (quote C): nullary ctor pattern — the dump yields the 2-item
        # list (quote C)
        if len(items) == 2 and items[0] == Symbol("quote") \
                and _sym(items[1]):
            if items[1].name in self.ctors:
                return items[1].name
            raise Unsupported("quoted-symbol pattern '%s" % items[1].name)
        # (Ctor b1 ...) ctor sub-pattern (binders / nested patterns)
        if _sym(items[0]) and items[0].name in self.ctors:
            subs = [self._render_pattern(s) for s in items[1:]]
            return "%s(%s)" % (items[0].name, ", ".join(subs))
        # (cons h t) list pattern
        if items[0] == Symbol("cons") and len(items) == 3:
            return "cons(%s, %s)" % (self._render_pattern(items[1]),
                                     self._render_pattern(items[2]))
        raise Unsupported("pattern %s" % sexp.sexp_write(p))

    # -- purity -----------------------------------------------------------------

    def pure(self, e):
        """True iff e can stay in place as a pure expression (no user or
        builtin CALL, no print).  Lambda literals are values: pure.
        if/match/let are pure when all their parts are."""
        if isinstance(e, (int, str)) or e is NIL or e is TRUE or e is FALSE:
            return True
        if _sym(e):
            return not _looks_like_float(e.name)
        items = _list_items(e) if isinstance(e, Pair) else None
        if items is None:
            return False                # dotted pair value: not in subset
        head = items[0]
        if not _sym(head):
            return False
        name = head.name
        if name == "quote":
            return len(items) == 2 and _sym(items[1])
        if name in ("and", "or"):
            return len(items) == 3 and all(self.pure(x) for x in items[1:])
        if name in _BINOPS:
            return len(items) == 3 and all(self.pure(x) for x in items[1:])
        if name in _ARITY1:
            return len(items) == 2 and self.pure(items[1])
        if name == "cons":
            return len(items) == 3 and all(self.pure(x) for x in items[1:])
        if name == "if":
            rest = items[1:]
            if len(rest) == 2:
                rest = rest + [NIL]
            return all(self.pure(x) for x in rest)
        if name == "match":
            scrut, arms = items[1], items[2:]
            if not self.pure(scrut) or not arms:
                return False
            for arm in arms:
                a = _list_items(arm)
                if a is None or len(a) < 2 or not self.pure(a[-1]):
                    return False
                if len(a) == 3 and not self.pure(a[1]):
                    return False
            return True
        if name == "lambda":
            return True
        if name == "let":
            # threaded (let x v body): pure iff both parts are
            return len(items) == 4 and self.pure(items[2]) \
                and self.pure(items[3])
        return False                    # calls (user / print / unknown)

    # -- pure rendering ------------------------------------------------------------

    def pure_render(self, e, ind=0):
        """Render a PURE subtree to TA source; lambda literals found inside
        are transformed (they must accept the continuation parameter)."""
        if isinstance(e, int):
            return str(e)
        if isinstance(e, str):
            return _render_string(e)
        if e is NIL:
            return "nil"
        if e is TRUE:
            return "true"
        if e is FALSE:
            return "false"
        if _sym(e):
            if _looks_like_float(e.name):
                raise Unsupported("float literal %s" % e.name)
            return e.name
        items = _list_items(e)
        head = items[0]
        if head == Symbol("quote"):
            name = items[1]
            if name.name in self.ctors:
                return name.name        # nullary ctor as value
            return "'%s" % name.name    # plain quoted symbol value
        if head.name in _BINOPS:
            op = _BINOPS[head.name]
            a = self.pure_render(items[1], ind)
            b = self.pure_render(items[2], ind)
            if head.name == "-" and isinstance(items[1], int) \
                    and items[1] == 0:
                return "(0 - %s)" % b       # R1 negative literal form
            return "(%s %s %s)" % (a, op, b)
        if head.name in _ARITY1:
            return "%s(%s)" % (head.name, self.pure_render(items[1], ind))
        if head == Symbol("cons"):
            return self._render_cons(items, ind)
        if head == Symbol("if"):
            els = items[3] if len(items) == 4 else NIL
            return "if (%s) { %s } else { %s }" % (
                self.pure_render(items[1], ind),
                self.pure_render(items[2], ind + 1),
                self.pure_render(els, ind + 1))
        if head == Symbol("match"):
            return self._render_pure_match(items, ind)
        if head == Symbol("lambda"):
            return Renderer()._inline(self._trans_lambda(e), ind)
        if head == Symbol("let"):
            raise Unsupported("let in strict expression position")
        raise Unsupported("pure render of %s" % sexp.sexp_write(e))

    def _render_cons(self, items, ind):
        # ctor call: (cons (quote Name) arg-chain)
        first = items[1]
        if _is_ctor_head(first) and first.cdr.car.name in self.ctors:
            name = first.cdr.car.name
            args = self._ctor_args(items[2])
            return "%s(%s)" % (name, ", ".join(
                self.pure_render(a, ind) for a in args))
        return "cons(%s, %s)" % (self.pure_render(first, ind),
                                 self.pure_render(items[2], ind))

    def _ctor_args(self, node):
        """Ctor arg chain: (cons (quote C) (cons a1 (cons a2 nil)))
        -> [a1, a2]."""
        args = []
        chain = node
        while True:
            items = _list_items(chain)
            if items is None or len(items) != 3 \
                    or items[0] != Symbol("cons"):
                raise Unsupported("malformed constructor call")
            args.append(items[1])
            if items[2] is NIL:
                return args
            chain = items[2]

    def _render_pure_match(self, items, ind):
        pad = "  " * ind
        lines = ["match %s {" % self.pure_render(items[1], ind)]
        for arm in items[2:]:
            a = _list_items(arm)
            pat = self._render_pattern(a[0])
            head = pat
            if len(a) == 3:
                head += " when (%s)" % self.pure_render(a[1], ind)
            lines.append("%s  %s -> %s"
                         % (pad, head, self.pure_render(a[-1], ind + 1)))
        lines.append("%s}" % pad)
        return "\n".join(lines)

    # -- lambda transform ---------------------------------------------------------

    def _trans_lambda(self, e):
        # (lambda (params...) body ret-type)
        params = _list_items(e.cdr.car) or []
        pnames = []
        for p in params:
            if not _sym(p):
                raise Unsupported("non-symbol lambda parameter")
            pnames.append(p.name)
        kp = self.fresh("k")
        body = self.trans(e.cdr.cdr.car, OAtom(kp))
        return OLambda(pnames + [kp], body)

    # -- the core: trans(e, k) -----------------------------------------------------

    def trans(self, e, k):
        """Transform expression e so that its value flows into continuation
        k (an ONode denoting a function value)."""
        # atoms (literals, variables, (quote S))
        if isinstance(e, (int, str)) or e is NIL or e is TRUE or e is FALSE \
                or _sym(e):
            if _sym(e) and _looks_like_float(e.name):
                raise Unsupported("float literal %s" % e.name)
            return OCall(k, [OAtom(self.pure_render(e))])
        if isinstance(e, Pair) and e.car == Symbol("quote"):
            return OCall(k, [OAtom(self.pure_render(e))])
        items = _list_items(e)
        if items is None:
            raise Unsupported("dotted-pair expression")
        head = items[0]

        if not _sym(head):
            # chained call ((f a) b ...): bind the callee, then apply
            return self._trans_chained(items, k)
        name = head.name

        if name == "lambda":
            return OCall(k, [self._trans_lambda(e)])
        if name == "let":
            return self._trans_let(items, k)
        if name == "if":
            return self._trans_if(items, k)
        if name == "match":
            return self._trans_match(items, k)
        if name == "begin":
            return self._trans_begin(items, k)
        if name == "print":
            if len(items) != 2:
                raise Unsupported("print with %d args" % (len(items) - 1))
            arg = items[1]
            if self.pure(arg):
                return OCall(k, [OCall("print",
                                       [OAtom(self.pure_render(arg))])])
            v = self.fresh("v")
            return self.trans(arg, OLambda([v], OCall(k, [
                OCall("print", [OAtom(v)])])))
        if name in _BINOPS or name == "cons" or name in _ARITY1:
            if self.pure(e):
                return OCall(k, [OAtom(self.pure_render(e))])
            return self._trans_ctor(items, k)
        if name in self.top_fns or name in self.local_fns:
            # CPS call: the callee expects the continuation as last argument
            # (top-level fns are all transformed; local fn values are let-
            # bound lambdas / bare top-level names / unannotated params)
            return self._trans_args(items[1:],
                                    lambda vs: OCall(name, vs + [k]), k)
        raise Unsupported("call to %s" % name)

    # -- call argument threading (left-to-right evaluation preserved) -----

    def _trans_args(self, args, build, k):
        if not args:
            return build([])
        a, rest = args[0], args[1:]
        if self.pure(a):
            return self._trans_args(
                rest, lambda vs: build([OAtom(self.pure_render(a))] + vs), k)
        p = self.fresh("v")
        return self.trans(a, OLambda([p], self._trans_args(
            rest, lambda vs: build([OAtom(p)] + vs), k)))

    def _trans_chained(self, items, k):
        # ((f a) b ...): callee expression evaluated first, then applied
        h = self.fresh("h")
        callee = items[0]

        def build(vs):
            return OCall(OAtom(h), vs + [k])
        return self.trans(callee, OLambda([h], self._trans_args(
            items[1:], build, k)))

    # -- value operators / constructors with impure parts -----------------

    def _trans_ctor(self, items, k):
        head = items[0]
        n = len(items) - 1
        if head.name in _ARITY1:
            if n != 1:
                raise Unsupported("%s with %d args" % (head.name, n))
        elif n != 2:
            raise Unsupported("%s with %d operands" % (head.name, n))

        def build(vs):
            parts = [_atom_text(v) for v in vs]
            if head.name in _ARITY1:
                text = "%s(%s)" % (head.name, parts[0])
            elif head == Symbol("cons"):
                if _is_ctor_head(items[1]) \
                        and items[1].cdr.car.name in self.ctors:
                    text = "%s(%s)" % (items[1].cdr.car.name,
                                       ", ".join(parts))
                else:
                    text = "cons(%s, %s)" % (parts[0], parts[1])
            else:
                op = _BINOPS[head.name]
                if head.name == "-" and isinstance(items[1], int) \
                        and items[1] == 0:
                    text = "(0 - %s)" % parts[1]    # R1 negative form
                else:
                    text = "(%s %s %s)" % (parts[0], op, parts[1])
            return OCall(k, [OAtom(text)])
        return self._trans_args(items[1:], build, k)

    # -- control forms -----------------------------------------------------------

    def _register_let(self, x, v):
        """Track let-bound function values (lambda literal or a bare
        top-level fn name) so calls to the binder become CPS calls."""
        if isinstance(v, Pair) and v.car == Symbol("lambda"):
            self.local_fns.add(x)
        elif _sym(v) and v.name in self.top_fns:
            self.local_fns.add(x)

    def _trans_let(self, items, k):
        # (let x v) 2-arg form == threaded let with nil body
        x, v = items[1], items[2]
        body = items[3] if len(items) == 4 else NIL
        if not _sym(x):
            raise Unsupported("non-symbol let binder")
        self._register_let(x.name, v)
        if self.pure(v):
            return OLet(x.name, OAtom(self.pure_render(v)),
                        self.trans(body, k))
        t = self.fresh("v")
        return self.trans(v, OLambda([t], OLet(
            x.name, OAtom(t), self.trans(body, k))))

    def _trans_if(self, items, k):
        cond, then = items[1], items[2]
        els = items[3] if len(items) == 4 else NIL
        k2, kname = self._bind_k(k)
        if self.pure(cond):
            node = OIf(OAtom(self.pure_render(cond)),
                       self.trans(then, k2), self.trans(els, k2))
        else:
            c = self.fresh("c")
            node = self.trans(cond, OLambda([c], OIf(
                OAtom(c), self.trans(then, k2), self.trans(els, k2))))
        if kname is not None:
            node = OLet(kname, k, node)
        return node

    def _trans_match(self, items, k):
        scrut, arm_nodes = items[1], items[2:]
        if not arm_nodes:
            raise Unsupported("match with no arms")
        k2, kname = self._bind_k(k)
        arms = []
        for arm in arm_nodes:
            a = _list_items(arm)
            if a is None or len(a) < 2:
                raise Unsupported("malformed match arm")
            pat = self._render_pattern(a[0])
            guard = None
            if len(a) == 3:
                if not self.pure(a[1]):
                    raise Unsupported("impure match guard")
                guard = self.pure_render(a[1])
            arms.append((pat, guard, self.trans(a[-1], k2)))
        if self.pure(scrut):
            node = OMatch(OAtom(self.pure_render(scrut)), arms)
        else:
            s = self.fresh("s")
            node = self.trans(scrut, OLambda([s], OMatch(OAtom(s), arms)))
        if kname is not None:
            node = OLet(kname, k, node)
        return node

    def _bind_k(self, k):
        """k used in MORE THAN ONE place (if/match branches): a variable k
        is used in place; a closure literal is bound to a fresh let name
        (no code duplication).  Returns (k2, let_name_or_None)."""
        if isinstance(k, OAtom):
            return k, None
        name = self.fresh("kk")
        return OAtom(name), name

    def _trans_begin(self, items, k):
        stmts = items[1:]
        if not stmts:
            return OCall(k, [OAtom("nil")])
        # simple prefix: pure prints / pure 2-arg lets stay as statements
        # (pre-register any let-bound fn values first: folding is right-to-
        # left, so a call site may be transformed before its binder)
        for s in stmts:
            its = _list_items(s) if isinstance(s, Pair) else None
            if its and its[0] == Symbol("let") and len(its) == 3 \
                    and _sym(its[1]):
                self._register_let(its[1].name, its[2])
        pre = []
        i = 0
        while i < len(stmts) - 1:
            s = stmts[i]
            its = _list_items(s) if isinstance(s, Pair) else None
            if its and its[0] == Symbol("print") and len(its) == 2 \
                    and self.pure(its[1]):
                pre.append(OCall("print",
                                 [OAtom(self.pure_render(its[1]))]))
                i += 1
            elif its and its[0] == Symbol("let") and len(its) == 3 \
                    and _sym(its[1]) and self.pure(its[2]):
                pre.append(OLet(its[1].name,
                                OAtom(self.pure_render(its[2])),
                                OAtom("nil")))
                i += 1
            else:
                break
        acc = self.trans(stmts[-1], k)
        for s in reversed(stmts[i:len(stmts) - 1]):
            acc = self._trans_stmt(s, acc)
        if not pre:
            return acc
        return OBegin(pre, acc)

    def _trans_stmt(self, s, acc):
        """One non-simple statement s followed by the remaining computation
        acc (an expression).  Returns the combined expression."""
        its = _list_items(s)
        done = OLambda([self.fresh("d")], acc)
        if its and its[0] == Symbol("print") and len(its) == 2:
            if self.pure(its[1]):
                return OCall(done, [OCall("print", [
                    OAtom(self.pure_render(its[1]))])])
            v = self.fresh("v")
            return self.trans(its[1], OLambda([v], OCall(done, [
                OCall("print", [OAtom(v)])])))
        if its and its[0] == Symbol("let") and len(its) == 3 \
                and _sym(its[1]):
            self._register_let(its[1].name, its[2])
            if self.pure(its[2]):
                return OLet(its[1].name, OAtom(self.pure_render(its[2])),
                            acc)
            t = self.fresh("v")
            return self.trans(its[2], OLambda([t], OLet(
                its[1].name, OAtom(t), acc)))
        # if / match / begin / threaded let / anything else:
        # general continuation
        return self.trans(s, done)


def _atom_text(v):
    if isinstance(v, OAtom):
        return v.text
    if isinstance(v, str):
        return v
    raise TypeError("expected atom value, got %r" % (v,))


# ---------------------------------------------------------------------------
# public API
# ---------------------------------------------------------------------------

def cps_transform(tree):
    """Transform one whole program (sexp tree from ast-dump.ta) -> TA
    source text.  Raises Unsupported for constructs outside the subset."""
    return CPSTransformer().transform(tree)


def dump_tree(runner, src_path):
    """ast-dump.ta on src_path (via morph.Runner) -> parsed sexp tree.
    Raises RuntimeError with the runner output on failure."""
    rr = runner.dump(src_path)
    if rr.timed_out:
        raise RuntimeError("ast-dump timed out")
    if rr.rc != 0:
        raise RuntimeError("ast-dump failed: %s"
                           % (rr.out + rr.err).decode("latin-1", "replace"))
    text = rr.out.decode("latin-1").strip()
    return sexp.sexp_read_string(text)


def check_program(runner, src_text, tag):
    """Full 4-way identity check for one program (see module docstring).

    Returns (verdict, detail).  verdict is one of:
        consistent | anchor-diverge | transformer-mismatch | suspect-vm |
        cps-build-fail | cps-dump-fail | golden-fail | unsupported |
        orig-build-fail | orig-hang
    detail: dict with per-side outputs / exception text (for findings);
    includes "cps_src" whenever the transform itself succeeded.
    """
    import morph

    d = {}
    orig_rr, paths, bp = runner.build_and_run(src_text, tag)
    d["orig_src_path"] = paths[0]
    if bp.rc != 0:
        return "orig-build-fail", {"build_err": (
            bp.out + bp.err).decode("latin-1", "replace")}
    if morph.classify_run(orig_rr) == "hang":
        return "orig-hang", {}
    a_lines = morph.norm_tavm(orig_rr.out, orig_rr.rc)
    d["a_lines"] = a_lines

    # golden on the original
    try:
        tree = dump_tree(runner, paths[0])
    except RuntimeError as ex:
        return "golden-fail", {"error": str(ex)}
    # the transform itself (before the golden anchors, so subset-exit
    # constructs report "unsupported" even when golden cannot evaluate
    # the original)
    try:
        cps_src = cps_transform(tree)
    except Unsupported as ex:
        return "unsupported", {"error": str(ex)}
    except Exception as ex:                     # transformer bug: finding
        return "cps-transform-crash", {
            "error": "%s: %s" % (type(ex).__name__, ex)}
    d["cps_src"] = cps_src

    gpath = os.path.join(runner.workdir, "dump_%s.sexp" % tag)
    with open(gpath, "wb") as f:
        f.write(sexp.sexp_write(tree).encode("latin-1"))
    gres = runner.golden_eval(gpath)
    if gres.rc != 0:
        return "golden-fail", {"error": (gres.out + gres.err).decode(
            "latin-1", "replace")}
    g_lines = morph.norm_golden(gres.out)
    d["g_lines"] = g_lines

    cps_rr, cps_paths, cbp = runner.build_and_run(cps_src, tag + "_cps")
    d["cps_src_path"] = cps_paths[0]
    if cbp.rc != 0:
        return "cps-build-fail", {"build_err": (
            cbp.out + cbp.err).decode("latin-1", "replace")}
    if morph.classify_run(cps_rr) == "hang":
        d["r_lines"] = "<timeout>"
        return "suspect-vm", d
    r_lines = morph.norm_tavm(cps_rr.out, cps_rr.rc)
    d["r_lines"] = r_lines

    # golden on the CPS program (the isolation anchor)
    try:
        cps_tree = dump_tree(runner, cps_paths[0])
    except RuntimeError as ex:
        return "cps-dump-fail", {"error": str(ex)}
    cpath = os.path.join(runner.workdir, "dump_%s_cps.sexp" % tag)
    with open(cpath, "wb") as f:
        f.write(sexp.sexp_write(cps_tree).encode("latin-1"))
    cgres = runner.golden_eval(cpath)
    if cgres.rc != 0:
        return "cps-dump-fail", {"error": (cgres.out + cgres.err).decode(
            "latin-1", "replace")}
    gc_lines = morph.norm_golden(cgres.out)
    d["gc_lines"] = gc_lines

    if a_lines != g_lines:
        return "anchor-diverge", d
    if gc_lines != g_lines:
        return "transformer-mismatch", d
    if r_lines != gc_lines:
        return "suspect-vm", d
    return "consistent", d


# ---------------------------------------------------------------------------
# TCO rider (§5.2): deep recursion must stay stack-bounded after CPS
# ---------------------------------------------------------------------------

TCO_SOURCE = """\
// TCO rider case for the CPS transformer (tools/kernfuzz/cps.py --tco)
fn down(n: int) -> int {
  if (n <= 0) { 0 } else { down((n - 1)) }
}
fn main() {
  print(down(100000));
  print("tco-ok")
}
"""


def tco_check(runner, levels=100000, timeout=60.0):
    """Transform TCO_SOURCE (at `levels` recursion depth), run BOTH the
    original and the CPS program; both must finish within timeout with
    identical output.  Returns (ok, detail)."""
    import morph

    src = TCO_SOURCE.replace("down(100000)", "down(%d)" % levels)
    t0 = time.time()
    orig_rr, paths, bp = runner.build_and_run(src, "tco_orig")
    if bp.rc != 0:
        return False, "orig build failed: %s" % (
            bp.out + bp.err).decode("latin-1", "replace")
    tree = dump_tree(runner, paths[0])
    cps_src = cps_transform(tree)
    cps_rr, _cp, cbp = runner.build_and_run(cps_src, "tco_cps")
    elapsed = time.time() - t0
    if cbp.rc != 0:
        return False, "cps build failed: %s" % (
            cbp.out + cbp.err).decode("latin-1", "replace")
    if cps_rr.timed_out:
        return False, "cps program timed out (stack not bounded?)"
    a = morph.norm_tavm(orig_rr.out, orig_rr.rc)
    r = morph.norm_tavm(cps_rr.out, cps_rr.rc)
    if a != r:
        return False, "output mismatch: orig=%r cps=%r" % (a, r)
    return True, "levels=%d elapsed=%.1fs out=%r" % (levels, elapsed, a)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _write_finding(findings_dir, verdict, tag, src_text, detail):
    os.makedirs(findings_dir, exist_ok=True)
    base = "%s-%s" % (verdict, tag)
    meta = {"verdict": verdict, "tag": tag,
            "detail": {k: v for k, v in detail.items()
                       if isinstance(v, (str, list)) and k != "cps_src"}}
    with open(os.path.join(findings_dir, base + ".json"), "w",
              encoding="latin-1") as f:
        json.dump(meta, f, indent=1)
    if src_text is not None:
        with open(os.path.join(findings_dir, base + ".orig.ta"), "wb") as f:
            f.write(src_text.encode("latin-1"))
    cps_src = detail.get("cps_src")
    if cps_src is not None:
        with open(os.path.join(findings_dir, base + ".cps.ta"), "wb") as f:
            f.write(cps_src.encode("latin-1"))


def tempdir():
    import tempfile
    return tempfile.mkdtemp(prefix="kernfuzz-cps-")


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Tier C CPS whole-program transformer (DELIV-11)")
    ap.add_argument("--emit", metavar="FILE",
                    help="transform FILE and print the CPS source")
    ap.add_argument("--file", metavar="FILE",
                    help="4-way identity check on one file")
    ap.add_argument("--corpus", type=int, metavar="N",
                    help="check N gen programs (seeds --start..--start+N-1)")
    ap.add_argument("--start", type=int, default=0,
                    help="first gen seed for --corpus (default 0)")
    ap.add_argument("--max-depth", type=int, default=4,
                    help="gen max expression depth (default 4)")
    ap.add_argument("--tco", action="store_true",
                    help="deep-recursion stack-boundedness check (10^5)")
    ap.add_argument("--tco-levels", type=int, default=100000)
    ap.add_argument("--timeout", type=float, default=10.0,
                    help="per-run timeout in seconds (default 10)")
    ap.add_argument("--findings-dir", default=os.path.join(
        _REPO, ".pge", "findings", "cps"))
    args = ap.parse_args(argv)

    modes = sum(1 for m in (args.emit, args.file, args.corpus, args.tco)
                if m)
    if modes != 1:
        ap.error("exactly one of --emit / --file / --corpus / --tco")

    import morph
    runner = morph.Runner(tempdir(), timeout=args.timeout)

    if args.emit:
        try:
            tree = dump_tree(runner, args.emit)
            sys.stdout.write(cps_transform(tree))
        except Unsupported as ex:
            sys.stderr.write("unsupported: %s\n" % ex)
            return 1
        return 0

    if args.tco:
        ok, detail = tco_check(runner, args.tco_levels, args.timeout * 6)
        print("TCO %s: %s" % ("OK" if ok else "FAIL", detail))
        return 0 if ok else 1

    if args.file:
        with open(args.file, "rb") as f:
            src_text = f.read().decode("latin-1")
        verdict, detail = check_program(runner, src_text, "file")
        line = verdict
        if verdict == "unsupported":
            line += " (%s)" % detail.get("error", "")
        print(line)
        if verdict in ("transformer-mismatch", "suspect-vm",
                       "cps-build-fail", "cps-dump-fail",
                       "cps-transform-crash"):
            _write_finding(args.findings_dir, verdict, "file",
                           src_text, detail)
            return 1
        return 0 if verdict == "consistent" else 1

    # --corpus N
    import gen
    counts = {}
    n_findings = 0
    for i in range(args.corpus):
        seed = i + args.start
        src_text = gen.gen_program(seed, args.max_depth)
        verdict, detail = check_program(runner, src_text, "p%d" % seed)
        counts[verdict] = counts.get(verdict, 0) + 1
        line = "seed %d: %s" % (seed, verdict)
        if verdict == "unsupported":
            line += " (%s)" % detail.get("error", "")
        print(line)
        if verdict in ("transformer-mismatch", "suspect-vm",
                       "cps-build-fail", "cps-dump-fail",
                       "cps-transform-crash"):
            n_findings += 1
            _write_finding(args.findings_dir, verdict, "p%d" % seed,
                           src_text, detail)
    consistent = counts.get("consistent", 0)
    judged = sum(v for k, v in counts.items()
                 if k not in ("unsupported", "anchor-diverge",
                              "orig-build-fail", "golden-fail", "orig-hang"))
    infra = sum(v for k, v in counts.items()
                if k in ("orig-build-fail", "golden-fail", "orig-hang"))
    print("== corpus: %d programs, consistent %d/%d judged "
          "(unsupported %d, anchor-diverge %d, infra-fails %d), "
          "findings %d"
          % (args.corpus, consistent, judged,
             counts.get("unsupported", 0),
             counts.get("anchor-diverge", 0), infra, n_findings))
    return 1 if n_findings else 0


if __name__ == "__main__":
    sys.exit(main())