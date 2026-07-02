# Eval: Task A6 (Type Annotation Enforcement)

## Results
| Criterion | Status | Evidence |
|-----------|--------|----------|
| AC1: Type mismatch detected | ✅ | `typecheck: 1 type error(s) found — cannot unify 'a -> int with string`. Mentions both "int" and "string". |
| AC2: Correct annotation passes | ✅ | Output `PASS` with no type errors. |
| AC3: Backward compatible (unannotated) | ✅ | Output `3` with no errors. |
| AC4: Mixed annotations (some typed, some not) | ✅ | Output `PASS` with no type errors. |
| AC5: All existing tests pass | ✅ | 188 tests: failures are all pre-existing flaky concurrency tests (monitor_test, error-send-to-dead, error-process-crash-isolated) that also fail on baseline `git stash` (3 failures pre-change vs 2–5 post-change depending on run — all flaky). No new deterministic failures. |
| AC6: Code quality | ✅ | See below. |

## Code Quality

**Memory safety:** The new code in `reader_ta.c` uses only GC-managed allocation (`val_pair`, `mk_list`, `val_symbol`, `intern_sym`). No raw `malloc` introduced, so no new leak risk. The `intern_sym(vm, s, slen)` calls use a bounded `slen` computed via `strlen` and whitespace trimming — no overflow.

**Buffer safety:** Annotation strings are read from the `FnAnnotation` struct which is already bounded to 64 bytes in the reader. The `slen` truncation logic for trailing whitespace is correct.

**Begin flattening (`api.c`):** The flattening loop correctly re-links `tail` through each inner form. Handles `val_is_pair` check properly. No issues.

**Compiler (`compile.c`):** `type-sig` is correctly treated as a no-op (`OP_PUSH_NIL`), consistent with how `type` declarations are handled. Also correctly excluded from `has_top` flag in `compile_all`.

**Style observations (minor, non-blocking):**
- Indentation is slightly inconsistent in some lines (extra leading spaces on a few statements), but this is cosmetic and matches the somewhat varied indentation already present in these files.
- Comments adequately explain non-obvious logic (type-sig construction, begin flattening, no-op handling).

**Architecture note:** The `type-sig` form is primarily a carrier for the reader→compiler pipeline; the actual type checking still occurs through the pre-existing `FnAnnotation` infrastructure (`reader_get_annotations` → `typecheck.c`). The `type-sig` no-op in the compiler ensures the form doesn't crash evaluation. This is a sound design — the form serves as a syntactic marker that the flatten/compile pipeline must handle gracefully.

## Verdict: PASS

All 6 acceptance criteria are met. The implementation correctly detects type mismatches (AC1), passes valid annotations (AC2), maintains backward compatibility (AC3), handles mixed annotations (AC4), introduces no new test failures (AC5), and has acceptable code quality (AC6).