/*
 * lib/compute.c — CPU-bound compute kernel module for TinyActor.
 *
 * Exports compute.kernel(n): n rounds of a deterministic 64-bit integer mix,
 * returning a 47-bit checksum (always inside TA's int48 range, never negative).
 * The hot loop lives in C so a TA caller's raw speed matches compiled Go; the
 * point is that the TA loop wrapping it (lib/compute.ta burn) still pays a VM
 * reduction every few instructions and therefore yields. A native call made
 * from TA is not an unschedulable black box the way a cgo call is — the actor
 * between two calls crosses the reduction budget and goes back on the runq.
 *
 * Built as lib/compute.dylib (Makefile COMPUTE_MODS) and dlopen'd lazily on
 * the first compute.* reference (the lib/math.c pattern, docs/c-module.md). A
 * statically registered "compute" module name would make `import compute` a
 * no-op and silently drop lib/compute.ta.
 */

#include "ta.h"

static Val compute_kernel(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_int(args[0]))
        return val_int(0);
    int64_t n = val_get_int(args[0]);
    if (n < 0)
        n = 0;
    uint64_t x = (uint64_t)n * 0x9E3779B97F4A7C15ULL + 1;
    for (int64_t i = 0; i < n; i++) {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 29;
        x += (uint64_t)i;
    }
    return val_int((int64_t)(x & 0x7FFFFFFFFFFFULL)); /* 47 bits: fits int48, >= 0 */
}

static TaFunc compute_funcs[] = {{"kernel", compute_kernel, 1}, {NULL, NULL, 0}};

/* Dynamic module entry: dlsym("vm_load_self") after dlopen (main.c link_unit). */
void vm_load_self(VM *vm) { vm_register_module(vm, "compute", compute_funcs, 1); }