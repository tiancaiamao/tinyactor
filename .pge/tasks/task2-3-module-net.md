# Task 2+3: Module System + Network C Module + I/O Scheduler

## Part A: Module System

### Design (reuse existing OP_CCALL infrastructure)

1. **TaModule struct** in ta.h:
```c
typedef struct {
    const char *name;
    struct {
        const char *name;
        Val (*fn)(VM *vm, Val *args, int nargs);
        int nargs;
    } *funcs;
    int nfuncs;
} TaModule;
```

2. **vm_register_module(vm, "net", funcs, nfuncs)** in new file `src/module.c`:
   - For each function in the module, add it to `vm->cfuncs[]` with name `"module.funcname"` (e.g., "net.listen")
   - Track registered modules in `vm->modules[]` array

3. **Compiler: `(import "name")` special form** in compile.c:
   - `cx_def` or a new handler: recognize `(import "name")` as a compile-time directive
   - Record imported module names so the compiler knows to resolve `module.func` calls
   - No runtime code emitted — import is purely compile-time

4. **Compiler: `module.func` call resolution** in compile.c cx_call():
   - When compiler sees a symbol like "net.listen", it should check if it matches `module.func` pattern for an imported module
   - If yes, look up "net.listen" in `vm->cfuncs[]` → compile as OP_CCALL
   - This already works with existing cfunc lookup! Just need `(import "net")` to validate the module exists

### Files to Create/Modify
- `ta.h`: TaModule struct, vm_register_module decl, modules array in VM struct
- `src/module.c` (NEW): vm_register_module implementation
- `src/compile.c`: (import "name") handler in compile_all or cx_def
- `Makefile`: add src/module.o

### Test: module_test.lisp
```lisp
;; This test uses a built-in test module registered by main.c
(import "test")
(print (test.hello))   ;; → "hello from C"
(print (test.add 3 4)) ;; → 7
```

For this test to work, add a test module registration in main.c (or a separate test_helper.c):
```c
static Val test_hello(VM *vm, Val *args, int nargs) {
    (void)vm; (void)args; (void)nargs;
    return val_string(current_proc(vm), "hello from C");
}
static Val test_add(VM *vm, Val *args, int nargs) {
    (void)vm;
    if (nargs < 2) return val_int(0);
    return val_int(val_as_int(args[0]) + val_as_int(args[1]));
}
// In main or init:
TaFunc test_funcs[] = {
    {"hello", test_hello, 0},
    {"add", test_add, 2},
    {NULL, NULL, 0}
};
vm_register_module(vm, "test", test_funcs, 2);
```

## Part B: Network C Module

### New file: `src/net.c`

Implement these C functions (all registered as the "net" module):

1. **net.listen(port)** — Create TCP server socket
   - socket(), bind(), listen()
   - Set non-blocking (fcntl O_NONBLOCK)
   - Return socket fd as Val int

2. **net.accept(server_fd)** — Accept connection (non-blocking)
   - accept() on the server socket
   - If would-block (EAGAIN/EWOULDBLOCK): return symbol `'would-block`
   - Set client socket non-blocking
   - Return client fd as Val int

3. **net.read(fd [, max_len])** — Read from socket (non-blocking)
   - read() with default max_len=4096
   - If would-block: return symbol `'would-block`
   - If EOF (ret=0): return symbol `'eof`
   - Otherwise: return data as Val string

4. **net.write(fd, data)** — Write to socket (non-blocking)
   - write() the string data
   - If would-block: return symbol `'would-block`
   - Return number of bytes written as Val int

5. **net.close(fd)** — Close socket
   - close(fd)
   - Return nil

### Headers needed
```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
```

## Part C: I/O Scheduler

### New process state: PROC_WAIT_IO in ta.h
```c
typedef enum { PROC_RUNNING, PROC_WAIT_RECV, PROC_WAIT_IO, PROC_DEAD } ProcState;
```

