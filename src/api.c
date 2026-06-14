/*
 * api.c — Public C API for TinyActor
 */

#include "ta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Provided by reader.c / compile.c (not in ta.h) */
extern Val reader_read(VM *vm, const char *src, int *pos);
extern Val reader_ta_read(VM *vm, const char *src, int *pos);
extern int  compile_all(VM *vm, Val forms);

/* ============================================================
 * Symbol interning
 * ============================================================ */

int vm_intern_symbol(VM *vm, const char *name) {
    for (int i = 0; i < vm->sym_count; i++) {
        if (strcmp(vm->symbols[i], name) == 0) return i;
    }
    if (vm->sym_count >= vm->sym_cap) {
        vm->sym_cap = vm->sym_cap ? vm->sym_cap * 2 : 64;
        vm->symbols = realloc(vm->symbols, vm->sym_cap * sizeof(char *));
    }
    vm->symbols[vm->sym_count] = strdup(name);
    return vm->sym_count++;
}

/* ============================================================
 * VM lifecycle
 * ============================================================ */

VM *vm_new(void) {
    VM *vm = calloc(1, sizeof(VM));

        /* Run queue */
    vm->rq_cap  = 256;
    vm->runq    = malloc(vm->rq_cap * sizeof(int));
    vm->rq_head = 0;
    vm->rq_tail = 0;
    atomic_init(&vm->rq_count, 0);
    pthread_mutex_init(&vm->rq_lock, NULL);
    pthread_cond_init(&vm->rq_cond, NULL);

    /* Process table — pre-allocated to MAX_PROCS */
    vm->procs_cap   = MAX_PROCS;
    vm->procs       = calloc(MAX_PROCS, sizeof(Proc *));
    vm->procs_count = 0;
    atomic_init(&vm->next_pid, 0);
        atomic_init(&vm->active_procs, 0);
    atomic_init(&vm->busy_workers, 0);

    /* Threading */
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    vm->nworkers = (ncpu > 0) ? (int)ncpu : 1;
    vm->stop     = 0;

    /* Symbol table — pre-intern all language keywords and builtins */
    vm->sym_cap   = 128;
    vm->symbols   = malloc(vm->sym_cap * sizeof(char *));
    vm->sym_count = 0;

    static const char * const keywords[] = {
        "quote", "define", "lambda", "if", "begin", "let", "letrec",
        "match", "spawn", "send", "recv", "self", "monitor",
        "cons", "car", "cdr",
        "+", "-", "*", "/", "%",
        "=", "<", "<=", ">", ">=",
        "null?", "pair?", "int?", "string?", "bytes?", "pid?", "print",
        "true", "false", "DOWN", "nil", "_",
        "and", "or", "not", "set!",
        NULL
    };
    for (int i = 0; keywords[i]; i++)
        vm_intern_symbol(vm, keywords[i]);

    return vm;
}

void vm_free(VM *vm) {
            for (int i = 0; i < vm->procs_cap; i++) {
        Proc *p = vm->procs[i];
        if (!p) continue;
        /* free any undelivered message fragments */
        MsgFragment *frag = p->mbox_frag_head;
        while (frag) {
            MsgFragment *nx = frag->next;
            free(frag);
            frag = nx;
        }
        pthread_mutex_destroy(&p->mbox_lock);
        free(p->mem);
        free(p->mbox);  /* legacy field, always NULL — no-op */
        free(p->watchers);
        free(p->watcher_refs);
        free(p->gc_to);
        free(p);
    }
    free(vm->procs);
    pthread_mutex_destroy(&vm->rq_lock);
    pthread_cond_destroy(&vm->rq_cond);
    free(vm->workers);
    free(vm->code);
    free(vm->fn_table);
    for (int i = 0; i < vm->sym_count; i++)
        free(vm->symbols[i]);
    free(vm->symbols);
    free(vm->runq);
    for (int i = 0; i < vm->cfunc_count; i++)
        free(vm->cfuncs[i].name);
    for (int i = 0; i < vm->mod_count; i++)
        free(vm->mod_names[i]);
    free(vm->mod_names);
    free(vm->mod_funcs);
    free(vm->mod_nfuncs);
    free(vm);
}

/* ============================================================
 * C function registration
 * ============================================================ */

