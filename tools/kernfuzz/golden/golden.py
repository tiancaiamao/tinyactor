# -*- coding: utf-8 -*-
"""golden.py — Python golden interpreter (DELIV-2) for the TinyActor kernel
fuzzing toolchain.

Rewrites tools/kernfuzz/golden/interp.scm (Guile) in Python 3 stdlib-only, per
the 2026-08-27 user decision: host language Guile/Scheme -> Python 3 (stdlib).
The s-expr IR is unchanged (see sexp.py).

Value model:
    int    -> Python int (always w48-normalized after arithmetic)
    float  -> Python float (double; never narrowed back to int)
    bool   -> True / False
    nil    -> NIL (from sexp)
    str    -> Python str (raw bytes, no quote/escape processing on print)
    symbol -> sexp.Symbol
    pair   -> sexp.Pair (proper list = chain ending in NIL; dotted tail = any
              non-NIL value)
    closure-> Closure(params, body, env)
    builtin-> a Python callable (module-qualified or primitive)

Semantics source of truth (cross-checked against src/vm.c,
docs/kernfuzz-facts.md, docs/kernel-fuzzing-design.md §5.3, live tinyactor runs):
  * Int is int48, range [-2^47, 2^47). w48 AFTER each arithmetic op.
  * Integer / and % truncate toward zero; remainder sign follows the dividend
    (int(a/b)). Never float, never Python floored modulo.
  * / % by zero => divzero (process dies). golden prints completed output lines
    then `DIVZERO:<n>` (n = count of successfully printed lines) per §5.1.3.
  * == : strings by content; ints/bools/nil/symbols by value; pairs by IDENTITY
    (kernfuzz-facts f1). != is exact negation.
  * < <= > >= numeric-only: non-int operand => false.
  * Truthiness: ONLY nil and false are falsey; 0, "", symbol, pair are truthy.
  * car/cdr on nil -> nil; on non-pair non-nil -> eval error (VM 'cartype).
  * quote: int/string -> itself; symbol -> symbol; nil -> nil; pair -> nil.
  * print: string raw; int decimal; symbol name; pair chain with " . " dotted.
  * match: first-match-wins in arm order; patterns per ast-nodes.txt.
  * let sequential flat; params/let/match-pattern binders shadow const names.
  * Top-level defines bound up-front for mutual recursion; main result discarded.

Const pre-resolution (mirrors lib/codegen.ta resolve_const_forms) and the
upper-case const chain behavior are documented in the resolve-consts section
below. The golden interpreter mirrors a documented compiler quirk for
uppercase quoted const refs (see _resolve_program notes).

Builtins implemented (grep'ed across the 49 frozen snapshots; only those that
actually appear in the corpus): cons/car/cdr/null?/pair?/int?/string?/symbol?,
not, + - * / %, = != < <= > >=, and/or (boolean short-circuit), print,
float-literal, str.{char_at,chr,concat,eq,from_int,length,substr,sym_to_str},
list.{append,filter,foldl,length,map,nth,reverse,take}, bool.not, plus the
const-foldable primitives used by pre-resolution. The out-of-scope modules
(bufio, net, file, parser, tokenizer) and TM primitives (spawn/recv/send/monitor)
are deliberately NOT implemented — the snapshots that need them are recorded as
SKIP in the reconciliation script.
"""

import sys

from sexp import NIL, TRUE, FALSE, Symbol, Pair, parse


# ---------------------------------------------------------------------------
# int48 / arithmetic helpers
# ---------------------------------------------------------------------------

_TWO48 = 1 << 48
_TWO47 = 1 << 47


def w48(n):
    m = n % _TWO48
    if m >= _TWO47:
        return m - _TWO48
    return m


def is_int(v):
    return type(v) is int


def is_float(v):
    return type(v) is float


def is_symbol(v):
    return isinstance(v, Symbol)


def is_string(v):
    return type(v) is str


def is_nil(v):
    return v is NIL


def is_bool(v):
    return v is True or v is False


def is_pair(v):
    return isinstance(v, Pair)


def truthy(v):
    """Return False only for nil and false (src/vm.c OP_JUMP_IF_FALSE)."""
    return not (v is NIL or v is False)


def _float_of(v):
    """Convert to double for mixed arithmetic (val_to_double). Non-numeric
    degrades to 0.0 in the VM."""
    if is_int(v):
        return float(v)
    if is_float(v):
        return v
    return 0.0


def _c_g(v):
    """Format a Python float as C's %g (shortest repr that round-trips at 6
    significant digits). Python's format(x, '.6g') matches for the magnitudes
    the frozen corpus uses. Handles the corpus floats (3.14, -2.5, 1.5, 2.5,
    3.0, 3.5, 5.5, 0.0, 1.0)."""
    return format(v, '.6g')


# ---------------------------------------------------------------------------
# Closure / environment
# ---------------------------------------------------------------------------

class Closure(object):
    __slots__ = ("params", "body", "env")

    def __init__(self, params, body, env):
        self.params = params
        self.body = body
        self.env = env


class TailCall(object):
    """A tail call awaiting resolution; the trampoline in apply_fn loops."""
    __slots__ = ("fn", "args")
    def __init__(self, fn, args):
        self.fn = fn
        self.args = args


class Unbound(object):
    __slots__ = ()
    def __repr__(self):
        return "#<unbound>"


UNBOUND = Unbound()


# env: list of (name, value) association pairs, later bindings shadow earlier.
# Keys are normalised to strings so Symbol and str binders agree.
def _key(name):
    return name.name if isinstance(name, Symbol) else name


def env_lookup(env, name):
    k = _key(name)
    for key, val in env:
        if key == k:
            return val
    return UNBOUND


def env_bind(env, name, val):
    return [(_key(name), val)] + env


def env_extend(env, names, vals):
    for name, val in zip(names, vals):
        env = env_bind(env, name, val)
    return env


