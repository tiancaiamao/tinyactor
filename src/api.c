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

/* Length of bytecode produced by the last compile_all() (see compile.c). */
extern int g_last_code_len;

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
    vm->main_pid = -1;

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
    scratch.mem_size = 1 << 22;        /* 4 MiB */
    scratch.mem      = malloc(scratch.mem_size);
    scratch.gc_to    = malloc(scratch.mem_size);
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
    free(scratch.gc_to);
    return rc;
}

/* ============================================================
 * Module / import resolution (.ta files)
 * ============================================================ */

/* Read an entire file into a malloc'd, NUL-terminated buffer. */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Copy a string Val into a fixed C buffer, NUL-terminated. */
static void string_val_to_c(Val sv, char *out, int max) {
    HeapString *hs = val_get_string(sv);
    int n = hs->len;
    if (n > max - 1) n = max - 1;
    memcpy(out, hs->data, n);
    out[n] = '\0';
}

/* (define_pub (name params...) body...)
 *  -> (define (prefix.name params...) body...) */
static Val rename_export(VM *vm, Proc *sp, Val form, const char *prefix) {
    Val sig      = val_get_car(val_get_cdr(form));   /* (name . params) */
    Val name_val = val_get_car(sig);
    const char *name = vm->symbols[val_get_symbol(name_val)];

    int plen = (int)strlen(prefix);
    int nlen = (int)strlen(name);
    char *new_name = malloc((size_t)plen + 1 + nlen + 1);
    snprintf(new_name, (size_t)plen + 1 + nlen + 1, "%s.%s", prefix, name);
    Val new_name_sym = val_symbol((uint32_t)vm_intern_symbol(vm, new_name));
    free(new_name);

    Val new_sig    = val_pair(sp, new_name_sym, val_get_cdr(sig));
    Val define_sym = val_symbol((uint32_t)vm_intern_symbol(vm, "define"));
    return val_pair(sp, define_sym,
                    val_pair(sp, new_sig, val_get_cdr(val_get_cdr(form))));
}

/* Parse all top-level forms from a .ta source string into a list. */
static Val parse_ta_file(VM *vm, Proc *sp, const char *src) {
    int pos = 0;
    int len = (int)strlen(src);
    Val forms = val_nil();
    Val *tail = &forms;
    while (pos < len) {
        while (pos < len && (src[pos] == ' '  || src[pos] == '\n' ||
                             src[pos] == '\t' || src[pos] == '\r'))
            pos++;
        if (pos >= len) break;
        int old_pos = pos;
        Val form = reader_ta_read(vm, src, &pos);
        if (pos == old_pos) break;        /* no progress -> stop */
        if (val_is_nil(form)) continue;   /* skip stray nil forms */
        Val cell = val_pair(sp, form, val_nil());
        *tail = cell;
        tail  = &((HeapPair *)val_as_pair(cell))->cdr;
    }
    return forms;
}

/* Is `name` a built-in C module (net/http/test/...)? Such imports are
 * compile-time no-ops: their functions are registered globally in the VM. */
static int is_builtin_module(VM *vm, const char *name) {
    for (int i = 0; i < vm->mod_count; i++)
        if (strcmp(vm->mod_names[i], name) == 0)
            return 1;
    return 0;
}

/* Recursively load a module: resolve its imports, rename its exports,
 * and return the flat list of forms to splice into the caller. */
