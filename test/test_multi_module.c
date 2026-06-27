/* test_multi_module.c — verify vm_load_tabc appends modules */
#include "ta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int vm_load_tabc(VM *vm, const char *path);

int main(int argc, char **argv) {
    VM *vm = vm_new();

    /* Load module A — should populate code/fn_table/symbols */
    if (vm_load_tabc(vm, "test/multi_mod_a.tabc") != 0) {
        fprintf(stderr, "FAIL: load A\n");
        return 1;
    }
    int a_fn_count = vm->fn_count;
    int a_code_len = vm->code_len;
    int a_sym_count = vm->sym_count;
    int a_top = vm->top_fn_id;
    printf("After A: fn_count=%d code_len=%d sym_count=%d top_fn_id=%d\n",
           a_fn_count, a_code_len, a_sym_count, a_top);

    /* Load module A AGAIN — should append, rebasing fn_ids/code */
    if (vm_load_tabc(vm, "test/multi_mod_a.tabc") != 0) {
        fprintf(stderr, "FAIL: load A again\n");
        return 1;
    }
    int b_fn_count = vm->fn_count;
    int b_code_len = vm->code_len;
    int b_sym_count = vm->sym_count;
    int b_top = vm->top_fn_id;
    printf("After A+A: fn_count=%d code_len=%d sym_count=%d top_fn_id=%d\n",
           b_fn_count, b_code_len, b_sym_count, b_top);

        /* Verify append semantics.
     * Note: the .tabc contains n_symbols symbols; each load appends that
     * many.  The first load starts from vm_new()'s pre-interned count,
     * so we check the *delta* is consistent across loads, not that the
     * total doubles. */
    int sym_delta_first  = a_sym_count - 42;  /* 42 pre-interned by vm_new */
    int sym_delta_second = b_sym_count - a_sym_count;
    int ok = 1;
    if (b_fn_count != a_fn_count * 2) {
        fprintf(stderr, "FAIL: fn_count should double (%d vs %d)\n",
                b_fn_count, a_fn_count * 2);
        ok = 0;
    }
    if (b_code_len != a_code_len * 2) {
        fprintf(stderr, "FAIL: code_len should double (%d vs %d)\n",
                b_code_len, a_code_len * 2);
        ok = 0;
    }
    if (sym_delta_first != sym_delta_second) {
        fprintf(stderr, "FAIL: sym delta inconsistent (%d vs %d)\n",
                sym_delta_first, sym_delta_second);
        ok = 0;
    }
    if (b_top != a_top + a_fn_count) {
        fprintf(stderr, "FAIL: top_fn_id should rebase by fn_base (%d vs %d)\n",
                b_top, a_top + a_fn_count);
        ok = 0;
    }

    /* Verify fn_table[second top] points into the second code region */
    int second_entry = vm->fn_table[b_top];
    int first_entry  = vm->fn_table[a_top];
    if (second_entry != first_entry + a_code_len) {
        fprintf(stderr, "FAIL: fn_table entry not rebased by code_base "
                "(%d vs %d + %d = %d)\n",
                second_entry, first_entry, a_code_len,
                first_entry + a_code_len);
        ok = 0;
    }

    /* Now run the SECOND module's top-level thunk — it should print 42.
     * The second module's top_fn_id is b_top; its bytecode was rebased
     * so jump targets and fn_ids point into the combined code space.   */
    vm->top_fn_id = b_top;
    vm_spawn(vm, b_top);
    vm_run(vm);

    if (ok) {
        printf("PASS: multi-module append + rebase verified\n");
        return 0;
    } else {
        printf("FAIL\n");
        return 1;
    }
}