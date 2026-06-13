# Task: Implement String/Bytes Builtins + C Function Call Mechanism

## Context
TinyActor Phase 2. GC core has been implemented (gc.c). Now add string builtins and the C function call mechanism.

## Current State
- `ta.h` has HeapString, HeapBytes types already defined
- `ta.h` has OP_PRINT, OP_ADD, OP_SUB, etc. as inline ops
- `ta.h` has a C function registry in VM struct:
  ```c
  struct {
      char *name;
      Val  (*fn)(VM *vm, Val *args, int nargs);
      int  nargs;
  } cfuncs[MAX_CFUNCS];
  int cfunc_count;
  ```
- `api.c` has `vm_register(vm, name, fn, nargs)` stub
- The compiler (`compile.c`) has a list of "inline ops" that map symbol names to opcodes
- GC is now active — heap allocations may trigger GC, so C functions must be GC-safe

## What to Implement

### Part A: String Builtins

Add string operations as C functions registered via `vm_register`. These will be called through OP_CCALL (see Part B).

**Functions to implement:**
1. `string-length(s)` → returns int length of string
2. `string-concat(s1, s2)` → returns new concatenated string
3. `string-slice(s, start, end)` → returns substring from start to end (0-indexed, exclusive end)
4. `string-eq(s1, s2)` → returns true if strings equal, nil otherwise

**Implementation notes:**
- These are registered as C functions via `vm_register` in `api.c` or a new `src/builtins.c`
- They use `proc_heap_alloc` for new strings, which may trigger GC
- For string-concat: extract C strings to locals FIRST, then allocate and copy
- For string-slice: validate bounds, return empty string for out-of-range

### Part B: C Function Call Mechanism

Add the ability for compiled code to call C functions registered via `vm_register`.

**New opcode: OP_CCALL**
- Same format as OP_CALL: `OP_CCALL cfunc_idx nargs`
- In compile.c: when compiling a function call, if the function name matches a registered C function, emit OP_CCALL instead of OP_CALL

**Compile changes (compile.c):**
In `cx_call()`, before generating OP_CALL:
1. Check if the function name is in `vm->cfuncs[]` 
2. If yes, emit `OP_CCALL cfunc_idx nargs` 
3. If no, continue with existing OP_CALL logic

Wait — the compiler doesn't have access to VM. Let me reconsider.

**Alternative approach:** Register string builtins as special opcodes (like OP_PRINT).

Actually, looking at the codebase more carefully:
- `compile.c` has `inline_ops` table mapping names like "+", "-", "print" to opcodes
- The simplest approach: add string operations to the inline_ops table

**Revised approach:**
1. Add new opcodes to ta.h: `OP_STR_LEN`, `OP_STR_CONCAT`, `OP_STR_SLICE`, `OP_STR_EQ`
2. Add them to the `inline_ops` table in compile.c
3. Implement in vm_step() in vm.c

This is simpler and doesn't require OP_CCALL. OP_CCALL can be added later for user-registered C functions.

**But the user wants the C function mechanism to work.** So:

**Final approach: Both**
1. String builtins as inline opcodes (OP_STR_LEN, etc.) for performance
2. OP_CCALL for general C function calling — registered via `vm_register`

### Implementation Details

#### 1. ta.h additions
```c
/* New opcodes */
OP_STR_LEN,
OP_STR_CONCAT,
OP_STR_SLICE,
OP_STR_EQ,
OP_CCALL,         /* cfunc_idx(4), nargs(1) */

/* New heap type */
#define HEAP_USERDATA 5
typedef struct {
    HeapHeader hdr;
    int size;
    void (*finalizer)(void *data);
    uint8_t data[];
} HeapUserdata;
```

#### 2. compile.c additions

Add to `inline_ops` table (find where +,-,print are listed):
```c
{"string-length", OP_STR_LEN},
{"string-concat", OP_STR_CONCAT},
{"string-slice", OP_STR_SLICE},
{"string-eq", OP_STR_EQ},
```

For OP_CCALL: in `cx_call()`, add a check after the inline_ops check:
- Look up the function name in a compile-time cfunc table (the compiler needs access to cfunc names)
- If found, emit `OP_CCALL idx nargs`

Actually, the compiler doesn't have a VM pointer. The simplest design:
- Add a `char *cfunc_names[MAX_CFUNCS]` field to the compiler struct OR to VM (write-only during register)
- Compiler checks this table to decide between OP_CALL and OP_CCALL
- During `vm_load()`, copy registered cfunc names to the compiler's table

