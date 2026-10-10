/* Regression test for the embedding invariant: TA's GC only traces p->sp,
 * so tavm's eval stack MUST be the Proc stack, not a C array.
 *
 * This test asserts the NEGATIVE case (a C array loses values) on purpose:
 * if it ever starts passing, the C array became GC-visible and the premise
 * of README's "eval stack must be p->sp" needs revisiting.
 *
 * Build (from the repo root), with tavm's objects already built:
 *   OBJ=$(ls src/ -1 | grep '\.o$' | grep -v '^tavm\.o$' | sed 's|^|src/|')
 *   cc -Wall -Wextra -std=c99 -O2 -I. -o /tmp/gc_root_test \
 *      test/backend/gc_root_test.c $OBJ -lpthread \
 *      -L/opt/homebrew/opt/openssl@3/lib -lssl -lcrypto
 *
 * Expect: exit 1, "held-intact" < 64 (C-array values ARE collected).
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

    /* Values held ONLY in a C array — invisible to the GC, exactly like
     * tavm's `stack`. */
    Val held[N];
    char big[64];
    memset(big, 'Z', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    for (int i = 0; i < N; i++) {
        char buf[80];
        snprintf(buf, sizeof(buf), "held-%d-", i);
        int len = (int)strlen(buf);
        memcpy(buf + len, big, sizeof(big) - 1);
        held[i] = val_string(p, buf, (int)strlen(buf));
    }

    /* One str.concat call, with a long string to force allocation pressure. */
    Val args[2];
    args[0] = held[0];
    args[1] = val_string(p, big, (int)strlen(big));
    Val joined = vm->cfuncs[cidx].fn(vm, args, 2);

    /* Now check whether the C-array-held values survived and are intact. */
    int bad = 0;
    for (int i = 0; i < N; i++) {
        if (val_tag(held[i]) != TAG_STRING) {
            printf("FAIL held[%d] tag=%llu not string\n", i, (unsigned long long)val_tag(held[i]));
            bad++;
            continue;
        }
        const char *s = val_get_string(held[i])->data;
        char want[32];
        snprintf(want, sizeof(want), "held-%d-", i);
        if (strncmp(s, want, strlen(want)) != 0) {
            printf("FAIL held[%d] corrupted: %.24s...\n", i, s);
            bad++;
        }
    }
    printf("concat ok=%d  held-intact=%d/%d\n", val_tag(joined) == TAG_STRING, N - bad, N);
    return bad != 0;
}