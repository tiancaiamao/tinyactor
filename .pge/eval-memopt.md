# Eval Report: Actor Memory Optimization (memopt)

**Evaluator:** Independent Validator  
**Date:** 2025-01-28  
**Result: ⚠️ CONDITIONAL FAIL** — Structural goals met, but behavioral regressions introduced

---

## L1 — Structural Criteria

### ✅ `make` builds without errors or warnings
- Build succeeds. All warnings are pre-existing in non-memopt files (reader.c, reader_ta.c, compile.c, api.c, net.c — unused functions/parameters). Zero warnings from memopt-modified files (vm.c, gc.c, ta.h, api.c changes).

### ✅ `sizeof(Proc)` is significantly smaller than before (was 2416, target < 500)
- **sizeof(Proc) = 352 bytes** — exceeds target by 29%. 85% reduction from 2416.
- Achieved by: lazy heap (mem/gc_to now NULL until first use), gc_roots pointer instead of inline[256], lazy watchers.

### ✅ `MAX_PROCS` is set to 1048576
- `#define MAX_PROCS (1024 * 1024)` confirmed in ta.h:43.

### ✅ `gc_roots` is a pointer (not inline array) in Proc struct
- `Val *gc_roots;` confirmed at ta.h:163. Dynamically grown via realloc in gc_root_push (starts at cap=32, doubles on overflow).

### ✅ `proc_new()` sets mem=NULL, gc_to=NULL
- Confirmed in src/vm.c:296-302. Also sets gc_roots=NULL, gc_roots_cap=0, watchers=NULL, watcher_cap=0.

### ✅ `proc_die()` frees mem, gc_to, gc_roots after sending DOWN messages
- Confirmed in src/vm.c:375-388. DOWN messages are sent first (loop at line 343), then mailbox fragments freed, then mem/gc_to/gc_roots freed. Sets all to NULL after free.

### ✅ `proc_ensure_heap()` exists and allocates 4KB initial heap
- Defined at ta.h:431. Allocates `mem_size = 4096`, `mem = calloc(1, 4096)`, `gc_to = calloc(1, 4096)`.

**L1 Score: 7/7 ✅**

---

## L2 — Behavioral Criteria