Even simpler: just use the VM's cfunc table. The compiler's `Compiler` struct could hold a reference to VM, or we pass VM to the compiler. Look at how `compile_all` is called — it takes a `VM *vm` parameter already! Check `src/compile.c` for `compile_all(VM *vm, ...)` or similar.

#### 3. vm.c additions

In `vm_step()`:
```c
case OP_STR_LEN: {
    Val s = proc_pop(p);
    HeapString *hs = val_get_string(s);
    proc_push(p, val_int(hs->len));
    break;
}
case OP_STR_CONCAT: {
    Val s2 = proc_pop(p);
    Val s1 = proc_pop(p);
    HeapString *h1 = val_get_string(s1);
    HeapString *h2 = val_get_string(s2);
    // Extract to C locals before allocating
    int len1 = h1->len, len2 = h2->len;
    char *buf = malloc(len1 + len2 + 1); // temp buffer
    memcpy(buf, h1->data, len1);
    memcpy(buf + len1, h2->data, len2);
    buf[len1 + len2] = '\0';
    Val result = val_string(p, buf, len1 + len2);
    free(buf);
    proc_push(p, result);
    break;
}
case OP_STR_SLICE: {
    Val end = proc_pop(p);
    Val start = proc_pop(p);
    Val s = proc_pop(p);
    HeapString *hs = val_get_string(s);
    int s_val = (int)val_get_int(start);
    int e_val = (int)val_get_int(end);
    if (s_val < 0) s_val = 0;
    if (e_val > hs->len) e_val = hs->len;
    if (s_val >= e_val) { proc_push(p, val_string(p, "", 0)); break; }
    Val result = val_string(p, hs->data + s_val, e_val - s_val);
    proc_push(p, result);
    break;
}
case OP_STR_EQ: {
    Val s2 = proc_pop(p);
    Val s1 = proc_pop(p);
    HeapString *h1 = val_get_string(s1);
    HeapString *h2 = val_get_string(s2);
    int eq = (h1->len == h2->len && memcmp(h1->data, h2->data, h1->len) == 0);
    proc_push(p, eq ? val_true() : val_nil());
    break;
}
case OP_CCALL: {
    int cfidx = read_int32(p); // read cfunc index
    uint8_t nc = p->code[p->pc++];
    // Pop nc args from stack into array
    Val args[16];
    for (int i = nc - 1; i >= 0; i--) args[i] = proc_pop(p);
    vm->current_proc = p;
    Val result = vm->cfuncs[cfidx].fn(vm, args, nc);
    proc_push(p, result);
    break;
}
```

#### 4. api.c additions

Complete `vm_register()`:
```c
void vm_register(VM *vm, const char *name, Val (*fn)(VM*, Val*, int), int nargs) {
    if (vm->cfunc_count >= MAX_CFUNCS) return;
    int idx = vm->cfunc_count++;
    vm->cfuncs[idx].name = strdup(name);
    vm->cfuncs[idx].fn = fn;
    vm->cfuncs[idx].nargs = nargs;
}
```

#### 5. Makefile
Update if new source files are added.

## Important Notes
- **GC safety**: In string-concat, extract C string data to local variables BEFORE calling val_string (which allocates on heap and may trigger GC). The heap pointers h1/h2 may be invalidated after GC.
- **GC scanning**: GC must handle HeapString correctly — it has no child Val references, just inline char data.
- **HeapUserdata**: Just add the type definition to ta.h. No need to implement finalizers yet. GC should copy it like HeapBytes (memcpy + no child refs).
- **OP_CCALL**: This enables the C function mechanism. Keep it simple — no closures over C functions, no C function values as first-class.
- **Compilation of cfuncs**: The compiler needs to know which names are C functions. Since `compile_all` receives `VM*`, it can check `vm->cfuncs`. Add cfunc name checking in `cx_call`.

## Verification
1. `make clean && make` — compiles clean
2. Phase 1 tests all pass (11/11)
3. `echo '(print (string-length "hello"))' | ./tinyactor` → 5
4. `echo '(print (string-concat "hello" " world"))' | ./tinyactor` → hello world
5. `echo '(print (string-slice "hello" 1 3))' | ./tinyactor` → el
6. String GC stress test passes
7. Test C function registration and calling from script