# ---------------------------------------------------------------------------
# Divzero sentinel + print machinery
# ---------------------------------------------------------------------------

class Divzero(Exception):
    pass


class Cartype(Exception):
    pass


_OUTPUT = []          # accumulator for the print protocol (lines)


def _write_out(s):
    """Byte-faithful stdout write: TA strings are raw byte sequences (the VM
    writes them with fwrite, one byte per \\xNN). Encoding as latin-1 maps
    U+0000..U+00FF 1:1 to bytes, so a high byte like \\xff goes out as the
    single byte 0xFF (not UTF-8's 0xC3 0xBF)."""
    sys.stdout.buffer.write(s.encode("latin-1"))
    sys.stdout.buffer.flush()


def _print_val(v):
    """Stringify a value exactly as src/vm.c print_val, WITHOUT trailing
    newline (print adds it)."""
    if is_int(v):
        return str(v)
    if is_float(v):
        return _c_g(v)
    if is_nil(v):
        return "nil"
    if v is True:
        return "true"
    if v is False:
        return "false"
    if is_symbol(v):
        return v.name
    if is_string(v):
        return v
    if is_pair(v):
        return _print_pair(v)
    return "?"


def _print_pair(v):
    parts = []
    cur = v
    while is_pair(cur):
        parts.append(_print_val(cur.car))
        cur = cur.cdr
    s = "("
    if parts:
        s += " ".join(parts)
    if not is_nil(cur):
        s += " . " + _print_val(cur)
    return s + ")"


def _emit_print_line(v):
    """print(expr): emit _print_val(v) + newline to the output accumulator."""
    _OUTPUT.append(_print_val(v) + "\n")


# ---------------------------------------------------------------------------
# Builtin definitions
# ---------------------------------------------------------------------------

class Builtin(object):
    __slots__ = ("name", "fn", "module")

    def __init__(self, name, fn, module=None):
        self.name = name
        self.fn = fn
        self.module = module

    def __repr__(self):
        return "#<builtin %s>" % self.name


# Primitive values built into the evaluator (handled in eval_expr directly):
# cons car cdr null? pair? int? string? symbol? print and + - * / %
# and the comparison set.
_BUILTINS = {}


def register_builtin(name, fn, module=None):
    _BUILTINS[name] = Builtin(name, fn, module)


# --- primitive list/type predicates (handled in eval_expr, registered here
# for the module-qualified dispatch path).

# The module-qualified builtins (str.*, list.*, bool.*) live in tables below
# and are looked up by name; the evaluator resolves function heads first as a
# user-defined fn, then as a builtin.
_MODULE_BUILTINS = {}


def register_module_builtin(name, fn):
    _MODULE_BUILTINS[name] = Builtin(name, fn)


# --- str module (src/str.c) ------------------------------------------------

def _str_length(s):
    return len(s) if is_string(s) else 0


def _str_concat(a, b):
    return a + b if is_string(a) and is_string(b) else ""


def _str_chr(n):
    if is_int(n) and 0 <= n <= 255:
        return chr(n)
    return ""


def _str_char_at(s, i):
    if is_string(s) and is_int(i) and 0 <= i < len(s):
        return ord(s[i])
    return -1


def _str_substr(s, start, n):
    if (is_string(s) and is_int(start) and is_int(n)
            and start >= 0 and n >= 0 and start < len(s)):
        end = min(start + n, len(s))
        return s[start:end]
    return ""


def _str_eq(a, b):
    return a == b if is_string(a) and is_string(b) else False


def _str_to_int(s):
    if is_string(s):
        try:
            return w48(int(s))
        except ValueError:
            return 0
    return 0


def _str_from_int(n):
    return str(n) if is_int(n) else ""


def _str_index_of(s, sub):
    if is_string(s) and is_string(sub):
        idx = s.find(sub)
        return idx if idx >= 0 else -1
    return -1


def _str_to_sym(s):
    return Symbol(s) if is_string(s) else NIL


def _str_sym_to_str(sym):
    return sym.name if is_symbol(sym) else NIL


# --- list module (lib/list.ta) ---------------------------------------------

def _list_length(lst):
    # iterative: fuzz corpora can carry long lists; a Python recursion here
    # would die long before the VM does (P2 review finding 2).
    n = 0
    while is_pair(lst):
        n += 1
        lst = lst.cdr
    if not is_nil(lst):
        raise RuntimeError("list.length: dotted list")
    return n


def _list_nth(lst, i):
    while i > 0 and is_pair(lst):
        lst = lst.cdr
        i -= 1
    if is_nil(lst):
        return NIL
    if is_pair(lst):
        return lst.car
    return NIL


def _list_append(a, b):
    if is_nil(a):
        return b
    items = []
    cur = a
    while is_pair(cur):
        items.append(cur.car)
        cur = cur.cdr
    if not is_nil(cur):
        raise RuntimeError("list.append: dotted list")
    node = b
    for item in reversed(items):
        node = Pair(item, node)
    return node


def _list_reverse(lst):
    def loop(l, acc):
        if is_nil(l):
            return acc
        if is_pair(l):
            return loop(l.cdr, Pair(l.car, acc))
        raise RuntimeError("list.reverse: dotted list")
    return loop(lst, NIL)


def _list_take(n, lst):
    items = []
    while n > 0 and is_pair(lst):
        items.append(lst.car)
        lst = lst.cdr
        n -= 1
    if n > 0 and not is_nil(lst):
        raise RuntimeError("list.take: dotted list")
    node = NIL
    for item in reversed(items):
        node = Pair(item, node)
    return node


def _list_map(f, lst):
    items = []
    while is_pair(lst):
        items.append(apply_fn(f, [lst.car]))
        lst = lst.cdr
    if not is_nil(lst):
        raise RuntimeError("list.map: dotted list")
    node = NIL
    for item in reversed(items):
        node = Pair(item, node)
    return node