### ⚠️ All non-flaky tests pass (171+/174)
- **170/174 pass** in `make test`. Target was 171+/174.
- **FAIL: 4 tests fail:**
  1. `error-send-to-dead.ta` — NO OUTPUT (flaky, ~20% failure rate)
  2. `monitor_test.ta` — NO OUTPUT (flaky, ~20% failure rate)
  3. `bootstrap echo_test.ta` — TIMEOUT (100% failure with memopt)
  4. `bytecode-cmp echo_test.ta` — output differs (consequence of #3)

- **Critical finding: This is a REGRESSION.** Verified by stashing memopt changes and rebuilding:
  - `error-send-to-dead.ta`: **10/10 pass** pre-memopt, **8/10 pass** post-memopt
  - `monitor_test.ta`: **10/10 pass** pre-memopt, **8/10 pass** post-memopt
  - `echo_test.ta`: **2/3 pass** pre-memopt, **0/5 pass** post-memopt (100% timeout)

- **Root cause:** Lazy heap allocation makes `proc_new()` significantly faster (no 4KB×2 calloc). Spawned child processes start running sooner and die before the parent calls `monitor()`. Since `OP_MONITOR` doesn't handle the case where the target is already dead (should immediately deliver a DOWN message), the DOWN message is lost and `recv()` blocks forever.

### ✅ GC tests still pass
- All GC tests pass individually (10s timeout each):
  - `gc-closure-churn.ta` → "42" ✅
  - `gc-deep-list.ta` → "2000" ✅
  - `gc-string-churn.ta` → "hello world" ✅
  - `gc-multi-process-stress.ta` → "all done" ✅

### ⚠️ Actor tests still pass (ping_pong, monitor_test, error-supervisor-restart, etc.)
- All pass individually:
  - `ping_pong.ta` → "ping done" ✅
  - `error-supervisor-restart.ta` → "worker died" ✅
  - `actor-ping-pong-stress.ta` → "done" ✅
  - `actor-selective-recv.ta` → "got important" ✅
  - `monitor_test.ta` → "DOWN received" ✅ (but flaky ~20% of the time)
- **monitor_test is flaky** — passes individually most of the time but fails ~20% due to the same race condition as error-send-to-dead.

### ✅ No crash or ASAN error on error-send-to-dead test
- Ran `./tinyactor test/scripts/error-send-to-dead.ta` 10 times. All exited with code 0. No SIGSEGV (139), no SIGFPE, no crash. (Note: 2/10 runs produced no output due to the race condition described above, but no crashes.)
- ASAN is not enabled in the default build (`-fsanitize=address` not in CFLAGS), so ASAN errors cannot be detected. The build should optionally support ASAN for thorough memory safety verification.

**L2 Score: 1/4 full ✅, 3/4 ⚠️**

---

## Code Quality Review

### Memory Safety Issues Found

1. **Missing malloc/realloc failure checks in `gc_root_push` (ta.h:412-418)**
   ```c
   p->gc_roots = malloc(p->gc_roots_cap * sizeof(Val));  // no NULL check
   p->gc_roots = realloc(p->gc_roots, p->gc_roots_cap * sizeof(Val));  // no NULL check
   ```
   If allocation fails, NULL pointer dereference on next push. Low severity (unlikely on modern systems with overcommit).

2. **Pre-existing race condition on watchers (worsened by memopt)**
   - `proc_die` reads `p->watchers[i]` without synchronization.
   - `OP_MONITOR` on another thread may `realloc(p->watchers)` concurrently.
   - This can cause use-after-free if realloc moves the buffer during iteration.
   - The memopt change made this race more likely by making watchers lazily allocated (NULL→realloc(4) on first monitor).
   - **Not fixed by memopt.** Comment acknowledges it: "watchers/watcher_refs are NOT freed here — another thread may be concurrently in OP_MONITOR accessing them."

3. **OP_MONITOR doesn't handle already-dead targets**
   - If the target process is PROC_DEAD, OP_MONITOR adds the watcher but never sends a DOWN message.
   - This is a pre-existing bug but the memopt changes made it trigger more frequently.
   - **Fix:** In OP_MONITOR, check `if (t->state == PROC_DEAD)` and immediately deliver a DOWN message instead of (or in addition to) adding to watcher list.

### Positive Observations
- `gc_fixup_heap_pointers` correctly handles pointer relocation after `realloc` in both `proc_grow` and `gc_collect` swap-grow paths. Walks heap objects, stack Vals, and gc_roots.
- `gc_collect` properly guards against NULL heap (`if (p->mem == NULL) return;`).
- `vm_free` properly frees gc_roots alongside mem, gc_to, watchers, watcher_refs.
- Scratch Proc in `vm_load`/`vm_load_ta`/`vm_load_source_fn` properly frees gc_roots.

---

## Summary

| Category | Score |
|----------|-------|
| L1 Structural | 7/7 ✅ |
| L2 Behavioral | 1/4 ✅, 3/4 ⚠️ |
| **Overall** | **CONDITIONAL FAIL** |

### Verdict: ⚠️ CONDITIONAL FAIL

The structural goals are fully met — sizeof(Proc) dropped from 2416 to 352 bytes (85% reduction), lazy allocation works correctly, GC pointer fixup is sound. However, the optimization introduced behavioral regressions:

1. **error-send-to-dead.ta and monitor_test.ta became flaky** (~20% failure, was 0% pre-memopt)
2. **echo_test.ta became 100% timeout** (was ~33% pre-memopt)

### Required Fixes for PASS

1. **Add dead-process handling in OP_MONITOR** (src/vm.c ~line 1138): When `t->state == PROC_DEAD`, immediately deliver a DOWN message to the monitor caller instead of silently adding to watcher list. This fixes error-send-to-dead and monitor_test flakiness.

2. **Investigate echo_test timeout regression**: The 0%→100% timeout regression suggests the lazy allocation or faster spawning breaks network actor timing. May need debugging of the echo actor's startup sequence.

3. **Add malloc failure checks** in gc_root_push (minor).

### Issues for Generator

**Issue 1: OP_MONITOR dead-target race (CRITICAL)**
File: `src/vm.c`, OP_MONITOR case (~line 1138)
```c
// Current: blindly adds watcher even if target is dead
if (t) {
    // ... adds to watcher list ...
}
// Should also check:
//   if (t->state == PROC_DEAD) { deliver immediate DOWN msg }
```

**Issue 2: echo_test 100% timeout (CRITICAL)**
echo_test.ta times out every time with memopt changes. Was intermittent pre-memopt. Need to investigate if lazy heap allocation breaks network/IO actor initialization timing.

**Issue 3: gc_root_push missing NULL checks (MINOR)**
File: `ta.h:412-418`. Add NULL checks after malloc/realloc.