static Val load_module(VM *vm, Proc *sp,
                       const char *module_name,
                       const char *base_dir, int depth) {
    if (depth > 16) {
        fprintf(stderr, "error: import depth exceeded (circular import?)\n");
        return val_nil();
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ta", base_dir, module_name);
    char *src = read_file(path);
    if (!src) {
        snprintf(path, sizeof(path), "lib/%s.ta", module_name);
        src = read_file(path);
    }
    if (!src) {
        fprintf(stderr, "error: cannot find module '%s'\n", module_name);
        return val_nil();
    }

    Val mod_forms = parse_ta_file(vm, sp, src);
    free(src);

    Val result = val_nil();
    Val *tail  = &result;

    Val cur = mod_forms;
    while (val_is_pair(cur)) {
        Val form = val_get_car(cur);
        Val head = val_get_car(form);

        if (val_is_symbol(head)) {
            const char *hname = vm->symbols[val_get_symbol(head)];

                        if (strcmp(hname, "import") == 0) {
                Val mod_str = val_get_car(val_get_cdr(form));
                char sub_name[256];
                string_val_to_c(mod_str, sub_name, sizeof(sub_name));

                /* Built-in C modules (str/net/...) are compile-time no-ops:
                 * their functions are already registered globally as
                 * "module.func", so do not try to load a .ta file for them.
                 * Mirrors the top-level handling in vm_load_ta. */
                if (is_builtin_module(vm, sub_name)) {
                    Val cell = val_pair(sp, form, val_nil());
                    *tail = cell;
                    tail  = &((HeapPair *)val_as_pair(cell))->cdr;
                } else {
                    Val sub_forms = load_module(vm, sp, sub_name, base_dir, depth + 1);
                    while (val_is_pair(sub_forms)) {
                        Val cell = val_pair(sp, val_get_car(sub_forms), val_nil());
                        *tail = cell;
                        tail  = &((HeapPair *)val_as_pair(cell))->cdr;
                        sub_forms = val_get_cdr(sub_forms);
                    }
                }
                        } else if (strcmp(hname, "define_pub") == 0) {
                /* Keep original name — comp_find_fn has module prefix fallback.
                 * Convert define_pub to define (compiler needs define). */
                                Val define_sym = val_symbol((uint32_t)vm_intern_symbol(vm, "define"));
                Val new_form = val_pair(sp, define_sym, val_get_cdr(form));
                Val cell = val_pair(sp, new_form, val_nil());
                *tail = cell;
                tail  = &((HeapPair *)val_as_pair(cell))->cdr;
            } else {
                /* define / type / etc.: keep as-is */
                Val cell = val_pair(sp, form, val_nil());
                *tail = cell;
                tail  = &((HeapPair *)val_as_pair(cell))->cdr;
            }
        }
        cur = val_get_cdr(cur);
    }
    return result;
}

int vm_load_ta(VM *vm, const char *src, const char *base_dir) {
    Proc scratch;
    memset(&scratch, 0, sizeof(Proc));
        scratch.mem_size = 1 << 22;   /* 4 MiB; imports add extra forms */
    scratch.mem      = malloc(scratch.mem_size);
    scratch.gc_to    = malloc(scratch.mem_size);
    scratch.sp       = 0;

    Val main_forms = parse_ta_file(vm, &scratch, src);

    Val all_forms = val_nil();
    Val *tail     = &all_forms;

    Val cur = main_forms;
    while (val_is_pair(cur)) {
        Val form = val_get_car(cur);
        Val head = val_get_car(form);

                if (val_is_symbol(head) &&
            strcmp(vm->symbols[val_get_symbol(head)], "import") == 0) {
            Val mod_str = val_get_car(val_get_cdr(form));
            char mod_name[256];
            string_val_to_c(mod_str, mod_name, sizeof(mod_name));

            /* Built-in C modules (net/http/...) are no-ops: keep the
             * (import ...) form so compile.c handles it as before. */
            if (is_builtin_module(vm, mod_name)) {
                Val cell = val_pair(&scratch, form, val_nil());
                *tail = cell;
                tail  = &((HeapPair *)val_as_pair(cell))->cdr;
                cur = val_get_cdr(cur);
                continue;
            }

            Val mod_forms = load_module(vm, &scratch, mod_name, base_dir, 0);
            while (val_is_pair(mod_forms)) {
                Val cell = val_pair(&scratch, val_get_car(mod_forms), val_nil());
                *tail = cell;
                tail  = &((HeapPair *)val_as_pair(cell))->cdr;
                mod_forms = val_get_cdr(mod_forms);
            }
        } else {
            Val cell = val_pair(&scratch, form, val_nil());
            *tail = cell;
            tail  = &((HeapPair *)val_as_pair(cell))->cdr;
        }
        cur = val_get_cdr(cur);
    }

    int rc = compile_all(vm, all_forms);
    free(scratch.mem);
    free(scratch.gc_to);
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
        /* Derive the module search dir from the file's own directory. */
        char dir[512];
        strncpy(dir, path, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        char *last_slash = strrchr(dir, '/');
        if (last_slash) *last_slash = '\0';
        else strcpy(dir, ".");

        int rc = vm_load_ta(vm, buf, dir);
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

/* ============================================================
 * .tabc bytecode file format — dumper + loader
 * ============================================================ */

static void write_u32(FILE *f, uint32_t val) {
    fputc((int)(val & 0xFF), f);
    fputc((int)((val >> 8)  & 0xFF), f);
    fputc((int)((val >> 16) & 0xFF), f);
    fputc((int)((val >> 24) & 0xFF), f);
}

static uint32_t read_u32(FILE *f) {
    uint32_t b0 = (uint32_t)fgetc(f);
    uint32_t b1 = (uint32_t)fgetc(f);
    uint32_t b2 = (uint32_t)fgetc(f);
    uint32_t b3 = (uint32_t)fgetc(f);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

/* Dumper: write current VM bytecode state to a .tabc file.
 * Returns 0 on success, -1 on error. */
int vm_dump_tabc(VM *vm, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    /* Header */
    fwrite("TABC", 1, 4, f);
    write_u32(f, 1);                            /* version */
    write_u32(f, (uint32_t)vm->sym_count);      /* n_symbols */
    write_u32(f, (uint32_t)vm->fn_count);       /* n_fns */
    write_u32(f, (uint32_t)vm->top_fn_id);      /* top_fn_id */
    write_u32(f, (uint32_t)g_last_code_len);    /* code_len */

    /* Symbol table */
    for (int i = 0; i < vm->sym_count; i++) {
        int slen = (int)strlen(vm->symbols[i]);
        write_u32(f, (uint32_t)slen);
        fwrite(vm->symbols[i], 1, (size_t)slen, f);
    }

    /* Function table */
    for (int i = 0; i < vm->fn_count; i++)
        write_u32(f, (uint32_t)vm->fn_table[i]);

    /* Code section */
    if (g_last_code_len > 0)
        fwrite(vm->code, 1, (size_t)g_last_code_len, f);

    fclose(f);
    return 0;
}

/* Loader: read a .tabc file, populate VM state ready to run.
 * Returns 0 on success, -1 on error.  On error, partial allocations
 * are NOT cleaned up (caller is expected to vm_free() the VM). */
int vm_load_tabc(VM *vm, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    /* Header */
    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "TABC", 4) != 0) {
        fclose(f);
        return -1;
    }
    uint32_t version   = read_u32(f);
    (void)version;                              /* only v1 defined */
    uint32_t n_symbols = read_u32(f);
    uint32_t n_fns     = read_u32(f);
    uint32_t top_fn_id = read_u32(f);
    uint32_t code_len  = read_u32(f);

    /* Symbol table */
    vm->sym_count = (int)n_symbols;
    vm->sym_cap   = (int)n_symbols;
    vm->symbols   = malloc(sizeof(char *) * (n_symbols > 0 ? n_symbols : 1));
    for (uint32_t i = 0; i < n_symbols; i++) {
        uint32_t slen = read_u32(f);
        char *s = malloc((size_t)slen + 1);
        if (fread(s, 1, slen, f) != slen) { free(s); fclose(f); return -1; }
        s[slen] = '\0';
        vm->symbols[i] = s;
    }

    /* Function table */
    vm->fn_count = (int)n_fns;
    vm->fn_table = malloc(sizeof(int) * (n_fns > 0 ? n_fns : 1));
    for (uint32_t i = 0; i < n_fns; i++)
        vm->fn_table[i] = (int)read_u32(f);

    /* Code section */
    vm->code = malloc(code_len > 0 ? code_len : 1);
    if (code_len > 0 && fread(vm->code, 1, code_len, f) != code_len) {
        fclose(f);
        return -1;
    }
        vm->top_fn_id = (int)top_fn_id;
    vm->main_pid  = -1;

    fclose(f);
    return 0;
}