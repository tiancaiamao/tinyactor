# Evaluation Task: Task A6 (Type Annotation Enforcement)

You are an independent evaluator. Evaluate the changes for Task A6.

## What was implemented
The C reader (`src/reader_ta.c`) now emits `(type-sig name (param_types...) ret_type)` forms when functions have type annotations. The `parse_source` function in `src/api.c` flattens `(begin ...)` forms at the top level. The C compiler (`src/compile.c`) handles `type-sig` as a no-op.

## Acceptance Criteria to Verify

### AC1: Type mismatch detected
Run:
```bash
cd /Users/genius/project/tinyactor
cat > /tmp/test-bad-annot.ta << 'EOF'
fn bad_fn() -> string {
  42
}
fn main() {
  print("PASS")
}
EOF
NWORKERS=1 ./tinyactor --bootstrap /tmp/test-bad-annot.ta '' --check 2>&1
```
Expected: Output contains a type error mentioning "string" and/or "int"

### AC2: Correct annotation passes
Run:
```bash
cat > /tmp/test-good-annot.ta << 'EOF'
fn good_fn(x: int) -> int {
  x + 1
}
fn main() {
  print("PASS")
}
EOF
NWORKERS=1 ./tinyactor --bootstrap /tmp/test-good-annot.ta '' --check 2>&1
```
Expected: Output "PASS" with no type errors

### AC3: Backward compatible (unannotated functions work)
Run:
```bash
cat > /tmp/test-no-annot.ta << 'EOF'
fn add(a, b) {
  a + b
}
fn main() {
  print(add(1, 2))
}
EOF
NWORKERS=1 ./tinyactor --bootstrap /tmp/test-no-annot.ta '' --check 2>&1
```
Expected: Output "3" with no errors

### AC4: Mixed annotations (some typed, some not)
Run:
```bash
cat > /tmp/test-mixed-annot.ta << 'EOF'
fn greet(name: string, times) -> string {
  name
}
fn main() {
  print("PASS")
}
EOF
NWORKERS=1 ./tinyactor --bootstrap /tmp/test-mixed-annot.ta '' --check 2>&1
```
Expected: Output "PASS" with no type errors

### AC5: All existing tests pass
Run: `make && bash test/run_all_tests.sh 2>&1 | tail -10`
Expected: No new failures (pre-existing flaky tests for monitor_test and error-send-to-dead are acceptable)

### AC6: Code quality review
Check `git diff src/reader_ta.c src/api.c src/compile.c` for:
- No memory leaks (malloc without free)
- No buffer overflows
- Consistent code style with surrounding code
- Comments explaining non-obvious logic

## Output
Write your evaluation to `/Users/genius/project/tinyactor/.pge/eval-typesig-crash.md` using this format:

```
# Eval: Task A6 (Type Annotation Enforcement)

## Results
| Criterion | Status | Evidence |
|-----------|--------|----------|
| AC1: Type mismatch detected | ✅/❌ | <command output> |
| AC2: Correct annotation passes | ✅/❌ | <command output> |
| ... | ... | ... |

## Code Quality
<observations>

## Verdict: PASS / FAIL
<if FAIL, list specific issues that must be fixed>
```