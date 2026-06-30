# Spec: Make typecheck.ta work on real parsed AST + actor programs

## Goal
Extend `lib/typecheck.ta` so it can type-check real `.ta` programs produced by the parser, including actor operations, inline lambdas, and the desugared forms (match→let+if, receive→receive-scan).

## Background — Parser AST Format

The parser already desugars most surface syntax into a small set of core forms:

### Top-level forms
```
(define (name param1 param2 ...) body_expr1 body_expr2 ...)
(define_pub (name param1 ...) body...)
(import "module_name")
(type TypeName (quote Variant1) (quote Variant2) ((CtorName arg1 arg2) ...))
<expression>  // top-level expression (e.g. in main auto-spawn)
```

### Expressions
```
42                     → int literal
"hello"                → string literal  
true / false           → bool literal
nil                    → nil literal
'symbol                → (quote symbol) → symbol literal
x                      → variable reference (symbol)
(+ a b)                → arithmetic (also: - * / % < <= > >=)
(== a b)               → equality comparison (also: !=)
(lambda (params...) body...)  → inline lambda/closure
(let var expr body...)        → let binding
(if cond then else)           → conditional
(begin e1 e2 ...)             → sequencing
(func_name arg1 arg2 ...)     → function call
(receive-scan (lambda (msg) (if-chain)))  → receive block
(recv-commit)                 → receive matched arm action
(recv-skip)                   → receive no-match action
(str.eq a b)                  → string equality (from pattern match desugar)
(cons a b)                    → pair construction
(car p) (cdr p)               → pair access
```

### Pattern match → already desugared by parser
`match expr { pat -> body }` becomes:
```
(let temp expr (if guard1 body1 (if guard2 body2 nil)))
```
Patterns are desugared into guard expressions using `==`, `pair?`, `car`, `cdr`, `str.eq`, `null?`. So **we don't need to handle match directly** — it's already let/if/function-calls.

### Actor operations (plain function calls)
```
spawn(fn_name)         → spawn actor with function name symbol
spawn(lambda_expr)     → spawn actor with closure
send(pid, msg)         → send message
recv()                 → blocking receive (returns message)
self()                 → get current pid
monitor(pid)           → monitor a pid, returns ref
```

## What to Implement

### 1. Recognize `quote` as symbol/base type (ALREADY DONE — verify)

### 2. Add actor primitives to `make_builtin_env`

- `spawn`: `forall(a, arrow(a, pid))` — where `pid` is `(base pid)`. Add `t_pid()` helper.
- `send`: `forall(a, arrow(pid, arrow(a, nil)))` — but permissively: `forall(a, forall(b, arrow(a, arrow(b, b))))` since we don't have a `unit` type
- `recv`: `forall(a, a)` — returns any type (the message). Use `forall(a, a)` i.e. `t_forall(cons(0,nil), t_var(0))`
- `self`: `arrow(nil, pid)` — but takes no args, so just `t_pid()`. Actually in the AST it's `(self)` which is a 0-arg call. So `self` has type `arrow(unit, pid)`. Permissively: just make it a fresh var that unifies with anything, or `t_forall(cons(0,nil), t_arrow(t_var(0), t_pid()))`.
- `monitor`: `forall(a, arrow(a, ref))` — but we don't have ref type. Permissively: `forall(a, arrow(a, t_var(1)))`
- `receive-scan`: `forall(a, arrow(arrow(a, b), b))` — takes a function from msg to result. Permissively.
- `recv-commit`: `t_forall(cons(0,nil), t_var(0))` — returns any value
- `recv-skip`: `t_forall(cons(0,nil), t_var(0))` — returns any value

Add type helpers:
- `t_pid()` → `(base pid)`
- `t_unit()` → just use a fresh tvar, or `(base unit)`

### 3. Handle `type` declarations in `collect_defines` and `infer_program`

When processing top-level forms, a `(type Name variants...)` form should:
- Register each variant as a constructor function in env
- Nullary variant `(quote Red)` → `Red : forall(a, a)` or just the type `Name`
- N-ary variant `(CtorName arg1 arg2)` → `CtorName : arg1_type -> arg2_type -> Name_type`

For now, keep it permissive: register variants as fresh tvars or simple forall types.

Actually, simpler approach: just **skip** type declarations in collect_defines and infer_defines (like import). They don't need type inference. The variant constructors are used as function calls and will be handled permissively.

### 4. Handle `receive-scan` special form in `infer_compound`

Add a case for `receive-scan`:
- It's `(receive-scan (lambda (msg) body))`
- The lambda takes a message of any type and returns a result
- Infer the lambda body in extended env
- Return the body's return type

### 5. Handle `recv-commit` and `recv-skip`
These are 0-arg function calls that return fresh vars. Just let them fall through to the unknown function case (fresh var).

