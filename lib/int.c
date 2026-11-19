/*
 * lib/int.c — the `int` module (built as lib/int.dylib / .so)
 *
 * One function: int.to_float, the exact widening of the int48 payload to a
 * double. The strict numeric tower (issue #92) rejects int/float mixing at
 * typecheck time, so crossing the boundary is always explicit; the arrow
 * types are compiler promises (lib/bootstrap/typecheck.ta) and need no TA
 * lift layer — hence a bare C module, lazy-loaded on the first int.* call
 * like str/math/random.
 *
 * The counterpart (float.to_int) lives in lib/float.c: the lazy loader
 * derives the dylib name from the module name's first segment, so one C
 * module per module name is the only layout that needs no exceptions.
 */

#include "ta.h"

/* A non-int argument is unreachable through the compiler's promise; typecheck
 * pins the parameter to Int. Fall back to 0.0 like the other C modules do. */
static Val int_to_float(VM *vm, Proc *p, Val *args, int nargs) {
    if (!val_is_int(args[0]))
        return val_float(0.0);
    return val_float((double)val_get_int(args[0]));
}

static TaFunc int_funcs[] = {{"to_float", int_to_float, 1}, {NULL, NULL, 0}};

/* Dynamic module entry: dlsym("vm_load_self") after dlopen (main.c link_unit). */
void vm_load_self(VM *vm) { vm_register_module(vm, "int", int_funcs, 1); }
