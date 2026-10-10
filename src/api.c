/*
 * api.c — Public C API for TinyActor
 */

#define _DEFAULT_SOURCE /* expose POSIX strdup() under -std=c99 */

#include "ta.h"
#include <errno.h>
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

/* Retire a buffer displaced by append-time growth: the old allocation is
 * kept alive until vm_free. Workers may still dereference previously published
 * pointers (p->code, p->fn_table, vm->symbols) while vm_append_module /
 * vm_intern_symbol grow the shared tables — freeing via realloc would
 * turn those reads into use-after-free (observed as the multi-worker
 * compile flake: hallucinated typecheck errors / differing artifacts). */
static void vm_retire_buf(VM *vm, void *old_buf) {
    if (!old_buf)
        return;
    pthread_mutex_lock(&vm->retired_lock);
    if (vm->retired_count >= vm->retired_cap) {
        int newcap = vm->retired_cap ? vm->retired_cap * 2 : 8;
        void **nr = realloc(vm->retired_bufs, (size_t)newcap * sizeof(void *));
        if (!nr) {
            pthread_mutex_unlock(&vm->retired_lock);
            return; /* keep the old buffer alive by leaking rather than UAF */
        }
        vm->retired_bufs = nr;
        vm->retired_cap = newcap;
    }
    vm->retired_bufs[vm->retired_count++] = old_buf;
    pthread_mutex_unlock(&vm->retired_lock);
}

/* ---- 名字哈希：symbol intern / cfunc 查找共用 ----
 *
 * 两张表原本都是线性 strcmp 扫描：编译器逐 token intern、每次 CCALL 按
 * 名解析 cfunc，代价 O(名字数 × 调用数)——采样显示占 tavm 跑自举编译
 * 器的 ~95%（tavm 的 OP_CCALL_NAME 同样受害）。开放寻址 + 线性探测，
 * 条目存下标（宿主数组增长时旧数组退役不释放，下标恒稳定）。
 *
 * 并发纪律与两张宿主数组一致：读侧无锁，写侧各自在既有的临界区内；
 * 哈希增长走「新建-替换-退役」，在读者手里永不 free。 */

