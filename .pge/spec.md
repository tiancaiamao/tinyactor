# Spec: Phase 6b — Exhaustiveness Checking + Constructor Arity Checking

## Goal

Add compile-time checks to the `.ta` reader:
1. **Exhaustiveness checking**: `match`/`receive` on ADT constructors must cover all variants (or have `_` wildcard)
2. **Constructor arity checking**: `Hello(x)` when `Hello` declares 2 params → error

All checks happen during parsing in `reader_ta.c`. No VM/compiler changes.

## Design

### 1. Extended Type Registry

Replace the flat constructor table with a type→variants mapping:

```c
typedef struct {
    char name[64];                // Type name: "Msg"
    char variants[32][64];        // Variant names: "Ping", "Pong", "Stop"
    int  variant_arities[32];     // Arg counts: 1, 0, 0
    int  n_variants;
} TypeInfo;

static TypeInfo types[64];
static int n_types = 0;

// Reverse lookup: constructor name → (type_index, variant_index, arity)
static int find_constructor(const char *name, int len, int *out_arity) {
    for (int t = 0; t < n_types; t++)
        for (int v = 0; v < types[t].n_variants; v++) {
            int vn = (int)strlen(types[t].variants[v]);
            if (vn == len && memcmp(types[t].variants[v], name, len) == 0) {
                if (out_arity) *out_arity = types[t].variant_arities[v];
                return 1; // found
            }
        }
    return 0; // not a registered constructor
}
```

### 2. Type Declaration Parsing (modify parse_toplevel_type)

When parsing `type Msg { Ping(Pid); Pong; Stop }`:
- Record type name "Msg"
- For each variant:
  - Record name
  - Count parameters (number of comma-separated types in parentheses)
  - 0 if no parentheses

### 3. Exhaustiveness Checking

After parsing all arms in a `match` or `receive` block:

```c
static void check_exhaustiveness(VM *vm, Val arms, const char *kw) {
    // 1. Collect constructor names used in patterns
    // 2. Check if there's a _ wildcard → exhaustive, done
    // 3. Group constructors by their parent type
    // 4. For each type group: check if all variants present
    // 5. If not all present → fprintf(stderr, "warning: non-exhaustive %s: missing %s\n", ...)
}
```

How to extract constructor names from patterns:
- Pattern `(quote Foo)` → nullary constructor "Foo"
- Pattern list `[(quote Foo), ...]` → n-ary constructor "Foo"
- Pattern `_` → wildcard (exhaustive)
- Pattern `sym` → variable binding (not a constructor)
- Other patterns → non-constructor, skip

The pattern AST shapes (from Phase 6a):
- Nullary ctor pattern: `(quote Bye)` = `val_pair(sym("quote"), val_pair(sym("Bye"), nil))`
- N-ary ctor pattern: `[(quote Hello), pat1, pat2]` = `val_pair((quote Hello), val_pair(pat1, val_pair(pat2, nil)))`

To extract constructor name from a pattern:
1. Check if pattern is a pair `(quote Name)` → extract Name
2. Check if pattern is a list whose first element is `(quote Name)` → extract Name
3. Check if pattern is `_` → wildcard

### 4. Constructor Arity Checking

When parsing a constructor expression `Hello(self(), "hi")`:
- Look up `Hello` in the type registry
- Get expected arity (2)
- Count actual args (2)
- If mismatch → `fprintf(stderr, "error: constructor %s expects %d args, got %d\n", ...)`

When parsing a constructor pattern `Hello(from, text)`:
- Same check

## Error Handling

- **Exhaustiveness**: WARNING (fprintf to stderr), compilation continues
- **Arity mismatch**: ERROR (fprintf to stderr), compilation continues but may crash at runtime
- Neither is fatal — this is a gradual rollout. Future: add a `--strict` flag.

## Implementation Location

All changes in `src/reader_ta.c`. The checks fire during parsing, printing warnings/errors to stderr.

## Acceptance Criteria

### L1 — Structural
- [ ] `make clean && make` — 0 errors
- [ ] Type registry tracks type→variants with arities
- [ ] Exhaustiveness check runs after each match/receive block
- [ ] Arity check runs on constructor expressions and patterns
- [ ] All 49 .lisp + 6 .ta tests pass (zero regression)

### L2 — Behavioral

**Test 1: Exhaustiveness warning**
```ta
type Msg { Ping(Pid); Pong; Stop }

fn main() {
  let m = Pong
  match m {
    Ping(from) -> print("ping")
    Pong -> print("pong")
    // Missing Stop! Should print warning to stderr
  }
}
```
Expected: stdout `pong`, stderr contains `non-exhaustive` and `Stop`

**Test 2: Exhaustive match (no warning)**
```ta
type Msg { Ping(Pid); Pong; Stop }

fn main() {
  let m = Pong
  match m {
    Ping(from) -> print("ping")
    Pong -> print("pong")
    Stop -> print("stop")
  }
}
```
Expected: stdout `pong`, stderr empty

**Test 3: Wildcard is exhaustive**
```ta
type Msg { Ping(Pid); Pong; Stop }

fn main() {
  let m = Stop
  match m {
    Ping(from) -> print("ping")
    _ -> print("other")
  }
}
```
Expected: stdout `other`, stderr empty

**Test 4: Constructor arity error**
```ta
type Msg { Hello(Pid, String); Bye }

fn main() {
  let m = Hello(self())  // Only 1 arg, expects 2
  match m {
    _ -> print("ok")
  }
}
```
Expected: stderr contains `Hello` and `expects 2` and `got 1`

**Test 5: No regression — existing ADT tests still clean**
```bash
./tinyactor test/scripts/adt-basic.ta  # No warnings
```

## Out of Scope
- Full type inference / type checking (Phase 6c)
- Type annotation enforcement on regular variables
- Generic types
- `--strict` flag to make warnings fatal