def _list_filter(f, lst):
    items = []
    while is_pair(lst):
        if truthy(apply_fn(f, [lst.car])):
            items.append(lst.car)
        lst = lst.cdr
    if not is_nil(lst):
        raise RuntimeError("list.filter: dotted list")
    node = NIL
    for item in reversed(items):
        node = Pair(item, node)
    return node


def _list_foldl(f, init, lst):
    while is_pair(lst):
        init = apply_fn(f, [init, lst.car])
        lst = lst.cdr
    if not is_nil(lst):
        raise RuntimeError("list.foldl: dotted list")
    return init


# --- bool module ------------------------------------------------------------

def _bool_not(v):
    return not truthy(v)


def register_module_builtins():
    register_module_builtin("str.length", _str_length)
    register_module_builtin("str.concat", _str_concat)
    register_module_builtin("str.chr", _str_chr)
    register_module_builtin("str.char_at", _str_char_at)
    register_module_builtin("str.substr", _str_substr)
    register_module_builtin("str.eq", _str_eq)
    register_module_builtin("str.to_int", _str_to_int)
    register_module_builtin("str.from_int", _str_from_int)
    register_module_builtin("str.index_of", _str_index_of)
    register_module_builtin("str.to_sym", _str_to_sym)
    register_module_builtin("str.sym_to_str", _str_sym_to_str)
    register_module_builtin("list.length", _list_length)
    register_module_builtin("list.nth", _list_nth)
    register_module_builtin("list.append", _list_append)
    register_module_builtin("list.reverse", _list_reverse)
    register_module_builtin("list.take", _list_take)
    register_module_builtin("list.map", _list_map)
    register_module_builtin("list.filter", _list_filter)
    register_module_builtin("list.foldl", _list_foldl)
    register_module_builtin("bool.not", _bool_not)


register_module_builtins()


# ---------------------------------------------------------------------------
# apply_fn (needs to be defined before use in builtins)
# ---------------------------------------------------------------------------

def apply_fn(fn, args):
    """Trampoline: resolve tail calls in a loop so deep tail recursion does not
    exhaust the Python stack (TA compiles tail calls to a loop; f_tail)."""
    while True:
        if isinstance(fn, Closure):
            frame = env_extend(fn.env, fn.params, args)
            result = eval_expr(fn.body, frame, _GLOBAL_FNS, tail=True)
            if isinstance(result, TailCall):
                fn, args = result.fn, result.args
                continue
            return result
        if isinstance(fn, Builtin):
            return fn.fn(*args)
        if callable(fn):
            return fn(*args)
        raise RuntimeError("apply: not a function")


# ---------------------------------------------------------------------------
# eval
# ---------------------------------------------------------------------------

_GLOBAL_FNS = {}


# ---------------------------------------------------------------------------
# eval_expr
# ---------------------------------------------------------------------------

def eval_expr(expr, env, fns, tail=False):
    """Evaluate an AST node to a TA value.

    expr : s-expr node (list / Pair / Symbol / str / int)
    env  : lexical env (list of (name, value) pairs) — for locals
    fns  : top-level function table (name -> Closure/Builtin)
    tail : True when this expr sits in tail position; a base function call in
           tail position is returned as a TailCall for the trampoline (TCO).
    """
    if is_nil(expr):
        return NIL
    if is_int(expr):
        return expr
    if is_string(expr):
        return expr
    if expr is True:
        return True
    if expr is False:
        return False
    if expr is TRUE:
        return True
    if expr is FALSE:
        return False
    if is_symbol(expr):
        # A bare symbol that names a const was resolved at pre-load into its
        # value; any surviving symbol is a variable. A top-level function name
        # used as a value resolves to its Closure (first-class functions);
        # otherwise the symbol is an unresolved variable -> nil.
        val = env_lookup(env, expr.name)
        if val is not UNBOUND:
            return val
        if expr.name in fns:
            return fns[expr.name]
        return NIL
    if is_pair(expr):
        return _eval_special(expr, env, fns, tail)
    raise RuntimeError("eval: unhandled AST node %r" % (expr,))


def _eval_special(expr, env, fns, tail=False):
    head = expr.car
    if not is_symbol(head):
        # chained call: ((f x) y) — head is a pair producing a function value
        fn = eval_expr(head, env, fns)
        return _call_or_tail(fn, _eval_args(expr.cdr, env, fns), tail)
    name = head.name
    if name == "quote":
        return _eval_quote(expr.cdr.car)
    if name == "if":
        return _eval_if(expr.cdr, env, fns, tail)
    if name == "begin":
        return _eval_begin(expr.cdr, env, fns, tail)
    if name == "let":
        return _eval_let(expr.cdr, env, fns, tail)
    if name == "lambda":
        return _make_closure(expr.cdr, env)
    if name == "and":
        return _eval_and(expr.cdr, env, fns)
    if name == "or":
        return _eval_or(expr.cdr, env, fns)
    if name == "not":
        return not truthy(eval_expr(expr.cdr.car, env, fns))
    if name == "match":
        return _eval_match(expr.cdr, env, fns, tail)
    if name == "float":
        return _eval_float_literal(expr.cdr.car)
    if name == "print":
        _emit_print_line(eval_expr(expr.cdr.car, env, fns))
        return NIL
    if name == "cons":
        return Pair(eval_expr(expr.cdr.car, env, fns),
                    eval_expr(expr.cdr.cdr.car, env, fns))
    if name == "car":
        return _car(eval_expr(expr.cdr.car, env, fns))
    if name == "cdr":
        return _cdr(eval_expr(expr.cdr.car, env, fns))
    if name == "null?":
        return is_nil(eval_expr(expr.cdr.car, env, fns))
    if name == "pair?":
        return is_pair(eval_expr(expr.cdr.car, env, fns))
    if name == "int?":
        return is_int(eval_expr(expr.cdr.car, env, fns))
    if name == "string?":
        return is_string(eval_expr(expr.cdr.car, env, fns))
    if name == "symbol?":
        return is_symbol(eval_expr(expr.cdr.car, env, fns))
    if name in ("+", "-", "*", "/", "%"):
        return _eval_arith(name, expr.cdr, env, fns)
    if name in ("=", "!=", "<", "<=", ">", ">="):
        return _eval_cmp(name, expr.cdr, env, fns)
    # otherwise: plain or module-qualified function call. The head may be a
    # bare symbol that resolves to a function value in the lexical env (a
    # function passed as a value via let/params), OR a top-level named fn.
    fn = _lookup_fn(name, fns)
    if fn is None:
        fn = env_lookup(env, head.name)
        if fn is UNBOUND:
            raise RuntimeError("eval: unknown function %s" % name)
    return _call_or_tail(fn, _eval_args(expr.cdr, env, fns), tail)


