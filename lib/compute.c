/*
 * lib/compute.c — CPU-bound compute kernel module for TinyActor.
 *
 * Exports compute.kernel(passes): `passes` read-modify-write passes over a
 * 1024-entry uint32 state array built on the calling actor's stack, folded
 * into a 47-bit checksum (always inside TA's int48 range, never negative).
 *
 * The state array is on the stack, so two actors running on two workers never
 * share it — no static state, no lock, no init race.
 *
 * The hot loop is element-parallel (each o[j] evolves independently of the
 * others), so a C compiler packs it into SIMD lanes while a Go compiler emits
 * scalar code: same source, ~4x apart. That is the point of the module — see
 * benchmark/vs-go, where the heavy request is this kernel and the comparison
 * is against the identical loop compiled by the Go toolchain.
 *
 * The caller-side loop (lib/compute.ta burn) is what keeps the actor
 * preemptible. The VM charges its reduction budget on control transfers, so a
 * burn actor yields every ~500 kernel calls no matter how much each call
 * computes. A native call made from TA is therefore not an unschedulable black
 * box the way a cgo call is: the actor between two calls crosses the reduction
 * budget and goes back on the runq.
 *
 * Built as lib/compute.dylib (Makefile COMPUTE_MODS) and dlopen'd lazily on
 * the first compute.* reference (the lib/math.c pattern, docs/c-module.md). A
 * statically registered "compute" module name would make `import compute` a
 * no-op and silently drop lib/compute.ta.
 */

#include "ta.h"

#define EW_N 1024 /* u32 entries per pass; 4KB working set, L1-resident */

static Val compute_kernel(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_int(args[0]))
        return val_int(0);
    int64_t passes = val_get_int(args[0]);
    if (passes < 0)
        passes = 0;

    uint32_t o[EW_N];
    for (int j = 0; j < EW_N; j++)
        o[j] = (uint32_t)j * 0x9E3779B9u + 1u;

    for (int64_t p = 0; p < passes; p++)
        for (int j = 0; j < EW_N; j++)
            o[j] = o[j] * 1664525u + 1013904223u;

    uint64_t h = 0x9E3779B97F4A7C15ULL + (uint64_t)passes;
    for (int j = 0; j < EW_N; j++)
        h += o[j];
    h = (h ^ (h >> 29)) * 0xff51afd7ed558ccdULL;
    return val_int((int64_t)(h & 0x7FFFFFFFFFFFULL)); /* 47 bits: fits int48, >= 0 */
}

static TaFunc compute_funcs[] = {{"kernel", compute_kernel, 1}, {NULL, NULL, 0}};

/* Dynamic module entry: dlsym("vm_load_self") after dlopen (main.c link_unit). */
void vm_load_self(VM *vm) { vm_register_module(vm, "compute", compute_funcs, 1); }