### 6. Add string operations to builtin env
- `str.eq`: `forall(a, arrow(a, arrow(a, bool)))` — but actually it's string-specific. Use `arrow(string, arrow(string, bool))`
- `str.concat`: `arrow(string, arrow(string, string))`
- `str.length`: `arrow(string, int)`

### 7. New `main()` with real program tests

Test with actual .ta-style AST (what the parser would produce):

**Test A: Simple arithmetic program**
```ta
// fn add(x, y) { x + y }
// fn main() { add(1, 2) }
// AST: (define (add x y) (+ x y)) (define (main) (add 1 2))
let prog_a = list(
  cons('define, cons(cons('add, cons('x, cons('y, nil))),
    cons(cons('+, cons('x, cons('y, nil))), nil))),
  cons('define, cons(cons('main, nil),
    cons(cons('add, cons(1, cons(2, nil))), nil))))
// → add: int -> int -> int
// → main: int  (or infer top-level expr returns int)
```

**Test B: Closure + curry**
```ta
// fn adder(n) { fn(x) { x + n } }
// fn main() { let g = adder(5) in g(3) }
// AST: (define (adder n) (lambda (x) (+ x n)))
let prog_b = list(
  cons('define, cons(cons('adder, cons('n, nil)),
    cons(cons('lambda, cons(cons('x, nil),
      cons(cons('+, cons('x, cons('n, nil))), nil))), nil))))
// → adder: int -> int -> int  (curried)
```

**Test C: Recursive function**
```ta
// fn fib(n) { if n <= 1 { n } else { fib(n-1) + fib(n-2) } }
// AST: (define (fib n) (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))
let prog_c = list(
  cons('define, cons(cons('fib, cons('n, nil)),
    cons(cons('if,
      cons(cons('<=, cons('n, cons(1, nil))),
      cons('n,
      cons(cons('+, 
        cons(cons('fib, cons(cons('-, cons('n, cons(1, nil))), nil)),
        cons(cons('fib, cons(cons('-, cons('n, cons(2, nil))), nil)),
        nil))), nil)))), nil))))
// → fib: int -> int
```

**Test D: Actor program (ping-pong)**
```ta
// fn ping(n, pong_pid) {
//   if n == 0 { send(pong_pid, 'stop); print("done") }
//   else { send(pong_pid, cons('ping, self())); match recv() { 'pong -> ping(n-1, pong_pid) } }
// }
// fn pong() {
//   match recv() { 'stop -> print("pong done"); cons('ping, sender) -> { send(sender, 'pong); pong() } }
// }
// fn main() { let p = spawn('pong); spawn(fn { ping(100, p) }); ... }
// AST: match desugars to let+if, receive desugars to receive-scan+lambda
let prog_d = list(
  cons('define, cons(cons('ping, cons('n, cons('pong_pid, nil))),
    cons(/* body with send, receive-scan, etc */ nil))))
// → should type-check without errors (all types resolve)
```

**Test E: Type declaration + ADT**
```ta
// type Color { Red; Green; Blue }
// fn color_name(c) { match c { Red -> "red"; Green -> "green"; Blue -> "blue" } }
// AST: (type Color (quote Red) (quote Green) (quote Blue))
//      (define (color_name c) (let temp c (if (== temp (quote Red)) "red" ...)))
let prog_e = list(
  cons('type, cons('Color, cons(cons('quote, cons('Red, nil)), 
    cons(cons('quote, cons('Green, nil)), cons(cons('quote, cons('Blue, nil)), nil))))),
  cons('define, cons(cons('color_name, cons('c, nil)),
    cons(/* desugared match body */ nil))))
// → should not crash
```

## Acceptance Criteria

### L1 — Structural
- [ ] `make -j4` succeeds — Verify: `make -j4 2>&1 | tail -1`
- [ ] `./tinyactor lib/typecheck.ta` runs and exits 0 — Verify: `./tinyactor lib/typecheck.ta; echo $?`
- [ ] New functions exist: `t_pid`, updated `make_builtin_env` — Verify: `grep -c 'fn t_pid\|fn make_builtin_env' lib/typecheck.ta` ≥ 2

### L2 — Behavioral
- [ ] Test A: `add` inferred as `int -> int -> int`
- [ ] Test B: `adder` inferred as `int -> int -> int` (curried closure)
- [ ] Test C: `fib` inferred as `int -> int` (recursive)
- [ ] Test D: Actor program type-checks without crash (send/recv/spawn/self resolved)
- [ ] Test E: Type declaration doesn't crash, color_name type-checks
- [ ] All existing tests (tests 1-7) still pass

## Constraints
- ONLY modify `lib/typecheck.ta`
- NO `||` operator (silently broken in .ta)
- NO `!` operator (use `expr == false`)
- Functions top-level only
- Use `match` for pattern matching (it's supported in the language)
- Keep builtins permissive — prefer not crashing over precision

## Out of Scope
- Exhaustiveness checking
- Type annotation validation (x: int → checking)
- Module system type checking (import module.function)
- Integration with compile.c pipeline