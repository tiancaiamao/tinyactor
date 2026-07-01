# Evaluation Report: Phase 3 — Type Annotation Preservation + Validation

**Verdict: ⚠️ PARTIAL — Parser + Codegen done; Type Checker work entirely missing**

---

## Step 1: Core commands (all exit 0)

| Command | Result | Notes |
|---|---|---|
| `./tinyactor lib/parser.ta` | ✅ EXIT 0 | Parser self-consistent |
| `./tinyactor test/scripts/type-pass.ta` | ✅ EXIT 0 | Output: `3`, `hello`, `PASS` — matches expected |
| `./tinyactor lib/driver.ta` | ✅ EXIT 0 | No error message, bootstrap works |
| `./tinyactor lib/typecheck.ta` | ✅ EXIT 0 | Tests 1-7, A-I all pass |

## Step 2: Annotated function emits type-sig

Ran the `/tmp/test_annot.ta` test. Output:
```
((type-sig add (int int) int) (define (add x y) (+ x y)))
```
✅ **Exactly matches expected output.**

## Step 3: Unannotated function does NOT emit type-sig

Ran the `/tmp/test_no_annot.ta` test. Output:
```
((define (id x) x))
```
✅ **Exactly matches expected output** — no `type-sig` form.

## Step 4: Codegen.lisp changes

`git diff lib/codegen.lisp` shows `type-sig` added to skip conditions in `compile_top_level` (one new `if` branch mirroring the existing `type` skip). Paren balancing verified by reading surrounding context — all `if` branches close correctly, and the build/runtime works.

`pass1_register` was NOT modified, but this is acceptable: `pass1_register` already skips any form whose head is not a `define` form, so `type-sig`/`import`/`type` are all skipped naturally. ✅

---

## L1 — Structural Acceptance Criteria

| Criterion | Status | Evidence |
|---|---|---|
| `make -j4` succeeds | ✅ | Binary present, all commands run |
| `./tinyactor lib/typecheck.ta` exits 0 | ✅ | All existing tests pass |
| `./tinyactor lib/parser.ta` exits 0 | ✅ | Parser self-consistent |
| Parser emits `type-sig` for annotated fns | ✅ | Step 2 above |
| Codegen skips `type-sig` forms | ✅ | `type-pass.ta` compiles fine |

**L1: 5/5 PASS**

## L2 — Behavioral Acceptance Criteria

| Criterion | Status | Evidence |
|---|---|---|
| Test J: Annotated fn type-checks (types match annotation) | ❌ | `test_j` does not exist — **typecheck.ta was not modified at all** |
| Test K: Partially annotated fn type-checks | ❌ | `test_k` does not exist |
| All existing typecheck tests (1-7, A-I) still pass | ✅ | Verified in Step 1 |
| `test/scripts/type-pass.ta` compiles | ✅ | Step 1 |
| Bootstrap (`lib/driver.ta`) works | ✅ | Step 1 |

**L2: 3/5 PASS, 2 FAIL**

---

## Critical Gap: `lib/typecheck.ta` Unchanged

The spec explicitly lists **3 files to modify**: `lib/parser.ta`, `lib/codegen.lisp`, and **`lib/typecheck.ta`**. Only the first two were touched:

```
$ git diff --stat lib/
 lib/codegen.lisp | 12 ++++----
 lib/parser.ta    | 91 +++++++++++++++++++++++++++++++++++++++++++-------------
 2 files changed, 78 insertions(+), 25 deletions(-)
```

`git status lib/typecheck.ta` shows the file is untouched. None of the required additions exist:
- ❌ `parse_type_annot(sym)` — not present
- ❌ `collect_type_sigs(forms, env, counter)` — not present
- ❌ `infer_define` modification to unify with annotations — not present
- ❌ `test_j()`, `test_k()`, `test_l()` — not present

### Demonstrated failure

Calling `typecheck.infer_program` on an annotated program **crashes**:

```
$ ./tinyactor /tmp/test_typecheck_annot.ta
((type-sig add (int int) int) (define (add x y) (+ x + y)))
error: cannot call non-function value (tag=0xff01, raw=0xff01000000000000, pc=18732, nargs=1)
```

The type checker has no handling for `type-sig` forms — it treats `(type-sig add (int int) int)` as a function-call expression and crashes. Even a minimal "skip `type-sig` gracefully" change was not done.

---

## Parser Implementation Review

The parser changes are well-structured and correct:
- `parse_single_param` now returns `(name_sym . (type_sym . pos))` — captures annotation instead of skipping
- `parse_return_type` correctly handles `-> Type` or returns `nil`
- `parse_fn` extracts names and types via `extract_names`/`extract_types`, emits `type-sig` only when `has_any_type` is true
- Return format generalized to a list of forms; `parse_toplevel` wraps all other cases via `wrap_single`, and `parse_toplevels` flattens via `prepend_all`
- Verified: unannotated functions produce identical output to before (`((define (id x) x))`)

## Codegen Implementation Review

Single clean addition — `type-sig` branch mirrors the existing `type` skip in `compile_top_level`. Correct.

---

## Summary

- **L1 (Structural): 5/5 PASS**
- **L2 (Behavioral): 3/5 PASS, 2 FAIL**

The parser and codegen portions of the spec are correctly and cleanly implemented. However, **the entire type-checker portion was skipped** — `lib/typecheck.ta` was not modified at all, Test J and Test K do not exist, and the type checker crashes on annotated programs. This is one of three files the spec requires changing, and represents roughly one third of the work.

**Verdict: ⚠️ PARTIAL PASS** — Parser + Codegen done well; Type Checker work missing entirely. Cannot accept as complete.