### I/O wait mechanism
- Add to Proc struct: `int wait_fd;` — the fd this process is waiting on
- When a C function returns the symbol `would-block`, the calling process should:
  - Save the fd it was waiting on
  - Transition to PROC_WAIT_IO state
  - The current instruction (OP_CCALL) needs to be RE-EXECUTED when the process wakes up

### Approach for would-block handling
The simplest approach: when net.accept/net.read/net.write returns `'would-block`, the **script** handles it:
```lisp
(define (accept-loop server)
  (let conn (net.accept server))
  (if (eq conn 'would-block)
      (accept-loop server)    ;; busy-wait (but with scheduler yield)
      (handle-client conn)))
```

This means we DON'T need PROC_WAIT_IO for the initial implementation. Instead:
- The scheduler's stall counter will catch infinite busy-waits (it resets when any process transitions state)
- Each call to net.accept etc. that returns would-block consumes one reduction
- The process gets re-enqueued, giving other processes a chance to run
- When the fd becomes ready, the next call succeeds

**BUT** this means CPU busy-waiting. For a real implementation, we need poll(). So:

### Better approach: poll() in the scheduler

1. Add to VM struct: `struct pollfd *poll_fds; int poll_count; int poll_cap;`
2. When a C function returns `'would-block`, the VM (in OP_CCALL handler) should:
   - Record the fd that caused the would-block (the C function can set `vm->last_wait_fd`)
   - Set process state to PROC_WAIT_IO
   - Store the fd in `p->wait_fd`
   - **Rewind PC** back to the OP_CCALL so it retries when woken
3. In vm_run, after running all ready processes:
   - Collect all PROC_WAIT_IO processes' fds
   - Call poll(fds, nfds, timeout)
   - For each fd that's ready (POLLIN), wake the corresponding process (set PROC_RUNNING, enqueue)
4. vm_run loop becomes:
```c
while (1) {
    // Run all ready processes
    while (rq_head != rq_tail) { ... }
    
    // Collect wait_io processes
    // If none waiting and none running → done
    // If some waiting → poll(), wake ready ones
    // If stall detected → kill remaining
}
```

### OP_CCALL modification for would-block
In vm_step, OP_CCALL handler:
```c
case OP_CCALL: {
    // ... existing setup ...
    Val result = vm->cfuncs[cfidx].fn(vm, args, nc);
    if (val_is_symbol(result) && strcmp(sym_name(vm, result), "would-block") == 0) {
        // Transition to WAIT_IO
        p->state = PROC_WAIT_IO;
        p->wait_fd = vm->last_wait_fd;
        // Rewind PC to re-execute this OP_CCALL
        p->pc = pc_start; // need to save pc at start of OP_CCALL
        return -1; // break out of reduction loop
    }
    proc_push(p, result);
    break;
}
```

## Verification

```bash
cd /Users/genius/project/tinyactor
make clean && make

# Module system test
./tinyactor test/scripts/module_test.lisp
# Expected output: hello from C\n7

# Existing tests must not break
pass=0; fail=0; for f in test/scripts/*.lisp; do
  name=$(basename "$f")
  out=$(timeout 10 ./tinyactor "$f" 2>&1)
  rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1))
  else fail=$((fail+1)); echo "FAIL($rc) $name: $out"
  fi
done
echo "PASS: $pass  FAIL: $fail"
# Expected: 44+ pass (all existing + module_test)

# Quick smoke test for net module (if echo server example exists)
# (will be tested in Task 4)
```

## Files to Create
1. `src/module.c` — vm_register_module implementation (~50 lines)
2. `src/net.c` — Network C module (~150 lines)
3. `test/scripts/module_test.lisp` — Module system test (~10 lines)

## Files to Modify
1. `ta.h` — TaModule/TaFunc types, modules array in VM, PROC_WAIT_IO state, wait_fd in Proc, poll support
2. `src/compile.c` — (import "name") handling (~20 lines)
3. `src/vm.c` — I/O scheduler loop, OP_CCALL would-block handling (~60 lines)
4. `src/main.c` or `src/api.c` — test module registration for testing (~20 lines)
5. `Makefile` — add module.o and net.o

## Estimated
- ~400 lines of new code
- ~80 lines of modifications