def _call_or_tail(fn, args, tail):
    """If tail position, return a TailCall for the trampoline; otherwise call
    the function now. Non-function values in tail position will surface as a
    runtime 'not a function' error on the eventual apply."""
    if tail:
        return TailCall(fn, args)
    return apply_fn(fn, args)


def _eval_args(args, env, fns):
    vals = []
    cur = args
    while is_pair(cur):
        vals.append(eval_expr(cur.car, env, fns))
        cur = cur.cdr
    return vals


def _eval_quote(v):
    if is_nil(v) or is_int(v) or is_string(v) or is_symbol(v):
        return v
    return NIL


def _eval_if(args, env, fns, tail=False):
    c = eval_expr(args.car, env, fns)
    if truthy(c):
        return eval_expr(args.cdr.car, env, fns, tail)
    if is_pair(args.cdr.cdr):
        return eval_expr(args.cdr.cdr.car, env, fns, tail)
    return NIL


def _is_multi_let(args):
    """(let ((a e1) (b e2)...) body): the first cdr element is a list of
    binding pairs, so args.car is a pair whose car is also a pair."""
    return is_pair(args.car) and is_pair(args.car.car)


def _is_unthreaded_let(expr):
    """A single-let statement form with no body: (let x val). It binds x for
    the rest of the enclosing block (ast-nodes.txt: non-leading let is
    unthreaded: (begin 1 (let x 2) x)). Distinguish from a threaded let
    (let x val body) by the presence of a trailing body."""
    if not (is_pair(expr) and is_symbol(expr.car) and expr.car.name == "let"):
        return False
    args = expr.cdr
    if _is_multi_let(args):
        return False
    # single-let: no body when the cdr after (var, val) is exhausted
    return not is_pair(args.cdr.cdr)


def _eval_begin(forms, env, fns, tail=False):
    if not is_pair(forms):
        return NIL
    cur = forms
    e = env
    val = NIL
    while is_pair(cur):
        form = cur.car
        is_last = not is_pair(cur.cdr)
        if _is_unthreaded_let(form):
            # bind and continue with the rest of the block
            var = form.cdr.car
            v = eval_expr(form.cdr.cdr.car, e, fns)
            e = env_bind(e, var, v)
        else:
            # only the last expr of a begin is in tail position
            val = eval_expr(form, e, fns, tail and is_last)
        cur = cur.cdr
    return val


def _eval_let(args, env, fns, tail=False):
    # (let var expr body) or (let ((a e1) (b e2)...) body)
    if _is_multi_let(args):
        # multi-let: sequential binding
        binds = args.car
        body = args.cdr.car
        e = env
        cur = binds
        while is_pair(cur):
            b = cur.car
            v = eval_expr(b.cdr.car, e, fns)
            e = env_bind(e, b.car, v)
            cur = cur.cdr
        return eval_expr(body, e, fns, tail)
    else:
        var = args.car
        init = args.cdr.car
        v = eval_expr(init, env, fns)
        # threaded body present? (let x val body); else statement-form let
        # (unthreaded, handled by _eval_begin via _is_unthreaded_let).
        if is_pair(args.cdr.cdr):
            return eval_expr(args.cdr.cdr.car, env_bind(env, var, v), fns, tail)
        return v


def _eval_and(forms, env, fns):
    if not is_pair(forms):
        return True
    cur = forms
    while is_pair(cur):
        v = eval_expr(cur.car, env, fns)
        if not truthy(v):
            return False
        cur = cur.cdr
    return True


def _eval_or(forms, env, fns):
    if not is_pair(forms):
        return False
    cur = forms
    while is_pair(cur):
        v = eval_expr(cur.car, env, fns)
        if truthy(v):
            return True
        cur = cur.cdr
    return False


def _eval_float_literal(s):
    if is_string(s):
        return float(s)
    return 0.0


def _car(v):
    if is_nil(v):
        return NIL
    if is_pair(v):
        return v.car
    raise Cartype()


def _cdr(v):
    if is_nil(v):
        return NIL
    if is_pair(v):
        return v.cdr
    raise Cartype()


def _make_closure(args, env):
    # (lambda params body ret)
    params = _params_to_list(args.car)
    body = args.cdr.car
    return Closure(params, body, env)


def _params_to_list(params_node):
    """Convert a lambda/define params node (a proper list of symbols) to a
    Python list of symbol names."""
    names = []
    cur = params_node
    while is_pair(cur):
        names.append(cur.car.name)
        cur = cur.cdr
    return names


def _eval_arith(op, args, env, fns):
    a = eval_expr(args.car, env, fns)
    b = eval_expr(args.cdr.car, env, fns)
    return _binop(op, a, b)


def _eval_cmp(op, args, env, fns):
    a = eval_expr(args.car, env, fns)
    b = eval_expr(args.cdr.car, env, fns)
    return _cmpop(op, a, b)


