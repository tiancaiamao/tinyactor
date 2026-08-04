/*
 * api.c — Public C API for TinyActor
 */

#define _DEFAULT_SOURCE /* expose POSIX strdup() under -std=c99 */

#include "ta.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Provided by reader_ta.c (not in ta.h) */
/* reader_ta.c removed — TA parser is the only parser now.
 * These cfunc stubs remain for cfidx stability. */

/* ============================================================
 * Symbol interning
 * ============================================================ */

int vm_intern_symbol(VM *vm, const char *name) {
    for (int i = 0; i < vm->sym_count; i++) {
        if (strcmp(vm->symbols[i], name) == 0)
            return i;
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
    vm->rq_cap = 256;
    vm->runq = malloc(vm->rq_cap * sizeof(int));
    vm->rq_head = 0;
    vm->rq_tail = 0;
    atomic_init(&vm->rq_count, 0);
    pthread_mutex_init(&vm->rq_lock, NULL);
    pthread_cond_init(&vm->rq_cond, NULL);
    pthread_mutex_init(&vm->procs_lock, NULL);

    /* Process table — pre-allocated to MAX_PROCS */
    vm->procs_cap = MAX_PROCS;
    vm->procs = calloc(MAX_PROCS, sizeof(Proc *));
    vm->procs_count = 0;
    atomic_init(&vm->next_pid, 0);
    atomic_init(&vm->active_procs, 0);
    atomic_init(&vm->busy_workers, 0);

    /* Threading */
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    vm->nworkers = (ncpu > 0) ? (int)ncpu : 1;
    atomic_init(&vm->stop, 0);
    atomic_init(&vm->yield_requested, 0);
    atomic_init(&vm->main_dead, 0);
    vm->main_pid = -1;

    /* Symbol table — pre-intern all language keywords and builtins */
    vm->sym_cap = 128;
    vm->symbols = malloc(vm->sym_cap * sizeof(char *));
    vm->sym_count = 0;

    static const char *const keywords[] = {
        "quote", "define", "lambda",  "if",      "begin", "let",   "letrec", "match", "spawn",
        "send",  "recv",   "self",    "monitor", "cons",  "car",   "cdr",    "+",     "-",
        "*",     "/",      "%",       "=",       "<",     "<=",    ">",      ">=",    "null?",
        "pair?", "int?",   "string?", "bytes?",  "pid?",  "print", "true",   "false", "DOWN",
        "nil",   "_",      "and",     "or",      "not",   "set!",  NULL};
    for (int i = 0; keywords[i]; i++)
        vm_intern_symbol(vm, keywords[i]);

    return vm;
}

void vm_free(VM *vm) {
    /* Free procs retired by proc_die: they were removed from procs[] and
     * their free deferred (watcher arrays may be touched by a concurrent
     * OP_MONITOR). All threads are joined by vm_run before this runs. */
    Proc *r = vm->retired;
    while (r) {
        Proc *nx = r->next_retired;
        pthread_mutex_destroy(&r->mbox_lock);
        free(r->watchers);
        free(r->watcher_refs);
        free(r);
        r = nx;
    }
    vm->retired = NULL;

    for (int i = 0; i < vm->procs_cap; i++) {
        Proc *p = vm->procs[i];
        if (!p)
            continue;
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
    pthread_mutex_destroy(&vm->procs_lock);
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

void vm_register(VM *vm, const char *name, Val (*fn)(VM *vm, Val *args, int nargs), int nargs) {
    if (vm->cfunc_count >= MAX_CFUNCS)
        return;
    char *dup = strdup(name);
    if (!dup)
        return;
    vm->cfuncs[vm->cfunc_count].name = dup;
    vm->cfuncs[vm->cfunc_count].fn = fn;
    vm->cfuncs[vm->cfunc_count].nargs = nargs;
    vm->cfunc_count++;
}

/* ============================================================
 * Module registration
 * ============================================================ */

void vm_register_module(VM *vm, const char *name, TaFunc *funcs, int nfuncs) {
    /* Track in module registry */
    if (vm->mod_count >= vm->mod_cap) {
        int new_cap = vm->mod_cap ? vm->mod_cap * 2 : 16;
        TaFunc **new_funcs = realloc(vm->mod_funcs, new_cap * sizeof(TaFunc *));
        int *new_nfuncs = realloc(vm->mod_nfuncs, new_cap * sizeof(int));
        char **new_names = realloc(vm->mod_names, new_cap * sizeof(char *));
        if (!new_funcs || !new_nfuncs || !new_names) {
            free(new_funcs);
            free(new_nfuncs);
            free(new_names);
            return;
        }
        vm->mod_funcs = new_funcs;
        vm->mod_nfuncs = new_nfuncs;
        vm->mod_names = new_names;
        vm->mod_cap = new_cap;
    }
    vm->mod_funcs[vm->mod_count] = funcs;
    vm->mod_nfuncs[vm->mod_count] = nfuncs;
    vm->mod_names[vm->mod_count] = strdup(name);
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

/* Find a C function by qualified name (e.g. "http.parse_request").
 * Returns cfunc index or -1 if not found. */
int vm_find_cfunc(VM *vm, const char *name) {
    for (int i = 0; i < vm->cfunc_count; i++) {
        if (strcmp(vm->cfuncs[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* Load a C module from a shared library (.so/.dylib).
 * The library must export a function:
 *   void vm_load_self(VM *vm);
 * which calls vm_register_module() to register its functions.
 * Returns 0 on success, -1 on error. */
int vm_load_c_module(VM *vm, const char *path) {
    void *handle = dlopen(path, RTLD_NOW);
    if (!handle)
        return -1;
    void (*reg)(VM *) = (void (*)(VM *))dlsym(handle, "vm_load_self");
    if (!reg) {
        dlclose(handle);
        return -1;
    }
    reg(vm);
    return 0;
}

/* Loading is handled by the TA compiler (lib/codegen.ta) via bootstrap.tabc.
 * The C compiler (compile.c) has been removed. */

/* ============================================================
 * Module / import resolution (.ta files)
 * ============================================================ */

/* parse_source stub — C reader removed, TA parser handles parsing. */
static Val parse_source(VM *vm, Proc *sp, const char *src) {
    (void)vm;
    (void)sp;
    (void)src;
    return val_nil();
}

/* Is `name` a built-in C module (net/http/test/...)? Such imports are
 * compile-time no-ops: their functions are registered globally in the VM. */
static int is_builtin_module(VM *vm, const char *name) {
    for (int i = 0; i < vm->mod_count; i++)
        if (strcmp(vm->mod_names[i], name) == 0)
            return 1;
    return 0;
}

/* Forward declaration for vm_load_tabc below. */
static int vm_append_module(VM *vm, const uint8_t *data, int data_len);

/* Loader: read a .tabc file and APPEND it to VM state via vm_append_module.
 * On a fresh VM the first load behaves like a replace (bases are 0).
 * Returns 0 on success, -1 on error. */
int vm_load_tabc(VM *vm, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    /* Slurp the whole file into memory, then delegate to vm_append_module. */
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return -1;
    }
    rewind(f);

    uint8_t *buf = malloc((size_t)(sz > 0 ? sz : 1));
    if (!buf) {
        fclose(f);
        return -1;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    int top = vm_append_module(vm, buf, (int)sz);
    free(buf);
    if (top < 0)
        return -1;

    vm->top_fn_id = top;
    vm->main_pid = -1;
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
    1, /* 0  OP_PUSH_NIL */
    1, /* 1  OP_PUSH_TRUE */
    1, /* 2  OP_PUSH_FALSE */
    2, /* 3  OP_PUSH_INT8 */
    9, /* 4  OP_PUSH_INT */
    5, /* 5  OP_PUSH_SYM */
    0, /* 6  OP_PUSH_STRING  (variable: 1+4+len) */
    5, /* 7  OP_LOAD */
    5, /* 8  OP_STORE */
    1, /* 9  OP_CONS */
    1, /* 10 OP_CAR */
    1, /* 11 OP_CDR */
    1, /* 12 OP_ADD */
    1, /* 13 OP_SUB */
    1, /* 14 OP_MUL */
    1, /* 15 OP_DIV */
    1, /* 16 OP_MOD */
    1, /* 17 OP_EQ */
    1, /* 18 OP_LT */
    1, /* 19 OP_LE */
    1, /* 20 OP_IS_NIL */
    1, /* 21 OP_IS_PAIR */
    1, /* 22 OP_IS_INT */
    1, /* 23 OP_IS_STRING */
    1, /* 24 OP_IS_BYTES */
    1, /* 25 OP_IS_PID */
    5, /* 26 OP_JUMP */
    5, /* 27 OP_JUMP_IF_FALSE */
    1, /* 28 OP_POP */
    1, /* 29 OP_DUP */
    0, /* 30 OP_CLOSURE    (variable: 1+4+4+nfree*4) */
    5, /* 31 OP_CALL */
    5, /* 32 OP_TAIL_CALL */
    1, /* 33 OP_RET */
    5, /* 34 OP_SPAWN */
    5, /* 35 OP_SPAWN_MAIN */
    1, /* 36 OP_SPAWN_CLOS */
    1, /* 37 OP_SEND */
    1, /* 38 OP_RECV */
    1, /* 39 OP_RECV_PEEK */
    1, /* 40 OP_RECV_COMMIT */
    1, /* 41 OP_SELF */
    1, /* 42 OP_MONITOR */
    1, /* 43 OP_PRINT */
    1, /* 44 OP_HALT */
    9, /* 45 OP_MATCH_INT */
    5, /* 46 OP_MATCH_SYM */
    1, /* 47 OP_MATCH_NIL */
    1, /* 48 OP_MATCH_PAIR */
    5, /* 49 OP_MATCH_JUMP */
    1, /* 50 OP_STR_LEN */
    1, /* 51 OP_STR_CONCAT */
    1, /* 52 OP_STR_SLICE */
    1, /* 53 OP_STR_EQ */
    6, /* 54 reserved (was OP_CCALL — removed) */
    5, /* 55 OP_ENTER */
    6, /* 56 OP_CCALL_NAME */
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
        if (op >= OP_COUNT)
            break; /* corrupt bytecode — stop scanning */

        switch (op) {
        case OP_PUSH_SYM:
        case OP_MATCH_SYM:
        case OP_CCALL_NAME: {
            int32_t idx;
            memcpy(&idx, code + pc + 1, 4);
            idx = sym_map[idx];
            memcpy(code + pc + 1, &idx, 4);
            pc += (op == OP_CCALL_NAME) ? 6 : 5;
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
            memcpy(&fn_id, code + pc + 1, 4);
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
    if (r->pos + 4 > r->len)
        return -1;
    *out = (uint32_t)r->p[r->pos] | ((uint32_t)r->p[r->pos + 1] << 8) |
           ((uint32_t)r->p[r->pos + 2] << 16) | ((uint32_t)r->p[r->pos + 3] << 24);
    r->pos += 4;
    return 0;
}

static int mem_read(MemReader *r, void *dst, int n) {
    if (r->pos + n > r->len)
        return -1;
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
    MemReader r = {data, data_len, 0};

    /* Header */
    if (r.len < 4 || memcmp(r.p, "TABC", 4) != 0)
        return -1;
    r.pos = 4;
    uint32_t version, n_symbols, n_fns, top_fn_id, code_len;
    if (mem_u32(&r, &version) != 0)
        return -1;
    if (mem_u32(&r, &n_symbols) != 0)
        return -1;
    if (mem_u32(&r, &n_fns) != 0)
        return -1;
    if (mem_u32(&r, &top_fn_id) != 0)
        return -1;
    if (mem_u32(&r, &code_len) != 0)
        return -1;
    (void)version;

    int code_base = vm->code_len;
    int fn_base = vm->fn_count;

    /* --- Symbols: intern each (dedup against existing global table) --- */
    int *sym_map = malloc((size_t)n_symbols * sizeof(int));
    if (!sym_map)
        return -1;
    for (uint32_t i = 0; i < n_symbols; i++) {
        uint32_t slen;
        if (mem_u32(&r, &slen) != 0) {
            free(sym_map);
            return -1;
        }
        char *s = malloc((size_t)slen + 1);
        if (!s) {
            free(sym_map);
            return -1;
        }
        if (mem_read(&r, s, (int)slen) != 0) {
            free(s);
            free(sym_map);
            return -1;
        }
        s[slen] = '\0';
        /* Dedup: reuse existing index if symbol already in global table */
        int idx = -1;
        for (int j = 0; j < vm->sym_count; j++) {
            if (strcmp(vm->symbols[j], s) == 0) {
                idx = j;
                break;
            }
        }
        if (idx < 0) {
            if (vm->sym_count >= vm->sym_cap) {
                int newcap = vm->sym_cap ? vm->sym_cap * 2 : 64;
                char **ns = realloc(vm->symbols, (size_t)newcap * sizeof(char *));
                if (!ns) {
                    free(s);
                    free(sym_map);
                    return -1;
                }
                vm->symbols = ns;
                vm->sym_cap = newcap;
            }
            vm->symbols[vm->sym_count] = s;
            idx = vm->sym_count++;
        } else {
            free(s); /* duplicate — already in table */
        }
        sym_map[i] = idx;
    }

    /* --- Function table: rebasing each offset by code_base --- */
    {
        int need = (int)n_fns;
        if (vm->fn_count + need > vm->fn_table_cap) {
            int newcap = vm->fn_table_cap ? vm->fn_table_cap : 16;
            while (newcap < vm->fn_count + need)
                newcap *= 2;
            int *nt = realloc(vm->fn_table, (size_t)newcap * sizeof(int));
            if (!nt)
                return -1;
            vm->fn_table = nt;
            vm->fn_table_cap = newcap;
        }
        for (uint32_t i = 0; i < n_fns; i++) {
            uint32_t off;
            if (mem_u32(&r, &off) != 0)
                return -1;
            vm->fn_table[vm->fn_count++] = (int)off + code_base;
        }
    }

    /* --- Code section: copy to a scratch buffer, rebase, append --- */
    if (code_len > 0) {
        uint8_t *tmp = malloc(code_len);
        if (!tmp)
            return -1;
        if (mem_read(&r, tmp, (int)code_len) != 0) {
            free(tmp);
            return -1;
        }

        rebase_code(tmp, (int)code_len, code_base, fn_base, sym_map);

        if (vm->code_len + (int)code_len > vm->code_cap) {
            int newcap = vm->code_cap ? vm->code_cap : 256;
            while (newcap < vm->code_len + (int)code_len)
                newcap *= 2;
            uint8_t *nc = realloc(vm->code, (size_t)newcap);
            if (!nc) {
                free(tmp);
                return -1;
            }
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
            p->code = vm->code;
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
static int g_argc = 0;
static char **g_argv = NULL;

void vm_set_argv(int argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
}

/* (vm.load_bytecode buf_handle) -> Int top_fn_id, or -1 on error */
static Val vm_load_bytecode_fn(VM *vm, Val *args, int nargs) {
    (void)nargs;
    if (!val_is_int(args[0]))
        return val_int(-1);
    int64_t handle = val_get_int(args[0]);
    uint8_t *data;
    int len;
    if (buf_get_data(handle, &data, &len) != 0)
        return val_int(-1);
    return val_int(vm_append_module(vm, data, len));
}

/* (vm.spawn fn_id) -> Int pid */
static Val vm_spawn_fn(VM *vm, Val *args, int nargs) {
    (void)nargs;
    if (!val_is_int(args[0]))
        return val_int(-1);
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
    if (g_argc >= 2 &&
        (strcmp(g_argv[1], "--bootstrap") == 0 || strcmp(g_argv[1], "--bootstrap-emit") == 0))
        start = 2;
    int idx = 0;
    if (nargs >= 1 && val_is_int(args[0]))
        idx = (int)val_get_int(args[0]);
    int arg_idx = start + idx;
    if (arg_idx >= g_argc || !g_argv[arg_idx])
        return val_string(p, "", 0);
    return val_string(p, g_argv[arg_idx], (int)strlen(g_argv[arg_idx]));
}

/* vm.resolve_imports stub — import resolution handled in TA driver. */
static Val vm_resolve_imports_fn(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    return args[0];
}

/* (vm.load_source path) -> AST - DEPRECATED stub, kept for cfidx stability */
static Val vm_load_source_fn(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    return val_nil();
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
    if (!name)
        return val_int(-1);
    for (int i = 0; i < vm->cfunc_count; i++) {
        if (strcmp(vm->cfuncs[i].name, name) == 0)
            return val_int(i);
    }
    return val_int(-1);
}

/* (vm.is_builtin_module name) -> Bool
 * Returns true if the given module name is a C-registered builtin. */
static Val vm_is_builtin_module_fn(VM *vm, Val *args, int nargs) {
    (void)nargs;
    const char *name = NULL;
    if (val_is_string(args[0]))
        name = val_get_string(args[0])->data;
    if (!name)
        return val_false();
    return is_builtin_module(vm, name) ? val_true() : val_false();
}

/* (vm.parse_source src) -> List
 * Parses source text into a list of top-level forms using the C reader.
 * Returns nil on empty/invalid input.
 *
 * NOTE: Must strdup the source — parse_source allocates many heap
 * objects which can trigger GC. The moving GC invalidates the raw
 * pointer into the original HeapString. */
static Val vm_parse_source_fn(VM *vm, Val *args, int nargs) {
    (void)nargs;
    if (!val_is_string(args[0]))
        return val_nil();
    const char *src = val_get_string(args[0])->data;
    char *copy = strdup(src);
    if (!copy)
        return val_nil();
    Val result = parse_source(vm, tls_current_proc, copy);
    free(copy);
    return result;
}

/* ============================================================
 * Token vector — O(1) random access for TA parser
 * ============================================================ */
typedef struct {
    int type_idx; /* interned symbol index for the token type */
    int val_type; /* 0=nil, 1=int, 2=string */
    int64_t num;
    char *str;
} TokEntry;

typedef struct {
    int len;
    TokEntry *entries;
} TokVec;

#define MAX_TOK_VECS 32
static TokVec *tok_vecs[MAX_TOK_VECS];

static Val vm_make_tok_vec_fn(VM *vm, Val *args, int nargs) {
    (void)nargs;
    if (!val_is_pair(args[0]))
        return val_int(-1);
    int id = 0;
    while (id < MAX_TOK_VECS && tok_vecs[id])
        id++;
    if (id >= MAX_TOK_VECS)
        return val_int(-1);

    Val cur = args[0];
    int len = 0;
    while (val_is_pair(cur)) {
        len++;
        cur = val_get_cdr(cur);
    }

    TokVec *vec = calloc(1, sizeof(TokVec));
    vec->len = len;
    vec->entries = calloc((size_t)len, sizeof(TokEntry));

    cur = args[0];
    for (int i = 0; i < len; i++) {
        Val pair = val_get_car(cur);
        Val type_val = val_get_car(pair);
        Val val_val = val_get_cdr(pair);
        if (val_is_symbol(type_val))
            vec->entries[i].type_idx = (int)val_get_symbol(type_val);
        if (val_is_int(val_val)) {
            vec->entries[i].val_type = 1;
            vec->entries[i].num = val_get_int(val_val);
        } else if (val_is_string(val_val)) {
            HeapString *hs = val_get_string(val_val);
            vec->entries[i].val_type = 2;
            vec->entries[i].str = malloc((size_t)hs->len + 1);
            memcpy(vec->entries[i].str, hs->data, (size_t)hs->len);
            vec->entries[i].str[hs->len] = '\0';
        } else {
            vec->entries[i].val_type = 0;
        }
        cur = val_get_cdr(cur);
    }
    tok_vecs[id] = vec;
    return val_int(id);
}

static Val vm_tok_type_fn(VM *vm, Val *args, int nargs) {
    (void)nargs;
    int id = (int)val_get_int(args[0]);
    int pos = (int)val_get_int(args[1]);
    if (id < 0 || id >= MAX_TOK_VECS || !tok_vecs[id])
        return val_nil();
    TokVec *vec = tok_vecs[id];
    if (pos < 0 || pos >= vec->len) {
        return val_symbol((uint32_t)vm_intern_symbol(vm, "eof"));
    }
    return val_symbol((uint32_t)vec->entries[pos].type_idx);
}

static Val vm_tok_val_fn(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    int id = (int)val_get_int(args[0]);
    int pos = (int)val_get_int(args[1]);
    if (id < 0 || id >= MAX_TOK_VECS || !tok_vecs[id])
        return val_nil();
    TokVec *vec = tok_vecs[id];
    if (pos < 0 || pos >= vec->len)
        return val_nil();
    TokEntry *e = &vec->entries[pos];
    switch (e->val_type) {
    case 1:
        return val_int(e->num);
    case 2:
        return val_string(tls_current_proc, e->str, (int)strlen(e->str));
    default:
        return val_nil();
    }
}

static Val vm_free_tok_vec_fn(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    int id = (int)val_get_int(args[0]);
    if (id < 0 || id >= MAX_TOK_VECS || !tok_vecs[id])
        return val_nil();
    TokVec *vec = tok_vecs[id];
    for (int i = 0; i < vec->len; i++)
        free(vec->entries[i].str);
    free(vec->entries);
    free(vec);
    tok_vecs[id] = NULL;
    return val_nil();
}

/* vm.time_ms() -> int — monotonic clock in milliseconds (rate limiting etc). */
static Val vm_time_ms_fn(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return val_int((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static TaFunc vm_module_funcs[] = {{"time_ms", vm_time_ms_fn, 0},
                                   {"load_bytecode", vm_load_bytecode_fn, 1},
                                   {"spawn", vm_spawn_fn, 1},
                                   {"get_arg", vm_get_arg_fn, 1},
                                   {"load_source", vm_load_source_fn, 1},
                                   {"cfunc_index", vm_cfunc_index_fn, 1},
                                   {"resolve_imports", vm_resolve_imports_fn, 2},
                                   {"is_builtin_module", vm_is_builtin_module_fn, 1},
                                   {"parse_source", vm_parse_source_fn, 1},
                                   {"make_tok_vec", vm_make_tok_vec_fn, 1},
                                   {"tok_type", vm_tok_type_fn, 2},
                                   {"tok_val", vm_tok_val_fn, 2},
                                   {"free_tok_vec", vm_free_tok_vec_fn, 1},
                                   {NULL, NULL, 0}};

void vm_register_vm_module(VM *vm) { vm_register_module(vm, "vm", vm_module_funcs, 13); }