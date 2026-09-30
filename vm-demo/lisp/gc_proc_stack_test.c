/* Positive counterpart of gc_root_test.c: the same 64 values pushed onto
 * p->sp (which the GC traces) all survive. Together the two tests pin the
 * invariant down from both sides: a C array loses values, the Proc stack
 * does not.
 *
 * Build (from the repo root), with tavm's objects already built:
 *   OBJ=$(ls src/ -1 | grep '\.o$' | grep -v '^tavm\.o$' | sed 's|^|src/|')
 *   cc -Wall -Wextra -std=c99 -O2 -I. -o /tmp/gc_proc_stack_test \
 *      vm-demo/lisp/gc_proc_stack_test.c $OBJ -lpthread \
 *      -L/opt/homebrew/opt/openssl@3/lib -lssl -lcrypto
 *
 * Expect: exit 0, "proc-stack-intact=64/64".
 */
#include "ta.h"
#include <stdio.h>
#include <string.h>

extern void vm_register_str_module(VM *vm);

#define N 64

int main(void) {
    VM *vm = vm_new();
    vm_register_str_module(vm);
    Proc *p = proc_new(vm);
    tls_current_proc = p;

    int cidx = vm_find_cfunc(vm, "str.concat");
    char big[64];
    memset(big, 'Z', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    /* Push the interpreter's live values onto p->sp — the only stack the
     * GC traces. */
    for (int i = 0; i < N; i++) {
        char buf[96];
        snprintf(buf, sizeof(buf), "held-%d-", i);
        size_t blen = strlen(buf);
        memcpy(buf + blen, big, sizeof(big) - 1);
        proc_push(p, val_string(p, buf, (int)(blen + sizeof(big) - 1)));
    }

    /* Allocation pressure: must trigger at least one collection. */
    Val args[2];
    args[0] = proc_peek(p, N - 1);
    args[1] = val_string(p, big, (int)strlen(big));
    Val joined = vm->cfuncs[cidx].fn(vm, args, 2);

    /* Pop back and verify each survived. */
    int bad = 0;
    for (int i = N - 1; i >= 0; i--) {
        Val v = proc_pop(p);
        if (val_tag(v) != TAG_STRING) {
            printf("FAIL [%d] tag=%llu\n", i, (unsigned long long)val_tag(v));
            bad++;
            continue;
        }
        const char *s = val_get_string(v)->data;
        char want[32];
        snprintf(want, sizeof(want), "held-%d-", i);
        if (strncmp(s, want, strlen(want)) != 0) {
            printf("FAIL [%d] %.24s\n", i, s);
            bad++;
        }
    }
    printf("concat=%s  proc-stack-intact=%d/%d\n", val_tag(joined) == TAG_STRING ? "ok" : "bad",
           N - bad, N);
    return bad != 0;
}