# Task: Fix Phase 2 Issues — Review + Evaluator Findings

## Context
TinyActor Phase 2 is functionally complete (45/45 tests pass), but independent review and evaluation found several correctness issues. Fix all P1 and P2 issues while keeping all 45 tests passing.

## Source of Issues

### Evaluator Findings (3 caveats)
1. `src/cfunc.c` does not exist — C function support is distributed across files, not in a separate file as spec said
2. `bytes` type not implemented (segfaults on use) — test marked EXPECTED-FAIL
3. `gc_root_push/pop` defined but never called

### Code Review Findings (11 items)

#### P1 Issues (5 items) — MUST FIX

**1. GC safety hole in val_pair (CRITICAL)**
- `val_pair(Proc *p, Val car, Val cdr)` calls `proc_heap_alloc` which may trigger `gc_collect`
- The `car`/`cdr` C locals hold heap pointers that become stale after GC swaps fromspace→tospace
- The stale pointers get written into the new HeapPair's car/cdr fields → silent data loss
- Same issue in `val_closure`, `val_string`, `val_bytes` — any constructor that takes Val args
- Fix: Use `gc_root_push`/`gc_root_pop` to protect Val arguments before calling `proc_heap_alloc`
- Location: src/val.c:73-82 (val_pair), and similar constructors

**2. OP_CCALL buffer overflow**
- `Val args[16]` on C stack, but `nargs` is `uint8_t` (0-255)
- If nc > 16, the loop writes past the buffer → C stack corruption
- Fix: Use a larger buffer (e.g., 64) or add bounds check
- Location: src/vm.c:769-779

**3. Missing type checks in string ops**
- OP_STR_LEN checks `val_tag(s) != TAG_STRING`, but OP_STR_CONCAT, OP_STR_SLICE, OP_STR_EQ don't
- Non-string operands → val_get_string interprets low 48 bits as pointer → arbitrary memory dereference
- Fix: Add type checks like OP_STR_LEN does, push nil on type mismatch
- Location: src/vm.c:726-766

**4. gc_collect: no heap-stack collision check after copy**
- After copying all live objects to tospace, gc_to_size might exceed the stack region
- The subsequent memcpy restoring the stack can overwrite part of the new heap
- Fix: After gc_scan_tospace, check if gc_to_size would collide with stack. If so, try proc_grow. If grow fails, abort GC (don't swap).
- Location: src/gc.c:79-123

**5. proc_grow error path: dangling pointer + leak**
- If realloc(p->mem) succeeds but realloc(p->gc_to) fails, p->mem may point to freed memory
- Fix: Order allocations so failure is recoverable, or save old pointer and restore on failure
- Location: ta.h:359-371

#### P2 Issues (5 items) — SHOULD FIX

**6. OP_CCALL: no bounds check on cfunc index**
- cfidx from bytecode used directly to index vm->cfuncs[cfidx] without checking < cfunc_count
- Fix: Add bounds check, push nil if out of range
- Location: src/vm.c:770-775

**7. gc_copy_obj: misaligned forwarding pointer write**
- Forwarding pointer written at offset sizeof(HeapHeader) (2 bytes) as void* (8 bytes)
- Works on x86-64 but UB in C, traps on strict-alignment architectures
- Fix: Use memcpy for the forwarding pointer read/write instead of direct dereference
- Location: src/gc.c:28-42

**8. OP_STR_CONCAT: VLA can overflow C stack for large strings**
- `char tmp[len1 + len2 + 1]` on C stack
- Fix: Use malloc for large strings, VLA only for small ones, or cap size
- Location: src/vm.c:733

**9. gc_scan_tospace: missing default case handling**
- Unknown heap types advance by sizeof(HeapHeader) only, could misparse
- Fix: Add assertion or abort in default case of obj_size
- Location: src/gc.c:55-76

**10. vm_register: strdup failure not checked**
- strdup(name) can return NULL, stored directly without check
- Fix: Check strdup return, return error code or abort
- Location: src/api.c:97-103

#### P3 Issues (1 item) — OPTIONAL
**11. Inconsistent indentation** — Skip this, cosmetic only.

## Implementation Order

1. **Fix P1 #1 first** — GC safety in val_pair/val_closure/val_string/val_bytes
   - This is the most critical bug. Add gc_root_push before proc_heap_alloc, gc_root_pop after
   - Example fix for val_pair:
     ```c
     Val val_pair(Proc *p, Val car, Val cdr) {
         gc_root_push(p, car);
         gc_root_push(p, cdr);
         HeapPair *hp = (HeapPair *)proc_heap_alloc(p, sizeof(HeapPair));
         gc_root_pop(p); // cdr
         gc_root_pop(p); // car
         if (!hp) return val_nil();
         hp->hdr.type = HEAP_PAIR;
         hp->hdr.flags = 0;
         hp->car = car;
         hp->cdr = cdr;
         return box_tag_payload(TAG_PAIR, (uint64_t)(uintptr_t)hp);
     }
     ```
   - Apply same pattern to val_closure, val_string (no Val args, safe already), val_bytes (safe already)

2. **Fix P1 #2** — OP_CCALL args buffer → use args[64] or check bounds

3. **Fix P1 #3** — Add type checks to OP_STR_CONCAT/SLICE/EQ

4. **Fix P1 #4** — gc_collect heap-stack collision check

5. **Fix P1 #5** — proc_grow error path

6. **Fix P2 items** — bounds checks, memcpy for forwarding, VLA fix, assertions

## Verification
After ALL fixes:
```bash
cd /Users/genius/project/tinyactor
make clean && make 2>&1
# All 45 tests must still pass
for f in test/scripts/*.lisp; do
  echo -n "$(basename $f): "
  timeout 15 ./tinyactor "$f" 2>&1 | tr '\n' ' '
  echo ""
done
```

## CRITICAL RULES
1. WRITE CODE. Fix issues. Build and test.
2. ALL 45 existing tests must continue to pass after fixes.
3. Do NOT change test files.
4. Do NOT add new features — only fix bugs identified in the review.