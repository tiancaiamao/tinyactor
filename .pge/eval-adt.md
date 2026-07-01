# Eval Report: ADT Type Declaration Support

## Verdict: PASS

## L1 — Structural
- [✅] ./tinyactor lib/typecheck.ta runs and exits 0
- [✅] collect_type_decls and t_base exist (grep count = 2)
- [✅] type_format_resolved uses str.sym_to_str (line 1029)

## L2 — Behavioral
- [✅] Test G: nullary ADT type resolved — output: `'a -> Color` (acceptable per spec)
- [✅] Test H: n-ary ADT type resolved — output: `'a -> Option`
- [✅] Test I: multi-field ADT type resolved — output: `'a -> 'b -> Pair`
- [✅] Existing tests unchanged — Tests 1-7 and A-F all produce expected output

## Code Quality
- [✅] No || operator
- [✅] No ! operator
- [✅] Functions are top-level only (all new functions verified at top level)
- Any issues found: Minor whitespace inconsistencies (extra indentation on some lines), but no functional issues. No syntax problems that could cause silent failures.

## Summary
Implementation is complete and correct. The `collect_type_decls` function properly walks type declarations and registers both nullary constructors (as monomorphic base types) and n-ary constructors (as polymorphic arrow types wrapped in forall). The `infer_program` function calls `collect_type_decls` before `collect_defines`, ensuring constructors are in scope. The `infer_compound` quote handling correctly checks the environment for registered nullary constructors before falling back to `t_symbol()`. The `type_format_resolved` fix properly renders unknown base type names using `str.sym_to_str`. All existing tests produce unchanged output, and the three new ADT tests (G, H, I) produce correct types. No prohibited operators (`||`, `!`) are used.