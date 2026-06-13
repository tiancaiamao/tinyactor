# Task: Implement GC Core + GC Integration for TinyActor

## Context
TinyActor is a lightweight C-embedded actor scripting language. Phase 1 is complete with 11/11 tests passing. Phase 2 adds per-process semispace copying GC.

## Current Architecture
- **Memory layout per process**: `uint8_t *mem` is a contiguous block. Heap grows upward from `mem[0]` (tracked by `heap_ptr`). Stack grows downward from `mem + mem_size` (tracked by `sp`, a negative offset). When `heap_ptr + new_size > mem_size + sp * sizeof(Val)`, allocation fails.
- **Heap objects** (defined in ta.h):
  ```c
  typedef struct { uint8_t type; uint8_t flags; } HeapHeader;
  typedef struct { HeapHeader hdr; Val car, cdr; } HeapPair;
  typedef struct { HeapHeader hdr; int entry; int nfree; Val free[]; } HeapClosure;
  typedef struct { HeapHeader hdr; int len; char data[]; } HeapString;
  typedef struct { HeapHeader hdr; int len; uint8_t data[]; } HeapBytes;
  ```
- **Object types**: HEAP_PAIR=1, HEAP_CLOSURE=2, HEAP_STRING=3, HEAP_BYTES=4
- **Proc already has**: `uint8_t *gc_to; int gc_to_size;` (allocated but unused)
- **Val is uint64_t**: NaN-boxed. Tag in top 16 bits, payload in lower 48 bits. Heap objects are stored as raw pointers in the payload.
- **TAG_CLOS_ID (0xFF0A)**: When nfree=0, closure is encoded directly in Val as `(TAG_CLOS_ID<<48)|fn_id` — no heap allocation. These do NOT need GC.
- **Mailbox**: `Val *mbox` (separately allocated ring buffer), contains Val entries
- **Catch frames**: `CatchFrame catch_stack[8]` with sp, fp fields — values on stack between catch_sp and sp should be roots

## What to Implement

### 1. `src/gc.c` — Semispace Copying Collector (~150 lines)

Implement these functions:

```c
#include "ta.h"

/* Check if a pointer is in the fromspace */
static int in_fromspace(Proc *p, void *ptr);

/* Check if an object has been forwarded */
static int is_forwarded(HeapHeader *h);

/* Get forwarding pointer from a forwarded object */
static void *get_forward(HeapHeader *h);

/* Set forwarding pointer in an object */
static void set_forward(HeapHeader *old, void *new_ptr);

/* Copy a single heap object to tospace, return new pointer */
static void *gc_copy_obj(Proc *p, void *obj);

/* Update a single Val: if it points to fromspace, copy and update */
static void gc_copy_val(Proc *p, Val *v);

/* Scan tospace, updating all internal references */
static void gc_scan_tospace(Proc *p);

/* Main GC entry point */
void gc_collect(Proc *p);
```

**gc_collect algorithm**:
1. Initialize tospace scan pointer to `gc_to[0]`
2. Set `gc_to_size` = 0 (we'll track how much we've copied)
3. Scan roots:
   - **Stack**: For `i` from `sp` to -1, `gc_copy_val(p, &proc_stack(p)[i])`
   - **Mailbox**: For each message in mbox ring buffer, `gc_copy_val(p, &mbox[i])`
   - **gc_roots**: For `i` from 0 to `gc_root_count-1`, `gc_copy_val(p, &gc_roots[i])`
   - **catch_stack**: Values between catch frame's sp and current sp (already covered by stack scan)
4. `gc_scan_tospace(p)`: scan all objects in tospace, updating their child references
5. Swap: `gc_to` becomes new heap, old `mem` becomes `gc_to`. Update `heap_ptr` to tospace used.

**gc_copy_obj**:
- Get `HeapHeader *h = (HeapHeader *)obj`
- If already forwarded, return forwarding pointer
- Calculate object size based on type:
  - HEAP_PAIR: sizeof(HeapPair)
  - HEAP_CLOSURE: sizeof(HeapClosure) + h->nfree * sizeof(Val)
  - HEAP_STRING: sizeof(HeapString) + ((HeapString*)obj)->len + 1
  - HEAP_BYTES: sizeof(HeapBytes) + ((HeapBytes*)obj)->len
