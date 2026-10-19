/*
 * benchmark/vs-go/work.c — the CPU-bound computation behind /work/<n>.
 *
 * This is the benchmark's own code, NOT part of the standard library: it is
 * built by benchmark/vs-go/run.sh into <workdir>/lib/work.dylib, and both
 * servers run with that workdir as their CWD. (The VM resolves a C module as
 * lib/<mod>.<ext> relative to CWD, so a working directory of its own is all a
 * benchmark needs to carry its own native module.)
 *
 *   work.step(seed, passes) -> Int
 *     `passes` read-modify-write passes over a 1024-entry uint32 state array
 *     seeded from `seed`, folded into a 47-bit checksum (47 bits: always
 *     inside TA's int48 range, never negative).
 *
 * Two properties the workload is built on:
 *
 *  1. Element-parallel. Each o[j] evolves independently of the others, so a C
 *     compiler packs the inner loop into SIMD lanes. The identical loop in
 *     server.go stays scalar — same source, same seed, same pass count, ~4x
 *     apart.
 *
 *  2. Decomposable. The state is rebuilt from `seed` on every call, and the
 *     TA side gives each chunk its own seed (seed+i, see work.ta), so a call
 *     never depends on the call before it: the *caller* decides how much work
 *     one call does. That is the chunking knob, and it belongs to the caller
 *     because a native call with no such knob is exactly what a cgo call is —
 *     an unschedulable black box for as long as it runs.
 *
 * No static state: the working set lives on the calling worker's stack, so
 * two actors on two workers never share anything.
 */

#include "ta.h"

#define EW_N 1024 /* u32 entries per pass; 4KB working set, L1-resident */

static Val work_step(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_int(args[0]) || !val_is_int(args[1]))
        return val_int(0);
    int64_t seed = val_get_int(args[0]);
    int64_t passes = val_get_int(args[1]);
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

    uint64_t h = 0x9E3779B97F4A7C15ULL + (uint64_t)seed + (uint64_t)passes;
    for (int j = 0; j < EW_N; j++)
        h += o[j];
    h = (h ^ (h >> 29)) * 0xff51afd7ed558ccdULL;
    return val_int((int64_t)(h & 0x7FFFFFFFFFFFULL)); /* 47 bits: fits int48, >= 0 */
}

static TaFunc work_funcs[] = {{"step", work_step, 2}, {NULL, NULL, 0}};

/* Dynamic module entry: dlsym("vm_load_self") after dlopen (main.c link_unit). */
void vm_load_self(VM *vm) { vm_register_module(vm, "work", work_funcs, 1); }