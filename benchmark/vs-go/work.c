/*
 * The CPU work behind GET /work/<n>. Benchmark code, not stdlib: built by
 * `make` in this directory into lib/work.dylib, which the VM dlopens as
 * lib/<mod>.<ext> relative to CWD.
 *
 * work_step_c(seed, passes) -> uint64
 *   `passes` read-modify-write passes over a 1024-entry uint32 array seeded
 *   from `seed`, folded into a 47-bit checksum (int48 range, never negative).
 *
 * The state is rebuilt from `seed` on every call, so a call is independent of
 * the one before it and the CALLER decides how much work one call does. That
 * is the chunking knob: an unchunked native call is an unschedulable black box
 * for as long as it runs — which is what a cgo call is to a Go P.
 *
 * The function is exported (not static) so the Go server can call the very
 * same code through cgo: identical kernel on both sides, so a difference in
 * the numbers is a difference in the runtime, not in the workload.
 *
 * Element-parallel, so the compiler vectorizes the inner loop. No static
 * state: the working set is on the calling worker's stack.
 */
#include "ta.h"

#define EW_N 1024 /* u32 entries per pass; 4KB working set, L1-resident */

uint64_t work_step_c(uint64_t seed, int64_t passes) {
    if (passes < 0)
        passes = 0;

    uint32_t o[EW_N];
    uint32_t s = (uint32_t)seed * 2654435761u + 1u;
    for (int j = 0; j < EW_N; j++) {
        s = s * 1664525u + 1013904223u;
        o[j] = s;
    }

    for (int64_t p = 0; p < passes; p++)
        for (int j = 0; j < EW_N; j++)
            o[j] = o[j] * 1664525u + 1013904223u;

    uint64_t h = 0x9E3779B97F4A7C15ULL + seed + (uint64_t)passes;
    for (int j = 0; j < EW_N; j++)
        h += o[j];
    h = (h ^ (h >> 29)) * 0xff51afd7ed558ccdULL;
    return h & 0x7FFFFFFFFFFFULL; /* 47 bits: fits int48, >= 0 */
}

static Val work_step(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_int(args[0]) || !val_is_int(args[1]))
        return val_int(0);
    return val_int((int64_t)work_step_c((uint64_t)val_get_int(args[0]), val_get_int(args[1])));
}

static TaFunc work_funcs[] = {{"step", work_step, 2}, {NULL, NULL, 0}};

/* Dynamic module entry: dlsym("vm_load_self") after dlopen. */
void vm_load_self(VM *vm) { vm_register_module(vm, "work", work_funcs, 1); }