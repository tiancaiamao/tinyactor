/*
 * lib/math.c — C math module for TinyActor (built as lib/math.dylib / .so)
 *
 * Backs the external fn declarations in lib/math.ta. It follows the demo
 * module pattern (docs/c-module.md): lazily dlopen'd on the first call of
 * any math.* cfunc, so `import math` keeps resolving lib/math.ta as a
 * strict TA module (a statically registered "math" module name would make
 * the import a no-op and silently drop lib/math.ta).
 *
 * Float results are IEEE doubles (TA `float`). floor/ceil return TA ints,
 * normalized into int48 the same way as src/num.c float.to_int (fmod 2^48
 * wrap — no out-of-range int64 cast UB).
 *
 * Negative-input contract (documented in lib/math.ta): sqrt/log of a
 * negative argument return IEEE NaN. NaN != NaN and every comparison
 * against it is false; there is no error signal. exp/pow overflow
 * returns +-Inf. A non-float argument (unreachable when imported, since
 * typecheck enforces the declared signatures) returns NaN for float
 * results and 0 for int results.
 */

#include "ta.h"
#include <math.h>

/* double -> boxed int48, same normalization as src/num.c float_to_int. */
static Val d_to_i48(double t) {
    if (isnan(t))
        return val_int(0);
    double m = fmod(t, 281474976710656.0); /* 2^48 */
    if (m >= 140737488355328.0)            /* >= 2^47: shift into negative range */
        m -= 281474976710656.0;
    return val_int((int64_t)m);
}

static Val math_floor(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_float(args[0]))
        return val_int(0);
    return d_to_i48(floor(val_get_float(args[0])));
}

static Val math_ceil(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_float(args[0]))
        return val_int(0);
    return d_to_i48(ceil(val_get_float(args[0])));
}

static Val math_sqrt(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_float(args[0]))
        return val_float(0.0 / 0.0);
    return val_float(sqrt(val_get_float(args[0])));
}

static Val math_log(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_float(args[0]))
        return val_float(0.0 / 0.0);
    return val_float(log(val_get_float(args[0])));
}

static Val math_exp(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_float(args[0]))
        return val_float(0.0 / 0.0);
    return val_float(exp(val_get_float(args[0])));
}

static Val math_pow(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_float(args[0]) || !val_is_float(args[1]))
        return val_float(0.0 / 0.0);
    return val_float(pow(val_get_float(args[0]), val_get_float(args[1])));
}

static Val math_sin(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_float(args[0]))
        return val_float(0.0 / 0.0);
    return val_float(sin(val_get_float(args[0])));
}

static Val math_cos(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_float(args[0]))
        return val_float(0.0 / 0.0);
    return val_float(cos(val_get_float(args[0])));
}

static Val math_tan(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_float(args[0]))
        return val_float(0.0 / 0.0);
    return val_float(tan(val_get_float(args[0])));
}

static Val math_atan(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_float(args[0]))
        return val_float(0.0 / 0.0);
    return val_float(atan(val_get_float(args[0])));
}

static Val math_clamp(VM *vm, Val *args, int nargs) {
    (void)vm;
    if (!val_is_float(args[0]) || !val_is_float(args[1]) || !val_is_float(args[2]))
        return val_float(0.0 / 0.0);
    double lo = val_get_float(args[0]);
    double hi = val_get_float(args[1]);
    double x = val_get_float(args[2]);
    if (x < lo)
        return val_float(lo);
    if (x > hi)
        return val_float(hi);
    return val_float(x);
}

static TaFunc math_funcs[] = {
    {"floor", math_floor, 1}, {"ceil", math_ceil, 1},   {"sqrt", math_sqrt, 1},
    {"log", math_log, 1},     {"exp", math_exp, 1},     {"pow", math_pow, 2},
    {"sin", math_sin, 1},     {"cos", math_cos, 1},     {"tan", math_tan, 1},
    {"atan", math_atan, 1},   {"clamp", math_clamp, 3}, {NULL, NULL, 0}};

/* Dynamic module entry: dlsym("vm_load_self") after dlopen (vm.c). */
void vm_load_self(VM *vm) { vm_register_module(vm, "math", math_funcs, 11); }