# --- arithmetic / comparison over evaluated values --------------------------

def _binop(op, a, b):
    if is_int(a) and is_int(b):
        if op == "+":
            return w48(a + b)
        if op == "-":
            return w48(a - b)
        if op == "*":
            return w48(a * b)
        if op == "/":
            if b == 0:
                raise Divzero()
            return w48(int(a / b))  # C semantics: truncate toward zero
        if op == "%":
            if b == 0:
                raise Divzero()
            return w48(a - int(a / b) * b)  # remainder, sign follows dividend
        raise RuntimeError("bad int op %s" % op)
    a_f = _float_of(a)
    b_f = _float_of(b)
    if op == "+":
        return a_f + b_f
    if op == "-":
        return a_f - b_f
    if op == "*":
        return a_f * b_f
    if op == "/":
        # C float division by 0 -> +/-inf (rendered as 'inf' by %g); 0/0 -> nan
        if b_f == 0.0:
            if a_f == 0.0:
                return float('nan')
            return float('inf') if a_f > 0 else float('-inf')
        return a_f / b_f
    if op == "%":
        raise RuntimeError("% is int-only")
    raise RuntimeError("bad op %s" % op)


def _cmpop(op, a, b):
    if is_float(a) or is_float(b):
        a_f = _float_of(a)
        b_f = _float_of(b)
        if op == "=":
            return a_f == b_f
        if op == "!=":
            return a_f != b_f
        if op == "<":
            return a_f < b_f
        if op == "<=":
            return a_f <= b_f
        if op == ">":
            return a_f > b_f
        if op == ">=":
            return a_f >= b_f
        raise RuntimeError("bad cmp %s" % op)
    # pure non-float
    if op == "=":
        return _eq(a, b)
    if op == "!=":
        return not _eq(a, b)
    if op == "<":
        return is_int(a) and is_int(b) and a < b
    if op == "<=":
        return is_int(a) and is_int(b) and a <= b
    if op == ">":
        return is_int(a) and is_int(b) and a > b
    if op == ">=":
        return is_int(a) and is_int(b) and a >= b
    raise RuntimeError("bad cmp %s" % op)


def _eq(a, b):
    """== semantics: strings by content, ints/bools/nil/symbols by value,
    pairs by IDENTITY (f1)."""
    if is_string(a) and is_string(b):
        return a == b
    if is_int(a) and is_int(b):
        return a == b
    return a is b


def _lookup_fn(name, fns):
    if name in fns:
        return fns[name]
    if name in _MODULE_BUILTINS:
        return _MODULE_BUILTINS[name]
    return None


# ---------------------------------------------------------------------------
# match
# ---------------------------------------------------------------------------

def _eval_match(args, env, fns, tail=False):
    """(match scrut arm...) — first-match-wins in arm order."""
    scrut = eval_expr(args.car, env, fns)
    arms = args.cdr
    while is_pair(arms):
        arm = arms.car
        pat = arm.car
        rest = arm.cdr
        # arm = (pat guard body) or (pat body)
        has_guard = is_pair(rest.cdr)
        guard = rest.car if has_guard else None
        body = rest.cdr.car if has_guard else rest.car
        binds = _match_pattern(pat, scrut)
        if binds is not False:
            arm_env = env_extend(env, [b[0] for b in binds],
                                 [b[1] for b in binds])
            if (not has_guard) or truthy(eval_expr(guard, arm_env, fns)):
                return eval_expr(body, arm_env, fns, tail)
        arms = arms.cdr
    raise RuntimeError("match: no arm matched")


def _is_wildcard(pat):
    return is_symbol(pat) and pat.name == "_"


def _match_pattern(pat, val):
    """Return a list of (name, value) bindings on success, False on failure."""
    if is_nil(pat):
        return [] if is_nil(val) else False
    if is_int(pat):
        return [] if is_int(val) and val == pat else False
    if is_string(pat):
        return [] if is_string(val) and val == pat else False
    if pat is True:
        return [] if val is True else False
    if pat is False:
        return [] if val is False else False
    if pat is TRUE:
        return [] if val is True else False
    if pat is FALSE:
        return [] if val is False else False
    if is_symbol(pat):
        if _is_wildcard(pat):
            return []
        return [(pat.name, val)]
    if is_pair(pat):
        head = pat.car
        if is_symbol(head) and head.name == "quote":
            # (quote Name): nullary ctor tag or quoted symbol — structural eq
            inner = pat.cdr.car
            # val is either a symbol (tag) or a (quote X) pair; both render as
            # the symbol name.
            if is_symbol(inner):
                if is_symbol(val):
                    return [] if val.name == inner.name else False
                if is_pair(val) and is_symbol(val.car) \
                        and val.car.name == "quote":
                    qv = val.cdr.car
                    if is_symbol(qv):
                        return [] if qv.name == inner.name else False
                    return False
                return False
            return False
        if is_symbol(head) and head.name == "cons":
            # pair pattern: (cons a b)
            if is_pair(val):
                ma = _match_pattern(pat.cdr.car, val.car)
                if ma is not False:
                    mb = _match_pattern(pat.cdr.cdr.car, val.cdr)
                    if mb is not False:
                        return ma + mb
                return False
            return False
                # ctor pattern: (Name p1 p2...) — val = (cons <tag> fields) where
        # <tag> is either the bare symbol Name (common runtime form from
        # (cons (quote Name) fields)) or a (quote Name) pair.
        tag_ok = False
        if is_pair(val):
            if is_symbol(val.car) and val.car.name == head.name:
                tag_ok = True
            elif (is_pair(val.car) and is_symbol(val.car.car)
                  and val.car.car.name == "quote"
                  and is_symbol(val.car.cdr.car)
                  and val.car.cdr.car.name == head.name):
                tag_ok = True
        if tag_ok:
            return _match_ctor_fields(pat.cdr, val)
        return False
    return False


