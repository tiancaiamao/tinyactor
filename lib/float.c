/*
 * lib/float.c — the `float` module (built as lib/float.dylib / .so)
 *
 * One function: float.to_int. No VM primitive unboxes a double payload back
 * into an int48 (str.to_int parses text, not floats), so this is the minimal
 * C-layer addition backing the tower's downhill conversion (issue #92).
 *
 * Out-of-range behavior: the double is truncated toward zero, then wrapped
 * modulo 2^48 into the int48 two's-complement range — the same normalization
 * every int value undergoes when boxed (val_int). NaN truncates to 0; +-Inf
 * wraps like any other out-of-range finite value.
 */

#include "ta.h"
#include <math.h>

static Val float_to_int(VM *vm, Proc *p, Val *args, int nargs) {
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

static TaFunc float_funcs[] = {{"to_int", float_to_int, 1}, {NULL, NULL, 0}};

/* Dynamic module entry: dlsym("vm_load_self") after dlopen (main.c link_unit). */
void vm_load_self(VM *vm) { vm_register_module(vm, "float", float_funcs, 1); }
