#include "ta.h"

static Val template_answer(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    return val_int(42);
}

static TaFunc template_funcs[] = {
    {"answer", template_answer, 0},
    {NULL, NULL, 0},
};

void vm_load_self(VM *vm) { vm_register_module(vm, "template", template_funcs, 1); }