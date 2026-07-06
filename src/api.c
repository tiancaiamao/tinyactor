/*
 * api.c — Public C API for TinyActor
 */

#include "ta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Provided by reader.c */
extern Val reader_read(VM *vm, const char *src, int *pos);
extern Val reader_ta_read(VM *vm, const char *src, int *pos);

/* (REMOVED) compile_all was in compile.c — the C compilation path
 * is replaced by the TA tokenizer→parser→codegen pipeline.
 * Keep a stub for any callers that haven't been updated yet. */
static int compile_all(VM *vm, Val forms) {
    (void)vm; (void)forms;
    fprintf(stderr, "error: compile_all called but compile.c was removed. "
            "Use the TA pipeline (tokenizer→parser→codegen) instead.\n");
    return -1;
}

/* ============================================================
 * Symbol interning
 * ============================================================ */

int vm_intern_symbol(VM *vm, const char *name) {
    for (int i = 0; i < vm->sym_count; i++) {
        if (strcmp(vm->symbols[i], name) == 0) return i;
    }
    DA_GROW(vm->symbols, vm->sym_count, vm->sym_cap);
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
        free(p->gc_roots);
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
 * Module registration
 * ============================================================ */

void vm_register_module(VM *vm, const char *name,
                        TaFunc *funcs, int nfuncs) {
    /* Track in module registry */
    if (vm->mod_count >= vm->mod_cap) {
        int new_cap = vm->mod_cap ? vm->mod_cap * 2 : 16;
        TaFunc **new_funcs  = realloc(vm->mod_funcs,  new_cap * sizeof(TaFunc *));
        int     *new_nfuncs = realloc(vm->mod_nfuncs, new_cap * sizeof(int));
        char   **new_names  = realloc(vm->mod_names,  new_cap * sizeof(char *));
        if (!new_funcs || !new_nfuncs || !new_names) {
            free(new_funcs); free(new_nfuncs); free(new_names);
            return;
        }
        vm->mod_funcs  = new_funcs;
        vm->mod_nfuncs = new_nfuncs;
        vm->mod_names  = new_names;
        vm->mod_cap    = new_cap;
    }
    vm->mod_funcs[vm->mod_count]  = funcs;
    vm->mod_nfuncs[vm->mod_count] = nfuncs;
    vm->mod_names[vm->mod_count]  = strdup(name);
    vm->mod_count++;

    /* Register each function as "module.funcname" in cfunc table */
    for (int i = 0; i < nfuncs; i++) {
        int qlen = (int)(strlen(name) + 1 + strlen(funcs[i].name) + 1);
        char *qualified = malloc(qlen);
        snprintf(qualified, qlen, "%s.%s", name, funcs[i].name);
        vm_register(vm, qualified, funcs[i].fn, funcs[i].nargs);
        free(qualified);
    }
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
    free(scratch.gc_roots);
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

/* Parse all top-level forms from a source string into a list.
 * Uses reader_ta_read for .ta files, reader_read for .lisp files. */
static Val parse_source(VM *vm, Proc *sp, const char *src, int is_lisp) {
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
        Val form = is_lisp ? reader_read(vm, src, &pos)
                           : reader_ta_read(vm, src, &pos);
        if (pos == old_pos) break;        /* no progress -> stop */
                        if (val_is_nil(form)) continue;   /* skip stray nil forms */
        /* Flatten (begin form1 form2 ...) into individual top-level forms */
        if (val_is_pair(form)) {
            Val head = val_get_car(form);
            if (val_is_symbol(head) &&
                strcmp(vm->symbols[val_get_symbol(head)], "begin") == 0) {
                Val inner = val_get_cdr(form);
                while (val_is_pair(inner)) {
                    Val f = val_get_car(inner);
                    Val cell = val_pair(sp, f, val_nil());
                    *tail = cell;
                    tail  = &((HeapPair *)val_as_pair(cell))->cdr;
                    inner = val_get_cdr(inner);
                }
                continue;
            }
        }
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
    int is_lisp = 0;

    snprintf(path, sizeof(path), "%s/%s.ta", base_dir, module_name);
    char *src = read_file(path);
    if (!src) {
        snprintf(path, sizeof(path), "lib/%s.ta", module_name);
        src = read_file(path);
    }
    if (!src) {
        snprintf(path, sizeof(path), "%s/%s.lisp", base_dir, module_name);
        src = read_file(path);
        if (src) is_lisp = 1;
    }
    if (!src) {
        snprintf(path, sizeof(path), "lib/%s.lisp", module_name);
        src = read_file(path);
        if (src) is_lisp = 1;
    }
    if (!src) {
        fprintf(stderr, "error: cannot find module '%s'\n", module_name);
        return val_nil();
    }

    Val mod_forms = parse_source(vm, sp, src, is_lisp);
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

int vm_load_ta(VM *vm, const char *src, const char *base_dir, int is_lisp) {
    Proc scratch;
    memset(&scratch, 0, sizeof(Proc));
        scratch.mem_size = 1 << 22;   /* 4 MiB; imports add extra forms */
    scratch.mem      = malloc(scratch.mem_size);
    scratch.gc_to    = malloc(scratch.mem_size);
    scratch.sp       = 0;

        Val main_forms = parse_source(vm, &scratch, src, is_lisp);

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
    free(scratch.gc_roots);
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

                int rc = vm_load_ta(vm, buf, dir, 0);
        free(buf);
        return rc;
    }

    /* .lisp files: parse with S-expr reader, resolve imports (for codegen.lisp etc.) */
    if (len >= 5 && strcmp(path + len - 5, ".lisp") == 0) {
        char dir[512];
        strncpy(dir, path, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        char *last_slash = strrchr(dir, '/');
        if (last_slash) *last_slash = '\0';
        else strcpy(dir, ".");

        int rc = vm_load_ta(vm, buf, dir, 1);
        free(buf);
        return rc;
    }

    int rc = vm_load(vm, buf);
    free(buf);
    return rc;
}

/* ============================================================
  * eval (--eval mode)
 * ============================================================ */

Val vm_eval(VM *vm, const char *src) {
    /* Pass source directly to vm_load without (begin ...) wrapping.
     * Wrapping in begin hides (define ...) forms from compile_all's
     * function scanner (phase 1), causing function calls to crash. */
    if (vm_load(vm, src) != 0)
        return val_nil();

    /* Patch trailing OP_POP OP_PUSH_NIL OP_HALT → OP_DUP OP_POP OP_HALT
     * so the expression result stays on top of the stack when OP_HALT
     * saves it to vm->eval_result. */
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

    vm_spawn(vm, vm->top_fn_id);
    vm_run(vm);

    return vm->eval_result;
}

/* ============================================================
 * .tabc bytecode file format — dumper + loader
 * ============================================================ */

static int vm_append_module(VM *vm, const uint8_t *data, int data_len);

static void write_u32(FILE *f, uint32_t val) {
    fputc((int)(val & 0xFF), f);
    fputc((int)((val >> 8)  & 0xFF), f);
    fputc((int)((val >> 16) & 0xFF), f);
    fputc((int)((val >> 24) & 0xFF), f);
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
    write_u32(f, (uint32_t)vm->code_len);       /* code_len */

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
    if (vm->code_len > 0)
        fwrite(vm->code, 1, (size_t)vm->code_len, f);

    fclose(f);
    return 0;
}

/* Loader: read a .tabc file and APPEND it to VM state via vm_append_module.
 * On a fresh VM the first load behaves like a replace (bases are 0).
 * Returns 0 on success, -1 on error. */
int vm_load_tabc(VM *vm, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    /* Slurp the whole file into memory, then delegate to vm_append_module. */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    rewind(f);

    uint8_t *buf = malloc((size_t)(sz > 0 ? sz : 1));
    if (!buf) { fclose(f); return -1; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    int top = vm_append_module(vm, buf, (int)sz);
    free(buf);
    if (top < 0) return -1;

    vm->top_fn_id = top;
    vm->main_pid  = -1;
    return 0;
}

/* ============================================================
 * Multi-module loading: rebase + append
 * ============================================================ */

/* Instruction length table — total size (opcode + operand bytes) for
 * fixed-length opcodes.  Variable-length opcodes (CLOSURE, PUSH_STRING,
 * CCALL) are handled specially by the scanner; we store 0 here as a
 * sentinel meaning "variable, resolve at runtime".  The table is indexed
 * by OpCode enum value and covers OP_COUNT entries. */
static const uint8_t instr_len[OP_COUNT] = {
    1,  /* 0  OP_PUSH_NIL */
    1,  /* 1  OP_PUSH_TRUE */
    1,  /* 2  OP_PUSH_FALSE */
    2,  /* 3  OP_PUSH_INT8 */
    9,  /* 4  OP_PUSH_INT */
    5,  /* 5  OP_PUSH_SYM */
    0,  /* 6  OP_PUSH_STRING  (variable: 1+4+len) */
    5,  /* 7  OP_LOAD */
    5,  /* 8  OP_STORE */
    1,  /* 9  OP_CONS */
    1,  /* 10 OP_CAR */
    1,  /* 11 OP_CDR */
    1,  /* 12 OP_ADD */
    1,  /* 13 OP_SUB */
    1,  /* 14 OP_MUL */
    1,  /* 15 OP_DIV */
    1,  /* 16 OP_MOD */
    1,  /* 17 OP_EQ */
    1,  /* 18 OP_LT */
    1,  /* 19 OP_LE */
    1,  /* 20 OP_IS_NIL */
    1,  /* 21 OP_IS_PAIR */
    1,  /* 22 OP_IS_INT */
    1,  /* 23 OP_IS_STRING */
    1,  /* 24 OP_IS_BYTES */
    1,  /* 25 OP_IS_PID */
    5,  /* 26 OP_JUMP */
    5,  /* 27 OP_JUMP_IF_FALSE */
    1,  /* 28 OP_POP */
    1,  /* 29 OP_DUP */
    0,  /* 30 OP_CLOSURE    (variable: 1+4+4+nfree*4) */
    5,  /* 31 OP_CALL */
    5,  /* 32 OP_TAIL_CALL */
    1,  /* 33 OP_RET */
    5,  /* 34 OP_SPAWN */
    5,  /* 35 OP_SPAWN_MAIN */
    1,  /* 36 OP_SPAWN_CLOS */
    1,  /* 37 OP_SEND */
    1,  /* 38 OP_RECV */
    1,  /* 39 OP_RECV_PEEK */
    1,  /* 40 OP_RECV_COMMIT */
    1,  /* 41 OP_SELF */
    1,  /* 42 OP_MONITOR */
    1,  /* 43 OP_PRINT */
    1,  /* 44 OP_HALT */
    9,  /* 45 OP_MATCH_INT */
    5,  /* 46 OP_MATCH_SYM */
    1,  /* 47 OP_MATCH_NIL */
    1,  /* 48 OP_MATCH_PAIR */
    5,  /* 49 OP_MATCH_JUMP */
    1,  /* 50 OP_STR_LEN */
    1,  /* 51 OP_STR_CONCAT */
    1,  /* 52 OP_STR_SLICE */
    1,  /* 53 OP_STR_EQ */
    6,  /* 54 OP_CCALL */
    5,  /* 55 OP_ENTER */
};

/* Scan bytecode in [code, code+code_len) and rebase every embedded
 * reference so it points into the combined code/fn space:
 *   - jump targets (JUMP, JUMP_IF_FALSE, MATCH_JUMP): += code_base
 *   - fn_ids (CLOSURE, SPAWN, SPAWN_MAIN):            += fn_base
 * The buffer is modified in place. */
static void rebase_code(uint8_t *code, int code_len, int code_base, int fn_base,
                        const int *sym_map) {
    int pc = 0;
    while (pc < code_len) {
        uint8_t op = code[pc];
        if (op >= OP_COUNT) break;  /* corrupt bytecode — stop scanning */

        switch (op) {
                case OP_PUSH_SYM:
        case OP_MATCH_SYM: {
            int32_t idx;
            memcpy(&idx, code + pc + 1, 4);
            idx = sym_map[idx];
            memcpy(code + pc + 1, &idx, 4);
            pc += 5;
            break;
        }
        case OP_JUMP:
        case OP_JUMP_IF_FALSE:
        case OP_MATCH_JUMP: {
            int32_t addr;
            memcpy(&addr, code + pc + 1, 4);
            addr += code_base;
            memcpy(code + pc + 1, &addr, 4);
            pc += 5;
            break;
        }
        case OP_CLOSURE: {
            int32_t fn_id, nfree;
            memcpy(&fn_id,  code + pc + 1, 4);
            memcpy(&nfree, code + pc + 5, 4);
            fn_id += fn_base;
            memcpy(code + pc + 1, &fn_id, 4);
            pc += 9 + nfree * 4;
            break;
        }
        case OP_SPAWN:
        case OP_SPAWN_MAIN: {
            int32_t fn_id;
            memcpy(&fn_id, code + pc + 1, 4);
            fn_id += fn_base;
            memcpy(code + pc + 1, &fn_id, 4);
            pc += 5;
            break;
        }
        case OP_PUSH_STRING: {
            int32_t slen;
            memcpy(&slen, code + pc + 1, 4);
            pc += 5 + slen;
            break;
        }
        default:
            pc += instr_len[op];
            break;
        }
    }
}

/* Internal reader over a memory buffer — mirrors the FILE-based
 * helpers above but operates on in-memory .tabc data. */
typedef struct {
    const uint8_t *p;
    int len;
    int pos;
} MemReader;

static int mem_u32(MemReader *r, uint32_t *out) {
    if (r->pos + 4 > r->len) return -1;
    *out =  (uint32_t)r->p[r->pos]
          | ((uint32_t)r->p[r->pos + 1] << 8)
          | ((uint32_t)r->p[r->pos + 2] << 16)
          | ((uint32_t)r->p[r->pos + 3] << 24);
    r->pos += 4;
    return 0;
}

static int mem_read(MemReader *r, void *dst, int n) {
    if (r->pos + n > r->len) return -1;
    memcpy(dst, r->p + r->pos, n);
    r->pos += n;
    return 0;
}

/* Parse .tabc data from memory and APPEND it to vm.
 * Returns the rebased top_fn_id of the appended module, or -1 on error.
 * Bases:
  *   code_base = vm->code_len   (jump/branch targets shift by this)
 *   fn_base   = vm->fn_count   (fn_ids shift by this)
 *   sym_base  = vm->sym_count  (symbol indices in PUSH_SYM shift by this) */
static int vm_append_module(VM *vm, const uint8_t *data, int data_len) {
    MemReader r = { data, data_len, 0 };

    /* Header */
    if (r.len < 4 || memcmp(r.p, "TABC", 4) != 0) return -1;
    r.pos = 4;
    uint32_t version, n_symbols, n_fns, top_fn_id, code_len;
    if (mem_u32(&r, &version)   != 0) return -1;
    if (mem_u32(&r, &n_symbols) != 0) return -1;
    if (mem_u32(&r, &n_fns)     != 0) return -1;
    if (mem_u32(&r, &top_fn_id) != 0) return -1;
    if (mem_u32(&r, &code_len)  != 0) return -1;
    (void)version;

            int code_base = vm->code_len;
    int fn_base   = vm->fn_count;

        /* --- Symbols: intern each (dedup against existing global table) --- */
    int *sym_map = malloc((size_t)n_symbols * sizeof(int));
    if (!sym_map) return -1;
    for (uint32_t i = 0; i < n_symbols; i++) {
        uint32_t slen;
        if (mem_u32(&r, &slen) != 0) { free(sym_map); return -1; }
        char *s = malloc((size_t)slen + 1);
        if (!s) { free(sym_map); return -1; }
        if (mem_read(&r, s, (int)slen) != 0) { free(s); free(sym_map); return -1; }
        s[slen] = '\0';
        /* Dedup: reuse existing index if symbol already in global table */
        int idx = -1;
        for (int j = 0; j < vm->sym_count; j++) {
            if (strcmp(vm->symbols[j], s) == 0) { idx = j; break; }
        }
        if (idx < 0) {
            if (vm->sym_count >= vm->sym_cap) {
                int newcap = vm->sym_cap ? vm->sym_cap * 2 : 64;
                char **ns = realloc(vm->symbols, (size_t)newcap * sizeof(char *));
                if (!ns) { free(s); free(sym_map); return -1; }
                vm->symbols = ns;
                vm->sym_cap = newcap;
            }
            vm->symbols[vm->sym_count] = s;
            idx = vm->sym_count++;
        } else {
            free(s);  /* duplicate — already in table */
        }
        sym_map[i] = idx;
    }

    /* --- Function table: rebasing each offset by code_base --- */
    {
        int need = (int)n_fns;
        if (vm->fn_count + need > vm->fn_table_cap) {
            int newcap = vm->fn_table_cap ? vm->fn_table_cap : 16;
            while (newcap < vm->fn_count + need) newcap *= 2;
            int *nt = realloc(vm->fn_table, (size_t)newcap * sizeof(int));
            if (!nt) return -1;
            vm->fn_table = nt;
            vm->fn_table_cap = newcap;
        }
        for (uint32_t i = 0; i < n_fns; i++) {
            uint32_t off;
            if (mem_u32(&r, &off) != 0) return -1;
            vm->fn_table[vm->fn_count++] = (int)off + code_base;
        }
    }

    /* --- Code section: copy to a scratch buffer, rebase, append --- */
    if (code_len > 0) {
        uint8_t *tmp = malloc(code_len);
        if (!tmp) return -1;
        if (mem_read(&r, tmp, (int)code_len) != 0) { free(tmp); return -1; }

                        rebase_code(tmp, (int)code_len, code_base, fn_base, sym_map);

        if (vm->code_len + (int)code_len > vm->code_cap) {
            int newcap = vm->code_cap ? vm->code_cap : 256;
            while (newcap < vm->code_len + (int)code_len) newcap *= 2;
            uint8_t *nc = realloc(vm->code, (size_t)newcap);
            if (!nc) { free(tmp); return -1; }
            vm->code = nc;
            vm->code_cap = newcap;
        }
        memcpy(vm->code + vm->code_len, tmp, code_len);
        vm->code_len += (int)code_len;
        free(tmp);
    }

        /* Update all processes' shared pointers — code/fn_table may have
     * been realloc'd, leaving existing processes with stale pointers. */
    for (int i = 0; i < vm->procs_cap; i++) {
        Proc *p = vm->procs[i];
        if (p) {
            p->code     = vm->code;
            p->fn_table = vm->fn_table;
            p->fn_count = vm->fn_count;
        }
    }

        free(sym_map);

    return (int)top_fn_id + fn_base;
}

/* ============================================================
 * vm C module — load_bytecode, spawn, get_arg
 * ============================================================ */

extern int buf_get_data(int64_t handle, uint8_t **data_out, int *len_out);

/* Global argv for bootstrap mode */
static int   g_argc = 0;
static char **g_argv = NULL;

void vm_set_argv(int argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
}

/* (vm.load_bytecode buf_handle) -> Int top_fn_id, or -1 on error */
static Val vm_load_bytecode_fn(VM *vm, Val *args, int nargs) {
    (void)nargs;
    if (!val_is_int(args[0])) return val_int(-1);
    int64_t handle = val_get_int(args[0]);
    uint8_t *data;
    int len;
    if (buf_get_data(handle, &data, &len) != 0) return val_int(-1);
    return val_int(vm_append_module(vm, data, len));
}

/* (vm.spawn fn_id) -> Int pid */
static Val vm_spawn_fn(VM *vm, Val *args, int nargs) {
    (void)nargs;
    if (!val_is_int(args[0])) return val_int(-1);
    int fn_id = (int)val_get_int(args[0]);
    return val_int(vm_spawn(vm, fn_id));
}

/* (vm.get_arg [idx]) -> String
 * Returns target arg by index (0-based, after flags).
 * With no arg, returns index 0 (backward compatible). */
static Val vm_get_arg_fn(VM *vm, Val *args, int nargs) {
    (void)vm;
    Proc *p = tls_current_proc;
    int start = 1;
    if (g_argc >= 2 && (strcmp(g_argv[1], "--bootstrap") == 0 ||
                        strcmp(g_argv[1], "--bootstrap-emit") == 0))
        start = 2;
    int idx = 0;
    if (nargs >= 1 && val_is_int(args[0]))
        idx = (int)val_get_int(args[0]);
    int arg_idx = start + idx;
    if (arg_idx >= g_argc || !g_argv[arg_idx]) return val_string(p, "", 0);
    return val_string(p, g_argv[arg_idx], (int)strlen(g_argv[arg_idx]));
}

/* Deep-copy a Val tree from one Proc heap to another.
 * Symbols and integers are immediate values; strings and pairs are heap. */
static Val deep_copy_val(Proc *dst, Proc *src, Val v) {
    (void)src;
    if (val_is_pair(v)) {
        gc_root_push(dst, v);
        /* v is now in gc_roots; re-read from there after any proc_grow
         * (gc_fixup_heap_pointers updates gc_roots, not C locals). */
        int slot = dst->gc_root_count - 1;
        Val sv = dst->gc_roots[slot];
        Val car = deep_copy_val(dst, src, val_get_car(sv));
        gc_root_push(dst, car);
        sv = dst->gc_roots[slot];  /* re-read after potential proc_grow */
        Val cdr = deep_copy_val(dst, src, val_get_cdr(sv));
        car = gc_root_pop(dst);
        v = gc_root_pop(dst);
        return val_pair(dst, car, cdr);
    }
    if (val_is_string(v)) {
        HeapString *hs = val_get_string(v);
        return val_string(dst, hs->data, hs->len);
    }
    return v;  /* symbols, ints, nil, true, false — immediate */
}

/* Resolve imports in a pre-parsed AST.
 * Takes a list of S-expr forms (some of which may be (import name) forms)
 * and a base directory, returns a flat list with imports resolved.
 * Uses the provided scratch proc for allocations. */
static Val resolve_imports_in_ast(VM *vm, Proc *sp, Val ast, const char *dir) {
    Val result = val_nil();
    Val *tail = &result;

                    Val cur = ast;
    while (val_is_pair(cur)) {
        Val form = val_get_car(cur);

        /* Non-pair top-level forms (bare symbols, numbers) — keep as-is */
        if (!val_is_pair(form)) {
            Val cell = val_pair(sp, form, val_nil());
            *tail = cell;
            tail  = &((HeapPair *)val_as_pair(cell))->cdr;
            cur = val_get_cdr(cur);
            continue;
        }

        Val head = val_get_car(form);

        if (val_is_symbol(head) &&
            strcmp(vm->symbols[val_get_symbol(head)], "import") == 0) {
            Val mod_str = val_get_car(val_get_cdr(form));
            char mod_name[256];
            string_val_to_c(mod_str, mod_name, sizeof(mod_name));

            if (is_builtin_module(vm, mod_name)) {
                /* Built-in C module: keep import form as no-op marker */
                Val cell = val_pair(sp, form, val_nil());
                *tail = cell;
                tail  = &((HeapPair *)val_as_pair(cell))->cdr;
            } else {
                Val mod_forms = load_module(vm, sp, mod_name, dir, 0);
                while (val_is_pair(mod_forms)) {
                    Val cell = val_pair(sp, val_get_car(mod_forms), val_nil());
                    *tail = cell;
                    tail  = &((HeapPair *)val_as_pair(cell))->cdr;
                    mod_forms = val_get_cdr(mod_forms);
                }
            }
        } else {
            Val cell = val_pair(sp, form, val_nil());
            *tail = cell;
            tail  = &((HeapPair *)val_as_pair(cell))->cdr;
        }
        cur = val_get_cdr(cur);
    }
    return result;
}

/* (vm.resolve_imports ast path) -> AST
 * Takes a pre-parsed AST (list of S-expr forms, some of which may be
 * (import name) forms) and the original source file path. Returns the
 * flat form list with all imports resolved. Derives the base directory
 * for module search from the path. The AST must have been parsed by the
 * TA parser (parser.ta) which produces S-expression format. */
static Val vm_resolve_imports_fn(VM *vm, Val *args, int nargs) {
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_pair(args[0]) || !val_is_string(args[1])) return val_nil();

    char path[512];
    string_val_to_c(args[1], path, sizeof(path));

    /* Derive base dir from path */
    char dir[512];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *last_slash = strrchr(dir, '/');
    if (last_slash) *last_slash = '\0';
    else strcpy(dir, ".");

            /* Build resolved AST directly on the calling proc's heap.
     * We use gc_root slots to protect values from GC movement:
     *   slot 0: head of result list
     *   slot 1: last cell in result list (for O(1) append)
     *   slot 2: current cursor in input list (cur)
     *   slot 3: current form being processed (form)
     */
    Val result = val_nil();
    gc_root_push(p, result);  /* slot 0: result head */
    gc_root_push(p, result);  /* slot 1: result last cell */
    gc_root_push(p, args[0]); /* slot 2: cur */
    gc_root_push(p, val_nil()); /* slot 3: form (placeholder) */

    while (val_is_pair(p->gc_roots[2])) {
        Val cur = p->gc_roots[2];
        Val form = val_get_car(cur);
        p->gc_roots[3] = form;  /* root form */

        if (!val_is_pair(form)) {
            Val cell = val_pair(p, p->gc_roots[3], val_nil());
            Val head = p->gc_roots[0];
            if (val_is_nil(head)) {
                p->gc_roots[0] = cell;
            } else {
                HeapPair *hp = val_as_pair(p->gc_roots[1]);
                hp->cdr = cell;
            }
            p->gc_roots[1] = cell;
            p->gc_roots[2] = val_get_cdr(cur);
            continue;
        }

        Val head_sym = val_get_car(form);
        if (val_is_symbol(head_sym) &&
            strcmp(vm->symbols[val_get_symbol(head_sym)], "import") == 0) {
            Val mod_str = val_get_car(val_get_cdr(form));
            char mod_name[256];
            string_val_to_c(mod_str, mod_name, sizeof(mod_name));

            if (is_builtin_module(vm, mod_name)) {
                Val cell = val_pair(p, p->gc_roots[3], val_nil());
                Val head = p->gc_roots[0];
                if (val_is_nil(head)) {
                    p->gc_roots[0] = cell;
                } else {
                    HeapPair *hp = val_as_pair(p->gc_roots[1]);
                    hp->cdr = cell;
                }
                p->gc_roots[1] = cell;
            } else {
                /* Use scratch proc for module loading */
                Proc scratch;
                memset(&scratch, 0, sizeof(Proc));
                scratch.mem_size = 1 << 20;
                scratch.mem      = malloc(scratch.mem_size);
                scratch.gc_to    = malloc(scratch.mem_size);
                scratch.sp       = 0;

                Val mod_forms = load_module(vm, &scratch, mod_name, dir, 0);
                while (val_is_pair(mod_forms)) {
                    Val fv = val_get_car(mod_forms);
                    Val copy = deep_copy_val(p, &scratch, fv);
                    Val cell = val_pair(p, copy, val_nil());
                    Val head = p->gc_roots[0];
                    if (val_is_nil(head)) {
                        p->gc_roots[0] = cell;
                    } else {
                        HeapPair *hp = val_as_pair(p->gc_roots[1]);
                        hp->cdr = cell;
                    }
                    p->gc_roots[1] = cell;
                    mod_forms = val_get_cdr(mod_forms);
                }

                free(scratch.mem);
                free(scratch.gc_to);
                free(scratch.gc_roots);
            }
        } else {
            Val cell = val_pair(p, p->gc_roots[3], val_nil());
            Val head = p->gc_roots[0];
            if (val_is_nil(head)) {
                p->gc_roots[0] = cell;
            } else {
                HeapPair *hp = val_as_pair(p->gc_roots[1]);
                hp->cdr = cell;
            }
            p->gc_roots[1] = cell;
        }
        p->gc_roots[2] = val_get_cdr(cur);
    }

    gc_root_pop(p);  /* slot 3: form */
    gc_root_pop(p);  /* slot 2: cur */
    gc_root_pop(p);  /* slot 1: last cell */
    result = gc_root_pop(p);  /* slot 0: head */
    return result;
}

/* (vm.load_source path) -> AST
 * Reads a .ta source file, resolves imports recursively, and returns
 * the flat form list (S-expression AST). Uses C reader for both .ta
 * and .lisp files. The Lisp codegen then compiles the AST. */
static Val vm_load_source_fn(VM *vm, Val *args, int nargs) {
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_string(args[0])) return val_nil();

    char path[512];
    string_val_to_c(args[0], path, sizeof(path));

    /* Derive base dir from path */
    char dir[512];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *last_slash = strrchr(dir, '/');
    if (last_slash) *last_slash = '\0';
    else strcpy(dir, ".");

        char *src = read_file(path);
    if (!src) return val_nil();

    /* Detect .lisp files to use the S-expr reader instead of .ta reader */
    int is_lisp = (strstr(path, ".lisp") != NULL);

    /* Use a scratch proc for parsing and import resolution
     * (avoids GC issues in the running proc). */
    Proc scratch;
    memset(&scratch, 0, sizeof(Proc));
    scratch.mem_size = 1 << 22;   /* 4 MiB */
    scratch.mem      = malloc(scratch.mem_size);
    scratch.gc_to    = malloc(scratch.mem_size);
    scratch.sp       = 0;

                Val main_forms = parse_source(vm, &scratch, src, is_lisp);
    free(src);

    /* Resolve imports — flatten into a single form list. */
    Val all_forms = resolve_imports_in_ast(vm, &scratch, main_forms, dir);

            /* Deep-copy the resolved AST into the calling proc's heap. */
    Val result = deep_copy_val(p, &scratch, all_forms);

        free(scratch.mem);
    free(scratch.gc_to);
    free(scratch.gc_roots);
    return result;
}

/* (vm.cfunc_index sym_or_name) -> Int
 * Returns the C function registry index for the given name, or -1.
 * Used by the Lisp codegen to emit OP_CCALL for C module functions. */
static Val vm_cfunc_index_fn(VM *vm, Val *args, int nargs) {
    (void)nargs;
    const char *name = NULL;
    if (val_is_symbol(args[0])) {
        uint32_t idx = val_get_symbol(args[0]);
        if (idx < (uint32_t)vm->sym_count)
            name = vm->symbols[idx];
    } else if (val_is_string(args[0])) {
        name = val_get_string(args[0])->data;
    }
    if (!name) return val_int(-1);
    for (int i = 0; i < vm->cfunc_count; i++) {
        if (strcmp(vm->cfuncs[i].name, name) == 0)
            return val_int(i);
    }
    return val_int(-1);
}

static TaFunc vm_module_funcs[] = {
    {"load_bytecode", vm_load_bytecode_fn, 1},
    {"spawn",         vm_spawn_fn,         1},
    {"get_arg",       vm_get_arg_fn,       1},
    {"load_source",   vm_load_source_fn,   1},
    {"cfunc_index",   vm_cfunc_index_fn,   1},
    {"resolve_imports", vm_resolve_imports_fn, 2},
    {NULL, NULL, 0}
};

void vm_register_vm_module(VM *vm) {
    vm_register_module(vm, "vm", vm_module_funcs, 6);
}