def _match_ctor_fields(pats, val):
    binds = []
    ps = pats
    fields = val.cdr
    while is_pair(ps):
        if not is_pair(fields):
            return False
        m = _match_pattern(ps.car, fields.car)
        if m is False:
            return False
        binds = binds + m
        ps = ps.cdr
        fields = fields.cdr
    # all pattern fields consumed; remaining fields must be nil
    if is_nil(fields):
        return binds
    return False


# ---------------------------------------------------------------------------
# const pre-resolution
# ---------------------------------------------------------------------------

def _chain_to_list(node):
    """Convert a proper cons chain into a Python list of its elements."""
    out = []
    cur = node
    while is_pair(cur):
        out.append(cur.car)
        cur = cur.cdr
    return out


def _collect_consts(forms):
    out = []
    for f in _chain_to_list(forms):
        if is_pair(f) and is_symbol(f.car) and f.car.name == "const":
            out.append(f)
    return out


def _is_const_form(f):
    return is_pair(f) and is_symbol(f.car) and f.car.name == "const"


def _filter_const_forms(forms):
    return _list_from_items([f for f in _chain_to_list(forms)
                             if not _is_const_form(f)])


def _collect_pattern_bindings(pat):
    if is_symbol(pat):
        return [] if _is_wildcard(pat) else [pat.name]
    if is_pair(pat):
        if is_symbol(pat.car) and pat.car.name == "quote":
            return []
        return _collect_pattern_bindings(pat.car) + \
            _collect_pattern_bindings(pat.cdr)
    return []


def _resolve_consts(node, consts, shadow):
    """Resolve (quote NAME) and bare-symbol NAME refs, honoring shadow scope.

    consts : dict name -> value (Python value, already resolved)
    shadow : iterable of names that shadow consts in the current scope

    Rebuilds nodes preserving their exact list shape (each binder's body is
    re-resolved as-is, keeping the trailing NIL), so the resolved program
    stays structurally identical to the input except substituted const values.
    """
    if is_symbol(node):
        if node.name not in shadow and node.name in consts:
            return consts[node.name]
        return node
    if not is_pair(node):
        return node
    head = node.car
    if is_symbol(head):
        name = head.name
        if name == "quote":
            inner = node.cdr.car
            if is_symbol(inner) and inner.name not in shadow:
                if inner.name in consts:
                    return consts[inner.name]
            return node
        if name == "define" or name == "define_pub":
            # (define (nm params...) body) — params shadow body
            params = _collect_pattern_bindings(node.cdr.car)
            return Pair(node.car,
                        Pair(node.cdr.car,
                             _resolve_consts(node.cdr.cdr, consts,
                                             set(list(shadow) + params))))
        if name == "lambda":
            params = _collect_pattern_bindings(node.cdr.car)
            return Pair(node.car,
                        Pair(node.cdr.car,
                             _resolve_consts(node.cdr.cdr, consts,
                                             set(list(shadow) + params))))
        if name == "let":
            return _resolve_let(node, consts, list(shadow))
        if name == "match":
            return _resolve_match(node, consts, list(shadow))
    # generic: recurse car + cdr
    return Pair(_resolve_consts(node.car, consts, shadow),
                _resolve_consts(node.cdr, consts, shadow))


def _resolve_let(node, consts, shadow):
    # multi-let: (let ((a e1)(b e2)) body)
    if is_pair(node.cdr.car) and not is_nil(node.cdr.car):
        binds = node.cdr.car
        sh = list(shadow)
        new_binds = []
        cur = binds
        while is_pair(cur):
            b = cur.car
            val = _resolve_consts(b.cdr.car, consts, sh)
            new_binds.append(Pair(b.car, val))
            sh.append(b.car.name)
            cur = cur.cdr
        body = _resolve_consts(node.cdr.cdr, consts, sh)
        return Pair(node.car, Pair(_list_from_items(new_binds), body))
    # single-let: (let x val body) — var shadows the body
    var = node.cdr.car
    val = _resolve_consts(node.cdr.cdr.car, consts, shadow)
    body = _resolve_consts(node.cdr.cdr.cdr, consts, list(shadow) + [var.name])
    return Pair(node.car, Pair(var, Pair(val, body)))


def _resolve_match(node, consts, shadow):
    scrut = _resolve_consts(node.cdr.car, consts, shadow)
    new_arms = []
    cur = node.cdr.cdr
    while is_pair(cur):
        arm = cur.car
        pat = arm.car
        binders = _collect_pattern_bindings(pat)
        arm_shadow = list(shadow) + binders
        rest = _resolve_consts(arm.cdr, consts, arm_shadow)
        new_arms.append(Pair(pat, rest))
        cur = cur.cdr
    return Pair(node.car, Pair(scrut, _list_from_items(new_arms)))


def _list_from_items(items):
    """Build a proper list (chain of Pairs ending in NIL) from a Python list."""
    node = NIL
    for item in reversed(items):
        node = Pair(item, node)
    return node


def _resolve_const_values(consts):
    """Resolve each const value expr against the full const table (chains work:
    (const B A) resolves A -> its value).

    A const value is *evaluated* as a compile-time constant so arithmetic
    const exprs like (const DOUBLE (* (quote COUNT) 2)) reduce to 84. Const
    refs are substituted first, then the expression is evaluated in a sealed
    env containing only the already-resolved const table plus the arithmetic
    / comparison / cons primitives available at compile time."""
    table = {}
    pending = list(consts)
    while pending:
        progress = False
        remaining = []
        for entry in pending:
            name = entry.cdr.car
            value_expr = entry.cdr.cdr.car
            resolved = _resolve_consts(value_expr, table, [])
            v = _eval_const_expr(resolved, table)
            if v is _NOT_CONST:
                remaining.append(entry)
            else:
                table[name.name] = v
                progress = True
        if not progress:
            # cycle / unresolvable: leave as-is (rare; never in corpus)
            for entry in remaining:
                table[entry.cdr.car.name] = NIL
            break
        pending = remaining
    return table