static uint32_t ta_name_hash(const char *s) {
    uint32_t h = 2166136261u; /* FNV-1a */
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

/* 返回命中下标，-1 = 表未建或无此名 */
static int sym_hash_lookup(VM *vm, const char *name, uint32_t h) {
    uint32_t mask = (uint32_t)vm->sym_hash_cap - 1;
    for (uint32_t i = h & mask;; i = (i + 1) & mask) {
        int32_t e = vm->sym_hash[i];
        if (e < 0)
            return -1;
        if (strcmp(vm->symbols[e], name) == 0)
            return e;
    }
}

/* 全量重建（含 0..sym_count-1）。失败返回 -1，调用方把 cap 归零退化回
 * 线性扫描——正确性不变，只是慢。 */
static int sym_hash_rebuild(VM *vm, int newcap) {
    int32_t *nh = malloc((size_t)newcap * sizeof(int32_t));
    if (!nh)
        return -1;
    memset(nh, -1, (size_t)newcap * sizeof(int32_t));
    uint32_t mask = (uint32_t)newcap - 1;
    for (int k = 0; k < vm->sym_count; k++) {
        uint32_t i = ta_name_hash(vm->symbols[k]) & mask;
        while (nh[i] >= 0)
            i = (i + 1) & mask;
        nh[i] = k;
    }
    int32_t *old = vm->sym_hash;
    vm->sym_hash = nh;
    vm->sym_hash_cap = newcap;
    if (old)
        vm_retire_buf(vm, old); /* 读者手里可能还持有旧表，退役不 free */
    return 0;
}

/* 追加下标 idx（已先写入 vm->symbols）。载荷 0.7 触发扩容；扩容已含
 * 全量重建，无需再插。 */
static void sym_hash_insert(VM *vm, int idx) {
    if (vm->sym_count * 10 >= vm->sym_hash_cap * 7) {
        if (sym_hash_rebuild(vm, vm->sym_hash_cap ? vm->sym_hash_cap * 2 : 256) == 0)
            return;
        vm->sym_hash_cap = 0; /* OOM：退化回线性扫描 */
        return;
    }
    uint32_t mask = (uint32_t)vm->sym_hash_cap - 1;
    uint32_t i = ta_name_hash(vm->symbols[idx]) & mask;
    while (vm->sym_hash[i] >= 0)
        i = (i + 1) & mask;
    vm->sym_hash[i] = idx;
}

int vm_intern_symbol(VM *vm, const char *name) {
    /* Called from worker threads at runtime (str.to_sym, C modules, DOWN
     * messages) — the table is shared VM state, so intern under lock.
     * Without this, concurrent interns return colliding symbol ids or
     * read the table mid-realloc (source of nondeterministic compile
     * corruption under multi-worker builds). */
    pthread_mutex_lock(&vm->sym_lock);
    uint32_t h = ta_name_hash(name);
    if (vm->sym_hash_cap > 0) {
        int hit = sym_hash_lookup(vm, name, h);
        if (hit >= 0) {
            pthread_mutex_unlock(&vm->sym_lock);
            return hit;
        }
    } else {
        /* 哈希建表失败（OOM）的退化路径：线性扫描 */
        for (int i = 0; i < vm->sym_count; i++) {
            if (strcmp(vm->symbols[i], name) == 0) {
                pthread_mutex_unlock(&vm->sym_lock);
                return i;
            }
        }
    }
    if (vm->sym_count >= vm->sym_cap) {
        /* malloc+copy, NOT realloc: realloc frees the old index array,
         * yanking it from under concurrent readers that still hold
         * vm->symbols. Retiring keeps the old array alive until vm_free. */
        int newcap = vm->sym_cap ? vm->sym_cap * 2 : 16;
        char **ns = malloc((size_t)newcap * sizeof(char *));
        if (!ns) {
            pthread_mutex_unlock(&vm->sym_lock);
            return -1;
        }
        memcpy(ns, vm->symbols, (size_t)vm->sym_count * sizeof(char *));
        vm_retire_buf(vm, vm->symbols);
        vm->symbols = ns;
        vm->sym_cap = newcap;
    }
    vm->symbols[vm->sym_count] = strdup(name);
    int idx = vm->sym_count++;
    sym_hash_insert(vm, idx);
    pthread_mutex_unlock(&vm->sym_lock);
    return idx;
}

/* ============================================================
 * VM lifecycle
 * ============================================================ */

/* Process-table cap. Defaults to MAX_PROCS; TA_MAX_PROCS overrides it so
 * tests can exercise proc_new's exhaustion guard with a tiny table. Invalid
 * or out-of-range values are ignored (never silently truncate the table). */
static int ta_max_procs_env(void) {
    const char *s = getenv("TA_MAX_PROCS");
    if (s && *s) {
        char *end = NULL;
        errno = 0;
        long v = strtol(s, &end, 10);
        if (errno == 0 && end != s && *end == '\0' && v >= 2 && v <= MAX_PROCS)
            return (int)v;
    }
    return MAX_PROCS;
}

static Val cfunc_print(VM *vm, Val *args, int nargs);

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
    pthread_mutex_init(&vm->wait_lock, NULL);
    pthread_mutex_init(&vm->sym_lock, NULL);
    pthread_mutex_init(&vm->retired_lock, NULL);

    /* Process table — pre-allocated to the cap (TA_MAX_PROCS or MAX_PROCS).
     * Slots are only ever indexed by pid; see proc_new. */
    vm->procs_cap = ta_max_procs_env();
    vm->procs = calloc(vm->procs_cap, sizeof(Proc *));
    vm->procs_count = 0;
    atomic_init(&vm->next_pid, 0);
    atomic_init(&vm->next_ref, 0);
    atomic_init(&vm->active_procs, 0);
    atomic_init(&vm->recv_armed, 0);
    atomic_init(&vm->wait_count, 0);

    /* Threading */
    atomic_init(&vm->stop, 0);
    vm->wake_pipe_r = -1;
    vm->wake_pipe_w = -1;
    atomic_init(&vm->main_dead, 0);
    atomic_init(&vm->main_crashed, 0);
    vm->main_pid = -1;

    /* Symbol table — pre-intern all language keywords and builtins */
    vm->sym_cap = 128;
    vm->symbols = malloc(vm->sym_cap * sizeof(char *));
    vm->sym_count = 0;

    static const char *const keywords[] = {"quote",   "define", "lambda", "if",    "begin", "let",
                                           "letrec",  "match",  "spawn",  "send",  "recv",  "self",
                                           "monitor", "cons",   "car",    "cdr",   "+",     "-",
                                           "*",       "/",      "%",      "=",     "!=",    "<",
                                           "<=",      ">",      ">=",     "null?", "pair?", "int?",
                                           "string?", "bytes?", "pid?",   "print", "true",  "false",
                                           "DOWN",    "nil",    "_",      "and",   "or",    "not",
                                           "set!",    NULL};
    for (int i = 0; keywords[i]; i++)
        vm_intern_symbol(vm, keywords[i]);

    /* print is a first-class builtin registered as a cfunc: codegen has no
     * OP_PRINT anymore and compiles (print x) to OP_CCALL_NAME. */
    vm_register(vm, "print", cfunc_print, 1);

    return vm;
}

