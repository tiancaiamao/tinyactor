/*
 * num.c — Numeric conversion module for TinyActor VM (issue #92)
 *
 * The strict numeric tower (issue #92) rejects int/float mixing at
 * typecheck time, so explicit representation conversions are required:
 *
 *   int.to_float(n)   -> Float   exact (int48 payload widened to double)
 *   float.to_int(d)   -> Int     truncates toward zero (like `/`)
 *
 * There is no existing VM primitive that unboxes a double payload back
 * into an int48 (str.to_int parses text, not floats), so these two
 * builtins are the minimal C-layer addition backing the lib-level
 * conversion functions.
 *
 * Out-of-range behavior for float.to_int: the double is truncated toward
 * zero, then wrapped modulo 2^48 into int48 two's-complement range — the
 * same normalization every int value undergoes when boxed (val_int).
 * NaN truncates to 0. +-Inf wraps like any out-of-range finite value.
 */

#include "ta.h"
#include <math.h>

static Val int_to_float(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_int(args[0]))
        return val_float(0.0);
    return val_float((double)val_get_int(args[0]));
}

static Val float_to_int(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_float(args[0]))
        return val_int(0);
    /* trunc() cuts toward zero without the UB of an out-of-range
     * double-to-int64 cast; fmod by 2^48 is exact (both operands are
     * exactly representable doubles), giving the int48 wrap. */
    double t = trunc(val_get_float(args[0]));
    if (isnan(t))
        return val_int(0);
    double m = fmod(t, 281474976710656.0); /* 2^48 */
    if (m >= 140737488355328.0)            /* >= 2^47: shift into negative range */
        m -= 281474976710656.0;
    return val_int((int64_t)m);
}

static TaFunc int_funcs[] = {{"to_float", int_to_float, 1}, {NULL, NULL, 0}};

static TaFunc float_funcs[] = {{"to_int", float_to_int, 1}, {NULL, NULL, 0}};

void vm_register_num_modules(VM *vm) {
    vm_register_module(vm, "int", int_funcs, 1);
    vm_register_module(vm, "float", float_funcs, 1);
}