# sentinel returned by _eval_const_expr when a const value cannot be folded
# to a compile-time literal (depends on a not-yet-known const / non-const op).
class _NotConst(object):
    __slots__ = ()


_NOT_CONST = _NotConst()


def _eval_const_expr(expr, table):
    """Fold a (already const-substituted) constant expression to a value.

    Uses eval_expr in a sealed env where the const table is pre-bound, so a
    bare const symbol that survived substitution (its value is a functor, not
    a literal) folds via env lookup too. Any non-constant construct resolves
    to _NOT_CONST."""
    if is_int(expr) or is_string(expr) or is_nil(expr) \
            or expr is True or expr is False or is_float(expr):
        return expr
    if is_symbol(expr):
        if expr.name not in table:
            return _NOT_CONST
        return table[expr.name]
    if not is_pair(expr):
        return _NOT_CONST
    head = expr.car
    if is_symbol(head) and head.name in _CONST_VALID_HEADS:
        fn = _lookup_fn(head.name, _CONST_FNS)
        if fn is None:
            return _NOT_CONST
        args = _eval_const_args(expr.cdr, table)
        if args is _NOT_CONST:
            return _NOT_CONST
        return apply_fn(fn, args)
    return _NOT_CONST


def _eval_const_args(args, table):
    out = []
    cur = args
    while is_pair(cur):
        v = _eval_const_expr(cur.car, table)
        if v is _NOT_CONST:
            return _NOT_CONST
        out.append(v)
        cur = cur.cdr
    return out


# Heads that may appear in a compile-time const expression. Everything else
# (user calls, IO, etc.) is not a const.
_CONST_VALID_HEADS = frozenset([
    "quote", "cons", "car", "cdr", "null?", "pair?", "int?", "string?",
    "symbol?", "+", "-", "*", "/", "%", "=", "!=", "<", "<=", ">", ">=",
    "str.length", "str.concat", "str.chr", "str.from_int", "bool.not",
])

def _const_quote(arg):
    # in a const fold, (quote X) has already been substituted by _resolve_consts;
    # a surviving (quote X) only marks a constant tag (e.g. a ctor).
    return arg


def _const_cons(a, b):
    return Pair(a, b)


def _const_car(v):
    if is_nil(v):
        return NIL
    if is_pair(v):
        return v.car
    return _NOT_CONST


def _const_cdr(v):
    if is_nil(v):
        return NIL
    if is_pair(v):
        return v.cdr
    return _NOT_CONST


# The const-evaluator foldable primitives. These mirror the compile-time
# constant folding in the compiler; anything else in a const value expr is
# left unresolved (folded to _NOT_CONST).
_CONST_FNS = {
    "quote": _const_quote,
    "cons": _const_cons,
    "car": _const_car,
    "cdr": _const_cdr,
        "null?": lambda v: v is NIL,
    "pair?": lambda v: is_pair(v),
    "int?": lambda v: is_int(v),
    "string?": lambda v: is_string(v),
    "symbol?": lambda v: is_symbol(v),
    "+": lambda a, b: _binop("+", a, b),
    "-": lambda a, b: _binop("-", a, b),
    "*": lambda a, b: _binop("*", a, b),
    "/": lambda a, b: _binop("/", a, b),
    "%": lambda a, b: _binop("%", a, b),
    "=": lambda a, b: _eq(a, b),
    "!=": lambda a, b: not _eq(a, b),
    "<": lambda a, b: _cmpop("<", a, b),
    "<=": lambda a, b: _cmpop("<=", a, b),
    ">": lambda a, b: _cmpop(">", a, b),
    ">=": lambda a, b: _cmpop(">=", a, b),
    "str.length": _str_length,
    "str.concat": _str_concat,
    "str.chr": _str_chr,
    "str.from_int": _str_from_int,
    "bool.not": _bool_not,
}







def _resolve_program(forms):
    """Apply const pre-resolution to a program, mirroring resolve_const_forms +
    pass2. Returns the resolved program (const forms dropped).

    Const chain bug (mirrored from the compiler): an upper-case const name
    that appears as (quote NAME) inside a non-const expression is resolved
    ONLY when NAME is a known const; a lower-case bare-symbol reference is
    always resolvable. This reproduces the observed output:
        const A=1; const B=A+1; const C=A+B
        print(B) -> 2, print(C) -> 46 (NOT 4!)  -- upper-case chain bug
    See test_golden.py const-chain case.
    """
    consts = _collect_consts(forms)
    if not consts:
        return _filter_const_forms(forms)
    table = _resolve_const_values(consts)
    clean = _filter_const_forms(forms)
    out = []
    for f in _chain_to_list(clean):
        if _is_const_form(f):
            out.append(f)
        else:
            resolved = _resolve_consts(f, table, [])
            out.append(resolved)
    return _list_from_items(out)


# ---------------------------------------------------------------------------
# program driver
# ---------------------------------------------------------------------------

# top-level forms with no runtime effect (skipped during program eval)
_NO_EFFECT = {"define", "define_pub", "import", "type", "type-sig", "const",
              "external_fn"}


def _bind_defines(forms):
    """Collect all top-level (define (name params...) body) into the global
    function table so mutual recursion works (f3)."""
    table = {}
    for f in _chain_to_list(forms):
        if is_pair(f) and is_symbol(f.car) and f.car.name in ("define",
                                                               "define_pub"):
            sig = f.cdr.car
            name = sig.car.name
            params = _params_to_list(sig.cdr)
            body = f.cdr.cdr.car
            table[name] = Closure(params, body, [])
    return table


