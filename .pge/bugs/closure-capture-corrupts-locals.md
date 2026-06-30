# Compiler Bug: Closure Variable Capture Corrupts Locals

## Status
FOUND — workaround applied (avoid closures in typecheck.ta)

## Reproduction
When a function contains a closure that captures local variables,
the mere PRESENCE of the closure (even in an unreachable branch)
causes local variables to become corrupted (replaced by closure cells).

## Minimal repro
```ta
fn outer(x, y, s) {
  let xid = car(cdr(x))    // xid = 1000 (correct)
  // ... unreachable branch with closure capturing x, y ...
  let r1 = bind_s(unify(...), fn(s1) {   // THIS closure captures x,y
    unify(car(cdr(cdr(x))), ...)
  })
  // xid is now ? (a closure/function value) instead of 1000
}
```

## Impact
`xid = car(cdr(x))` returns `?` (function) instead of `1000` (int).
This means `x` was replaced by a closure/cell object.

## Likely cause
Compiler boxes captured variables (converts them to heap cells)
for ALL functions that contain closures, even before the closure
is created. The boxing replaces the local slot with a cell reference,
but `car`/`cdr` on a cell returns internal structure instead of the
intended data.

## Workaround
Avoid closures in deeply nested functions. Use explicit if/else
instead of continuation-passing style:

Instead of:
```
bind_s(unify(a, b, s), fn(s1) { unify(c, d, s1) })
```
Use:
```
let r = unify(a, b, s)
if car(r) == 'succ { unify(c, d, cdr(r)) }
else { r }
```