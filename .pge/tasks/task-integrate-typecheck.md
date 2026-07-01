# Task: Integrate typecheck into driver.ta compilation pipeline

## Problem
The type checker (`lib/typecheck.ta`) works in isolation but is never called during compilation. `driver.ta`'s pipeline is: `tokenize → parse → codegen → run`, with no typecheck step. Additionally, `typecheck.ta` has zero `pub fn` declarations, so its functions are nil when accessed from other modules — this is why cross-module calls crash with "cannot call non-function value (tag=0xff01=NIL)".

## Objective
Connect the type checker to the compilation pipeline so annotated programs are actually type-checked.

## Files to Modify

### 1. `lib/typecheck.ta` — export `infer_program`

Change `fn infer_program(forms)` to `pub fn infer_program(forms)`.

This is the ONLY function that needs to be exported. The driver only calls `typecheck.infer_program(ast)`.

### 2. `lib/driver.ta` — add typecheck step

Add `import typecheck` at the top, and call typecheck after parsing and before codegen.

Current `compile_file(path)` flow:
```
let ast = vm.load_source(path)
let bc = codegen.compile(ast)
```

New flow:
```
let ast = vm.load_source(path)
let tc_result = typecheck.infer_program(ast)
let bc = codegen.compile(ast)
```

The typecheck result is obtained but **errors are non-fatal for now** — typecheck runs, but compilation proceeds regardless. This is the "permissive" approach from the spec. A print statement can show the typecheck ran successfully.

Do the same for `compile_and_run(source)`.

**Do NOT add typecheck to `compile_file_to_tabc`** — the bootstrap path must remain lean (typecheck.ta itself needs to be compiled via this path, and it can't typecheck itself during bootstrap).

## Key Context

- `infer_program(forms)` returns `(env . (substitution . counter))` on success
- If it crashes, the error is non-fatal — driver should still compile
- The `typecheck.ta` module has ~90 helper functions, all private. Only `infer_program` needs to be `pub fn`
- `driver.ta` currently imports: tokenizer, parser, codegen, buf, file, str, vm
- The `import typecheck` will cause the VM to load and compile `lib/typecheck.ta` at startup

## Implementation Steps

1. In `lib/typecheck.ta`: change `fn infer_program` to `pub fn infer_program` (line ~1059)
2. In `lib/driver.ta`:
   - Add `import typecheck` to imports
   - In `compile_file(path)`: after `let ast = vm.load_source(path)`, add `let tc = typecheck.infer_program(ast)` before codegen
   - In `compile_and_run(source)`: after `let ast = parser.parse(toks)`, add `let tc = typecheck.infer_program(ast)` before codegen
3. Run verification commands (see below)

## Verify

```bash
# 1. typecheck.ta still works standalone
./tinyactor lib/typecheck.ta

# 2. driver.ta works (this now imports typecheck)
./tinyactor lib/driver.ta lib/hello.ta

# 3. End-to-end: annotated program compiles and runs
./tinyactor test/scripts/type-pass.ta

# 4. Full test suite
make test

# 5. Cross-module call works (the crash is fixed)
cat > /tmp/test_e2e.tc.ta << 'EOF'
import tokenizer
import parser
import typecheck

fn main() {
  let toks = tokenizer.tokenize("fn add(x: int, y: int) -> int { x + y }")
  let ast = parser.parse(toks)
  let result = typecheck.infer_program(ast)
  print("typecheck OK")
}
EOF
./tinyactor /tmp/test_e2e.tc.ta
# Expected: "typecheck OK" (no crash)
```

## Constraints
- NO `||` operator (broken in .ta)
- NO `!` operator (use `== false`)
- Functions top-level only
- Keep functions under 30 statements
- Bootstrap path (`compile_file_to_tabc`) must NOT call typecheck