- Memcpy to tospace
- Set forwarding pointer in old object
- Advance tospace allocation pointer

**gc_copy_val**:
- Check if Val's tag is a heap-allocated type (TAG_PAIR, TAG_CLOS, TAG_STRING, TAG_BYTES)
- Extract pointer, check if in fromspace
- If yes, gc_copy_obj and update the Val with new pointer
- TAG_CLOS_ID values do NOT need GC (they're immediate)

**gc_scan_tospace**:
- Iterate through all objects in tospace from `gc_to[0]` to current `gc_to_size`
- For each object, update its child Val references (gc_copy_val on each):
  - HEAP_PAIR: car, cdr
  - HEAP_CLOSURE: each free[i]
  - HEAP_STRING, HEAP_BYTES: no child refs (data is inline)
- Use same size calculation as gc_copy_obj to advance through tospace

### 2. `ta.h` modifications

Add these to Proc struct (some may already exist, just verify):
```c
Val       gc_roots[16];    /* temporary root stack for C functions */
int       gc_root_count;
```

Add declarations:
```c
void gc_collect(Proc *p);
static inline void gc_root_push(Proc *p, Val v);
static inline Val gc_root_pop(Proc *p);
```

Add inline implementations:
```c
static inline void gc_root_push(Proc *p, Val v) {
    if (p->gc_root_count < 16) p->gc_roots[p->gc_root_count++] = v;
}
static inline Val gc_root_pop(Proc *p) {
    return p->gc_roots[--p->gc_root_count];
}
```

Add `VM->current_proc` field:
```c
struct VM {
    ...
    Proc *current_proc;   /* currently executing process (for C functions) */
    ...
};
```

### 3. `src/vm.c` modifications

In `proc_new()`:
- Allocate `gc_to = calloc(1, mem_size)` alongside `mem`
- Set `gc_to_size = 0` (no objects copied yet)

In `proc_free()`:
- Free `gc_to`

In `proc_heap_alloc()`:
- When allocation fails (heap-stack collision):
  1. Call `gc_collect(p)`
  2. Retry the allocation
  3. If still fails, try `proc_grow(p)` — realloc both `mem` and `gc_to` to double size
  4. If still fails, return NULL (process will crash gracefully)

Add `proc_grow(Proc *p)` function:
- Realloc `mem` to 2x size
- Realloc `gc_to` to same new size
- Update `mem_size`
- Adjust stack pointers if needed

In `vm_step()`:
- Set `vm->current_proc = p` at the start of each step

### 4. `src/val.c` modifications

In `val_pair()`, `val_string()`, `val_bytes()`:
- These call `proc_heap_alloc()`. If it returns NULL (GC already tried), return val_nil() — the caller already handles this.
- No changes needed beyond what proc_heap_alloc already does.

### 5. Makefile

Add `src/gc.o` to the build:
```makefile
OBJS = src/val.o src/reader.o src/compile.o src/vm.o src/gc.o src/api.o src/main.o
```

## Important Notes
- **DO NOT** modify compile.c — no compiler changes needed for GC
- **DO NOT** modify reader.c — no reader changes needed
- **DO NOT** change any opcode behavior — GC is transparent to the VM loop
- **Preserve all Phase 1 behavior** — 11 existing tests must still pass
- The `HeapHeader.flags` field is currently always 0. Use bit 0 (0x01) for the forwarded flag.
- For forwarding pointer storage: when forwarded, set `h->type = HEAP_FORWARDED` (define as 0xFF) and store the tospace pointer in the first `sizeof(void*)` bytes after the header.
- **Alignment**: heap objects are 8-byte aligned (proc_heap_alloc already aligns to 8 bytes)
- **TAG_CLOS (0xFF03)**: regular closure with nfree>0, allocated on heap. Needs GC.
- **TAG_CLOS_ID (0xFF0A)**: nfree=0 closure, encoded in Val. Does NOT need GC.

## Verification
After implementation:
1. `make clean && make` — compiles clean
2. Run all 11 Phase 1 tests — all still pass
3. Run gc-pair-churn test — outputs "done"
4. Run gc-closure-churn test — outputs "42"
5. Run gc-retains-stack-refs test — outputs "30"
6. Run gc-retains-free-vars test — outputs correct value