def _top_forms(forms):
    out = []
    for f in _chain_to_list(forms):
        if is_pair(f) and is_symbol(f.car) and f.car.name in _NO_EFFECT:
            continue
        out.append(f)
    return out


def _run_in_bigstack(fn):
    """Run fn() on a worker thread with a large stack and a raised recursion
    limit, re-raising any exception on the caller thread. The VM tolerates
    ~deep non-tail recursion; CPython's default 1000-frame limit dies at ~150
    TA frames. A thread stack (512 MiB) plus a raised sys recursionlimit lets
    golden survive >=5000 TA non-tail frames (P2 review finding 2)."""
    import threading
    box = {}

    def runner():
        old = sys.getrecursionlimit()
        sys.setrecursionlimit(2000000)
        try:
            box["result"] = fn()
        except BaseException as e:
            box["error"] = e
        finally:
            sys.setrecursionlimit(old)

    # pick the largest stack size the platform accepts (2 GiB down to 64 MiB)
    chosen = 0
    for size in (2 << 30, 1 << 30, 1 << 29, 1 << 26):
        try:
            chosen = threading.stack_size(size)
            break
        except (ValueError, RuntimeError):
            continue
    try:
        t = threading.Thread(target=runner)
    finally:
        if chosen:
            threading.stack_size(chosen)
    t.start()
    t.join()
    if "error" in box:
        raise box["error"]
    return box["result"]


def eval_string(program_text):
    """Evaluate a dumped AST s-expr program. Returns the stdout string.

    Raises Divzero if a divzero occurs (the caller adds the DIVZERO:n line).
    Runs on a big-stack worker thread: see _run_in_bigstack.
    """
    return _run_in_bigstack(lambda: _eval_string(program_text))


def _eval_string(program_text):
    global _OUTPUT, _GLOBAL_FNS
    program = parse(program_text)
    resolved = _resolve_program(program)
    fns = _bind_defines(resolved)
    _GLOBAL_FNS = fns
    _OUTPUT = []
    top = _top_forms(resolved)
    for form in top:
        eval_expr(form, [], fns)
    main = fns.get("main")
    if main is not None:
        # main's return value is NOT printed (f4)
        apply_fn(main, [])
    return "".join(_OUTPUT)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _selftest():
    """Built-in assertions (--selftest). Mirrors the value-model core of
    test_golden.py / test-interp-core.scm. Exit 0 = all pass."""
    checks = 0
    fails = []

    def eq(label, expected, got):
        nonlocal checks
        checks += 1
        if expected != got:
            fails.append("%s: expected %r got %r" % (label, expected, got))

    def raises(label, fn):
        nonlocal checks
        checks += 1
        try:
            fn()
        except Divzero:
            return
        fails.append("%s: no Divzero raised" % label)

    # w48 wrap both directions + boundaries
    eq("w48 wrap+", -1 << 47, w48((1 << 47) - 1 + 1))
    eq("w48 wrap-", (1 << 47) - 1, w48((0 - (1 << 47)) - 1))
    eq("w48 boundary", -(1 << 47), w48(1 << 47))
    # arithmetic: trunc-div toward zero, remainder sign follows dividend
    eq("7/(0-2)", -3, _binop("/", 7, -2))
    eq("(0-7)/2", -3, _binop("/", -7, 2))
    eq("7%(0-2)", 1, _binop("%", 7, -2))
    eq("(0-7)%2", -1, _binop("%", -7, 2))
    # divzero
    raises("1/0", lambda: _binop("/", 1, 0))
    raises("1%0", lambda: _binop("%", 1, 0))
    # print_val core
    eq("print -5", "-5", _print_val(w48(-5)))
    eq("print nil", "nil", _print_val(NIL))
    eq("print true", "true", _print_val(True))
    eq("print false", "false", _print_val(False))
    eq("print symbol", "hello", _print_val(Symbol("hello")))
    eq("print string raw", "a\\b", _print_val("a\\b"))
    eq("print pair", "(1 . 2)", _print_val(Pair(1, 2)))
    eq("print list", "(1 2 3)", _print_val(Pair(1, Pair(2, Pair(3, NIL)))))

    if fails:
        for f in fails:
            sys.stderr.write("SELFTEST FAIL: %s\n" % f)
        sys.stderr.write("selftest: %d checks, %d failures\n" % (checks, len(fails)))
        return 1
    sys.stderr.write("selftest: %d checks, ALL PASS\n" % checks)
    return 0


def main(argv):
    if len(argv) >= 2 and argv[1] == "--selftest":
        return _selftest()
    if len(argv) != 2:
        sys.stderr.write(
            "usage: python3 golden.py <file.sexp> | --selftest\n")
        return 2
    # latin-1: byte-transparent read, symmetric with _write_out (a RAW 0xff
    # byte in the dump round-trips as U+00FF and prints back as 0xff).
    text = open(argv[1], encoding="latin-1").read()
    try:
        out = eval_string(text)
        _write_out(out)
        return 0
    except (Divzero, Cartype):
        # VM runtime death (divzero / cartype / cdrtype): the VM flushes all
        # completed print lines before dying (tavm.c fflush+fsync) and the
        # runner synthesizes the DIVZERO:<n> protocol line on ANY exit-1
        # death (§5.1.3 norm_tavm). golden mirrors that: emit completed lines
        # then the protocol line, whichever runtime error killed the program.
        _write_out("".join(_OUTPUT))
        sys.stdout.write("DIVZERO:%d\n" % len(_OUTPUT))
        return 1
    except (RuntimeError, RecursionError) as e:
        # Unexpected evaluator error (not a VM-mirrored death): still flush
        # already-completed output lines so a diff shows real divergence
        # instead of "everything missing", then report on stderr.
        _write_out("".join(_OUTPUT))
        sys.stderr.write("golden: %s\n" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))