void vm_free(VM *vm) {
    if (!vm)
        return;
    prof_finish(vm); /* safety net for embedders that skip the CLI path */

    /* Free procs retired by proc_die: they were removed from procs[] and
     * their free deferred (watcher arrays may be touched by a concurrent
     * monitor builtin call). All threads are joined before this runs. */
    Proc *r = vm->retired;
    while (r) {
        Proc *nx = r->next_retired;
        pthread_mutex_destroy(&r->mbox_lock);
        vm_free_proc_tokvecs(r); /* no-op if freed in proc_die */
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
        vm_free_proc_tokvecs(p);
        proc_chunk_reset(p);
        free(p->mem);
        free(p->watchers);
        free(p->watcher_refs);
        free(p->gc_to);
        free(p);
    }
    free(vm->procs);
    pthread_mutex_destroy(&vm->rq_lock);
    pthread_cond_destroy(&vm->rq_cond);
    pthread_mutex_destroy(&vm->procs_lock);
    pthread_mutex_destroy(&vm->wait_lock);
    pthread_mutex_destroy(&vm->sym_lock);
    pthread_mutex_destroy(&vm->retired_lock);
    /* retired buffers displaced by append-time growth (see vm_retire_buf)
     * — freed only now that no thread can hold a pointer */
    for (int i = 0; i < vm->retired_count; i++)
        free(vm->retired_bufs[i]);
    free(vm->retired_bufs);
    free(vm->code);
    free(vm->fn_table);
    for (int i = 0; i < vm->fn_names_count; i++)
        free(vm->fn_names[i]);
    free(vm->fn_names);
    for (int i = 0; i < vm->sym_count; i++)
        free(vm->symbols[i]);
    free(vm->symbols);
    free(vm->sym_hash);
    free(vm->cfunc_hash);
    free(vm->runq);
    if (vm->wake_pipe_r >= 0)
        close(vm->wake_pipe_r);
    if (vm->wake_pipe_w >= 0)
        close(vm->wake_pipe_w);
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

/* print: same behavior as the deleted OP_PRINT opcode — print_val +
 * newline + flush, evaluates to nil. Typecheck pins arity to 1. */
static Val cfunc_print(VM *vm, Val *args, int nargs) {
    (void)nargs;
    print_val(vm, args[0]);
    printf("\n");
    fflush(stdout);
    return val_nil();
}

/* cfunc 名字哈希：MAX_CFUNCS 有界（128），一次建表 512 槽（载荷上限
 * ~358），此后只插不扩。重名保首见——与线性扫描 first-match 同语义。 */
static int cfunc_hash_lookup(VM *vm, const char *name, uint32_t h) {
    uint32_t mask = (uint32_t)vm->cfunc_hash_cap - 1;
    for (uint32_t i = h & mask;; i = (i + 1) & mask) {
        int32_t e = vm->cfunc_hash[i];
        if (e < 0)
            return -1;
        if (strcmp(vm->cfuncs[e].name, name) == 0)
            return e;
    }
}

static void cfunc_hash_insert(VM *vm, int idx) {
    if (vm->cfunc_hash_cap == 0) {
        int32_t *nh = malloc(512 * sizeof(int32_t));
        if (!nh)
            return; /* 无哈希：vm_find_cfunc 走线性扫描，正确性不变 */
        memset(nh, -1, 512 * sizeof(int32_t));
        int32_t *old = vm->cfunc_hash;
        vm->cfunc_hash = nh;
        vm->cfunc_hash_cap = 512;
        if (old)
            vm_retire_buf(vm, old);
    }
    uint32_t mask = (uint32_t)vm->cfunc_hash_cap - 1;
    uint32_t i = ta_name_hash(vm->cfuncs[idx].name) & mask;
    while (vm->cfunc_hash[i] >= 0) {
        if (strcmp(vm->cfuncs[vm->cfunc_hash[i]].name, vm->cfuncs[idx].name) == 0)
            return; /* 重名：先见者胜，线性扫描同款 */
        i = (i + 1) & mask;
    }
    vm->cfunc_hash[i] = idx;
}

void vm_register(VM *vm, const char *name, Val (*fn)(VM *vm, Val *args, int nargs), int nargs) {
    if (vm->cfunc_count >= MAX_CFUNCS)
        return;
    char *dup = strdup(name);
    if (!dup)
        return;
    vm->cfuncs[vm->cfunc_count].name = dup;
    vm->cfuncs[vm->cfunc_count].fn = fn;
    vm->cfuncs[vm->cfunc_count].nargs = nargs;
    cfunc_hash_insert(vm, vm->cfunc_count); /* 先进哈希，后发布 count */
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
    if (vm->cfunc_hash_cap > 0)
        return cfunc_hash_lookup(vm, name, ta_name_hash(name));
    for (int i = 0; i < vm->cfunc_count; i++) {
        if (strcmp(vm->cfuncs[i].name, name) == 0)
            return i;
    }
    return -1;
}

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

/* ============================================================
 * vm C module — spawn, get_arg, tokvec, time
 * ============================================================ */

/* Global argv for bootstrap mode */
static int g_argc = 0;
static char **g_argv = NULL;

void vm_set_argv(int argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
}

/* Read back the argv that vm_set_argv stored (used by the os module). */
void vm_get_argv(int *argc, char ***argv) {
    *argc = g_argc;
    *argv = g_argv;
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
 * Used by the Lisp codegen to emit OP_CCALL_NAME for C module functions. */
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

/* Token vectors are owned by the Proc that creates them (issue #121):
 * the table lives in Proc.tok_vecs (void* here, TokVec* in use). The
 * builtins below always run via OP_CCALL_NAME, where tls_current_proc is set.
 * Ids are opaque to TA and invalid across procs. */
#define TOK_TABLE(p) ((TokVec **)(p)->tok_vecs)

static Val vm_make_tok_vec_fn(VM *vm, Val *args, int nargs) {
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!p || !val_is_pair(args[0]))
        return val_int(-1);
    TokVec **tok_vecs = TOK_TABLE(p);
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
    Proc *p = tls_current_proc;
    if (!p)
        return val_nil();
    TokVec **tok_vecs = TOK_TABLE(p);
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
    Proc *p = tls_current_proc;
    if (!p)
        return val_nil();
    TokVec **tok_vecs = TOK_TABLE(p);
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
    Proc *p = tls_current_proc;
    if (!p)
        return val_nil();
    TokVec **tok_vecs = TOK_TABLE(p);
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

/* Free every token vector owned by p. Called from proc_die (per-proc
 * ownership also fixes the leak: the old global table kept vectors alive
 * until vm_free) and from vm_free for procs that never hit proc_die. */
void vm_free_proc_tokvecs(Proc *p) {
    TokVec **tok_vecs = TOK_TABLE(p);
    for (int i = 0; i < MAX_TOK_VECS; i++) {
        TokVec *vec = tok_vecs[i];
        if (!vec)
            continue;
        for (int j = 0; j < vec->len; j++)
            free(vec->entries[j].str);
        free(vec->entries);
        free(vec);
        tok_vecs[i] = NULL;
    }
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

/* vm.time_us() -> int — monotonic clock in microseconds. TA ints are
 * int48 (±~1.4e14): µs since boot stays in range for ~4.5 years, while
 * raw ns would overflow after ~1.6 days of machine uptime. */
static Val vm_time_us_fn(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    return val_int((int64_t)(prof_now_ns() / 1000));
}

static TaFunc vm_module_funcs[] = {{"time_ms", vm_time_ms_fn, 0},
                                   {"time_us", vm_time_us_fn, 0},
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