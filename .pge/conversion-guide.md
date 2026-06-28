# Lisp → .ta Conversion Guide

## Syntax Mapping

### Comments
```
;; comment          →  // comment
;;; comment         →  // comment
```

### Function Definition
```
(define (f x y) body...)           →  fn f(x, y) { body... }
(define (f) body...)               →  fn f() { body... }
```

### Top-level expressions
.lisp allows top-level expressions outside any function. .ta requires them inside `fn main()`:
```
(print (fib 30))                   →  fn main() { print(fib(30)) }
(loop 1000000)                     →  fn main() { loop(1000000) }
```
If there's already a `(define (main) ...)`, put top-level expressions inside it instead.

### Let bindings
```
(let x val)                        →  let x = val
```
Note: In .lisp, `(let x val)` is followed by more expressions in the same scope. In .ta, `let x = val;` is a statement.

### If/Else
```
(if cond then_expr else_expr)      →  if cond { then_expr } else { else_expr }
(if cond then_expr)                →  if cond { then_expr }
```

### Begin / Multiple expressions
```
(begin e1 e2 e3)                   →  { e1; e2; e3 }
```
In .ta, multiple statements in a block are separated by newlines (semicolons optional).

### Lambda
```
(lambda (x) body)                  →  fn(x) { body }
(lambda () body)                   →  fn { body }
```
Lambda in spawn context:
```
(spawn (lambda () body))           →  spawn(fn { body })
(spawn (lambda () (f arg)))        →  spawn(fn { f(arg) })
```

### Arithmetic / Comparison
```
(+ a b)          →  a + b
(- a b)          →  a - b
(* a b)          →  a * b
(/ a b)          →  a / b
(% a b)          →  a % b
(= a b)          →  a == b
(< a b)          →  a < b
(<= a b)         →  a <= b
(> a b)          →  a > b
(>= a b)         →  a >= b
```
IMPORTANT: Always use spaces around operators. `n-1` would be parsed as identifier `n-1` in .ta. Write `n - 1`.

### Function calls (including nested)
```
(f a b)           →  f(a, b)
(f (- n 1))       →  f(n - 1)
(+ (fib (- n 1)) (fib (- n 2)))  →  fib(n - 1) + fib(n - 2)
(print x)         →  print(x)
```

### Cons / Car / Cdr / Predicates
```
(cons a b)        →  cons(a, b)
(car x)           →  car(x)
(cdr x)           →  cdr(x)
(null? x)         →  null?(x)
(pair? x)         →  pair?(x)
(string? x)       →  string?(x)
(int? x)          →  int?(x)
```

### String operations
```
(string-length s)   →  string-length(s)
(string-concat a b) →  string-concat(a, b)
(string-slice a b c)→  string-slice(a, b, c)
(string-eq a b)     →  string-eq(a, b)
```

### Actor primitives
```
(spawn 'func)       →  spawn('func)          // zero-arg function
(spawn (lambda () body))  →  spawn(fn { body })
(send pid msg)      →  send(pid, msg)
(recv)              →  recv()
(self)              →  self()
(monitor pid)       →  monitor(pid)
```

### Match
```
(match expr
  (pat1 body1...)
  (pat2 body2...)
  (_ body3...))

→

match expr {
  pat1 -> body1...
  pat2 -> body2...
  _ -> body3...
}
```
Multi-expression bodies in match branches use `{ }`:
```
(pat -> { e1; e2 })
```

### Receive
```
(receive
  (pat1 body1...)
  (pat2 body2...))

→

receive {
  pat1 -> body1...
  pat2 -> body2...
}
```

### Patterns
```
'symbol           →  'symbol          (quoted symbol — stays the same)
42                →  42               (integer literal)
_                 →  _                (wildcard)
(cons a b)        →  cons(a, b)       (pair pattern)
'nil              →  nil              (nil literal)
```

### List patterns (proper list)
.lisp: `(pat1 pat2 pat3)` as a match pattern matches a proper list.
.ta: `[pat1, pat2, pat3]`

### Dotted pair patterns — CRITICAL CONVERSION
.lisp dotted pair pattern: `('ping . sender)` matches `(cons 'ping sender)`
.ta does NOT support dotted pair patterns. You must:

1. Change the MESSAGE to a proper list:
   - `(cons 'ping (self))` → `cons('ping, cons(self(), nil))`
   - `(cons 'hello (cons "world" 42))` → `cons('hello, cons(cons("world", 42), nil))`
     (wrap the original cdr tail in a single-element list)

2. Change the PATTERN to a list pattern:
   - `('ping . sender)` → `['ping, sender]`
   - `('hello . rest)` → `['hello, rest]`

### DOWN message patterns
DOWN messages from the VM are: `('DOWN . (ref . (pid . reason)))` — a dotted chain.
.lisp: `('DOWN r p reason)` matches this (as a proper list with 4 elements works because C compiler detects dotted tail)
.ta: `['DOWN, r, p, reason]` — list pattern with 4 elements

### Nil
```
'nil    →  nil
```

### Boolean
```
true    →  true
false   →  false
```

### Identifier naming
Convert kebab-case to snake_case:
```
pong-pid    →  pong_pid
my-name     →  my_name
new-pid     →  new_pid
test-pair   →  test_pair
```

## Conversion Process for each file

1. Read the .lisp file
2. Apply the syntax transformations above
3. Write the .ta file with same base name (e.g., `fib.lisp` → `fib.ta`)
4. Test with C compiler: `./tinyactor test/scripts/FILE.ta`
5. Test with bootstrap: `./tinyactor --bootstrap test/scripts/FILE.ta`
6. Compare output to the original .lisp output
7. Delete the .lisp file only after .ta passes both modes

## Common Pitfalls

1. **Operators need spaces**: `n-1` → parsed as identifier. Must write `n - 1`.
2. **Match scrutinee**: `(match (recv) ...)` → `match recv() { ... }`
3. **Nested function calls**: `(+ (f x) (g y))` → `f(x) + g(y)`
4. **Multi-body match branches**: `(pat (e1) (e2))` → `pat -> { e1; e2 }`
5. **Let in match body**: `(pat (let x v) (expr))` → `pat -> { let x = v; expr }`