void vm_register(VM *vm, const char *name,
                 Val (*fn)(VM *vm, Val *args, int nargs), int nargs) {
    if (vm->cfunc_count >= MAX_CFUNCS) return;
    char *dup = strdup(name);
    if (!dup) return;
    vm->cfuncs[vm->cfunc_count].name  = dup;
    vm->cfuncs[vm->cfunc_count].fn    = fn;
    vm->cfuncs[vm->cfunc_count].nargs = nargs;
    vm->cfunc_count++;
}

/* ============================================================
 * Loading
 * ============================================================ */

int vm_load(VM *vm, const char *src) {
    int pos = 0;
    int len = (int)strlen(src);

    /* Scratch proc for building the forms list (pair allocation) */
    Proc scratch;
    memset(&scratch, 0, sizeof(Proc));
    scratch.mem_size = 32768;
    scratch.mem      = malloc(scratch.mem_size);
    scratch.sp       = 0;

    Val forms  = val_nil();
    Val *tail  = &forms;

    while (pos < len) {
        while (pos < len && (src[pos] == ' '  || src[pos] == '\n' ||
                             src[pos] == '\t' || src[pos] == '\r'))
            pos++;
        if (pos >= len) break;

        int old_pos = pos;
        Val form = reader_read(vm, src, &pos);
        if (pos == old_pos) break;

        Val cell = val_pair(&scratch, form, val_nil());
        *tail = cell;
        tail  = &((HeapPair *)val_as_pair(cell))->cdr;
    }

        int rc = compile_all(vm, forms);

    free(scratch.mem);
    return rc;
}

int vm_load_ta(VM *vm, const char *src) {
    int pos = 0;
    int len = (int)strlen(src);
    Proc scratch;
    memset(&scratch, 0, sizeof(Proc));
    scratch.mem_size = 32768;
    scratch.mem      = malloc(scratch.mem_size);
    scratch.sp       = 0;
    Val forms  = val_nil();
    Val *tail  = &forms;
    while (pos < len) {
        while (pos < len && (src[pos] == ' '  || src[pos] == '\n' ||
                             src[pos] == '\t' || src[pos] == '\r'))
            pos++;
        if (pos >= len) break;
        int old_pos = pos;
        Val form = reader_ta_read(vm, src, &pos);
        if (pos == old_pos) break;
        Val cell = val_pair(&scratch, form, val_nil());
        *tail = cell;
        tail  = &((HeapPair *)val_as_pair(cell))->cdr;
    }
    int rc = compile_all(vm, forms);
    free(scratch.mem);
    return rc;
}

int vm_load_file(VM *vm, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
        fclose(f);

    int len = (int)strlen(path);
    if (len >= 3 && strcmp(path + len - 3, ".ta") == 0) {
        int rc = vm_load_ta(vm, buf);
        free(buf);
        return rc;
    }

    int rc = vm_load(vm, buf);
    free(buf);
    return rc;
}

/* ============================================================
 * REPL / eval
 * ============================================================ */

Val vm_eval(VM *vm, const char *src) {
    /* Pass source directly to vm_load without (begin ...) wrapping.
     * Wrapping in begin hides (define ...) forms from compile_all's
     * function scanner (phase 1), causing function calls to crash. */
    if (vm_load(vm, src) != 0)
        return val_nil();

    /* Patch trailing OP_POP OP_PUSH_NIL OP_HALT → OP_DUP OP_POP OP_HALT
     * so the expression result stays on the dead process's stack. */
    {
                int top_fn = vm->top_fn_id;
        int entry  = vm->fn_table[top_fn];
        int last   = -1;
        for (int i = entry; i < entry + 65536; i++) {
            if (vm->code[i] == OP_POP &&
                vm->code[i + 1] == OP_PUSH_NIL &&
                vm->code[i + 2] == OP_HALT)
                last = i;
            if (vm->code[i] == OP_HALT && last >= 0) break;
        }
        if (last >= 0) {
            vm->code[last]     = OP_DUP;
            vm->code[last + 1] = OP_POP;
        }
    }

        int top_fn = vm->top_fn_id;
    int pid    = vm_spawn(vm, top_fn);

    vm_run(vm);

    if (pid >= 0 && pid < vm->procs_cap && vm->procs[pid]) {
        Proc *p = vm->procs[pid];
        if (p->sp < 0)
            return *(Val *)(p->mem + p->mem_size + p->sp * sizeof(Val));
    }
    return val_nil();
}