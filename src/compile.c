/*
 * compile.c — AST-to-bytecode compiler for TinyActor
 *
 * Converts Val pair-chain AST (from reader) into a flat uint8_t bytecode
 * array plus function table, stored in vm->code / vm->fn_table / vm->fn_count.
 */

#include "ta.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* ============================================================
 * Code buffer — dynamic byte array
 * ============================================================ */

typedef struct {
    uint8_t *code;
    int len, cap;
} CodeBuf;

static void cb_init(CodeBuf *b) {
    b->cap = 256;
    b->code = malloc(b->cap);
    b->len = 0;
}

static void cb_free(CodeBuf *b) {
    free(b->code);
}

static void cb_grow(CodeBuf *b, int need) {
    while (b->cap < b->len + need) b->cap *= 2;
    b->code = realloc(b->code, b->cap);
}

static void emit_byte(CodeBuf *b, uint8_t v) {
    cb_grow(b, 1);
    b->code[b->len++] = v;
}



static void emit_int32(CodeBuf *b, int v) {
    cb_grow(b, 4);
    b->code[b->len++] = (uint8_t)(v & 0xFF);
    b->code[b->len++] = (uint8_t)((v >> 8) & 0xFF);
    b->code[b->len++] = (uint8_t)((v >> 16) & 0xFF);
    b->code[b->len++] = (uint8_t)((v >> 24) & 0xFF);
}

static void emit_int64(CodeBuf *b, int64_t v) {
    cb_grow(b, 8);
    for (int i = 0; i < 8; i++)
        b->code[b->len++] = (uint8_t)((v >> (i * 8)) & 0xFF);
}

static void patch_int32(CodeBuf *b, int offset, int value) {
    b->code[offset]     = (uint8_t)(value & 0xFF);
    b->code[offset + 1] = (uint8_t)((value >> 8) & 0xFF);
    b->code[offset + 2] = (uint8_t)((value >> 16) & 0xFF);
    b->code[offset + 3] = (uint8_t)((value >> 24) & 0xFF);
}

/* ============================================================
 * Compile-time environment
 * ============================================================ */

typedef struct {
    const char **names;
    int         *slots;
    int          count;
    int          cap;
} Env;

static void env_init(Env *e) {
    e->cap = 16;
    e->names = malloc(sizeof(char *) * e->cap);
    e->slots = malloc(sizeof(int) * e->cap);
    e->count = 0;
}

static void env_free(Env *e) {
    free(e->names);
    free(e->slots);
}

static void env_push(Env *e, const char *name, int slot) {
    if (e->count >= e->cap) {
        e->cap *= 2;
        e->names = realloc(e->names, sizeof(char *) * e->cap);
        e->slots = realloc(e->slots, sizeof(int) * e->cap);
    }
    e->names[e->count] = name;
    e->slots[e->count] = slot;
    e->count++;
}

static int env_find(Env *e, const char *name) {
    if (!e) return -1;
    for (int i = e->count - 1; i >= 0; i--) {
        if (strcmp(e->names[i], name) == 0)
            return e->slots[i];
    }
    return -1;
}

static Env env_snapshot(Env *e) {
    Env s;
    if (!e) {
        s.cap = 8;
        s.count = 0;
        s.names = malloc(sizeof(char *) * s.cap);
        s.slots = malloc(sizeof(int) * s.cap);
        return s;
    }
    s.cap = e->cap;
    s.count = e->count;
    s.names = malloc(sizeof(char *) * s.cap);
    s.slots = malloc(sizeof(int) * s.cap);
    memcpy(s.names, e->names, sizeof(char *) * s.count);
    memcpy(s.slots, e->slots, sizeof(int) * s.count);
    return s;
}

/* ============================================================
 * Function table entry
 * ============================================================ */

typedef struct {
    char *name;
    int   fn_id;
} FnEntry;

typedef struct {
    int fn_id;
    int entry;
} FnEntry2;

/* ============================================================
 * Compiler state
 * ============================================================ */

typedef struct {
    CodeBuf    code;
    FnEntry   *fns;
    int        fn_count, fn_cap;
    FnEntry2  *entries;
    int        entry_count, entry_cap;
    VM        *vm;
    int        next_fn_id;
    int        next_slot;
} Compiler;

/* ============================================================
 * Free variables collector
 * ============================================================ */

typedef struct {
    const char **names;
    int         *slots;
    int          count;
    int          cap;
} FreeVars;

static void fv_init(FreeVars *fv) {
    fv->cap = 16;
    fv->names = malloc(sizeof(char *) * fv->cap);
    fv->slots = malloc(sizeof(int) * fv->cap);
    fv->count = 0;
}

static void fv_free(FreeVars *fv) {
    free(fv->names);
    free(fv->slots);
}

static void fv_add(FreeVars *fv, const char *name, int slot) {
    for (int i = 0; i < fv->count; i++) {
        if (strcmp(fv->names[i], name) == 0) return;
    }
    if (fv->count >= fv->cap) {
        fv->cap *= 2;
        fv->names = realloc(fv->names, sizeof(char *) * fv->cap);
        fv->slots = realloc(fv->slots, sizeof(int) * fv->cap);
    }
    fv->names[fv->count] = name;
    fv->slots[fv->count] = slot;
    fv->count++;
}

/* ============================================================
 * Fail jump tracking (for match compilation)
 * ============================================================ */

#define MAX_FAIL_JUMPS 64
static int g_fail_jumps[MAX_FAIL_JUMPS];
static int g_fail_jump_count;

static void fail_jumps_reset(void) { g_fail_jump_count = 0; }
static void fail_jumps_add(int offset) {
    if (g_fail_jump_count < MAX_FAIL_JUMPS)
        g_fail_jumps[g_fail_jump_count++] = offset;
}
static void fail_jumps_patch_all(CodeBuf *b, int target) {
    for (int i = 0; i < g_fail_jump_count; i++)
        patch_int32(b, g_fail_jumps[i], target);
}

/* ============================================================
 * Symbol / AST helpers
 * ============================================================ */

static const char *sym_name(VM *vm, Val v) {
    return vm->symbols[val_get_symbol(v)];
}

static int sym_eq(VM *vm, Val v, const char *s) {
    if (!val_is_symbol(v)) return 0;
    return strcmp(sym_name(vm, v), s) == 0;
}

static Val ast_car(Val v) {
    return val_is_pair(v) ? val_get_car(v) : val_nil();
}

static Val ast_cdr(Val v) {
    return val_is_pair(v) ? val_get_cdr(v) : val_nil();
}

static int list_length(Val v) {
    int n = 0;
    while (val_is_pair(v)) { n++; v = val_get_cdr(v); }
    return n;
}

static Val list_ref(Val v, int i) {
    for (int k = 0; k < i && val_is_pair(v); k++)
        v = val_get_cdr(v);
    return ast_car(v);
}

/* ============================================================
 * Inline operator table
 * ============================================================ */

typedef struct {
    const char *name;
    uint8_t     op;
} InlineOp;

static const InlineOp inline_ops[] = {
    {"+",      OP_ADD},
    {"-",      OP_SUB},
    {"*",      OP_MUL},
    {"/",      OP_DIV},
    {"%",      OP_MOD},
    {"=",      OP_EQ},
    {"<",      OP_LT},
    {"<=",     OP_LE},
    {">",      OP_LT},   /* swap operands */
    {">=",     OP_LE},   /* swap operands */
    {"cons",   OP_CONS},
    {"car",    OP_CAR},
    {"cdr",    OP_CDR},
    {"null?",  OP_IS_NIL},
    {"pair?",  OP_IS_PAIR},
    {"int?",   OP_IS_INT},
    {"string?",OP_IS_STRING},
    {"bytes?", OP_IS_BYTES},
    {"pid?",   OP_IS_PID},
        {"string-length", OP_STR_LEN},
    {"string-concat", OP_STR_CONCAT},
    {"string-slice",  OP_STR_SLICE},
    {"string-eq",     OP_STR_EQ},
    {"print",  OP_PRINT},
    {NULL, 0}
};

static const InlineOp *find_inline_op(const char *name) {
    for (const InlineOp *p = inline_ops; p->name; p++) {
        if (strcmp(p->name, name) == 0) return p;
    }
    return NULL;
}

/* ============================================================
 * Compiler helpers
 * ============================================================ */

static void comp_init(Compiler *c, VM *vm) {
    cb_init(&c->code);
    c->fn_cap = 32;
    c->fns = malloc(sizeof(FnEntry) * c->fn_cap);
    c->fn_count = 0;
    c->entry_cap = 32;
    c->entries = malloc(sizeof(FnEntry2) * c->entry_cap);
    c->entry_count = 0;
    c->vm = vm;
    c->next_fn_id = 0;
    c->next_slot = 0;
}

static void comp_free(Compiler *c) {
    cb_free(&c->code);
    for (int i = 0; i < c->fn_count; i++) free(c->fns[i].name);
    free(c->fns);
    free(c->entries);
}

static int comp_reg_fn(Compiler *c, const char *name) {
    if (c->fn_count >= c->fn_cap) {
        c->fn_cap *= 2;
        c->fns = realloc(c->fns, sizeof(FnEntry) * c->fn_cap);
    }
    int id = c->next_fn_id++;
    c->fns[c->fn_count].name = strdup(name);
    c->fns[c->fn_count].fn_id = id;
    c->fn_count++;
    return id;
}

static int comp_find_fn(Compiler *c, const char *name) {
    for (int i = 0; i < c->fn_count; i++) {
        if (strcmp(c->fns[i].name, name) == 0)
            return c->fns[i].fn_id;
    }
    return -1;
}

static int comp_alloc_fn_id(Compiler *c) {
    return c->next_fn_id++;
}

static void comp_record_entry(Compiler *c, int fn_id, int entry) {
    if (c->entry_count >= c->entry_cap) {
        c->entry_cap *= 2;
        c->entries = realloc(c->entries, sizeof(FnEntry2) * c->entry_cap);
    }
    c->entries[c->entry_count].fn_id = fn_id;
    c->entries[c->entry_count].entry = entry;
    c->entry_count++;
}

/* ============================================================
 * Forward declarations
 * ============================================================ */

static void cx_expr(Compiler *c, Val expr, Env *env, int tail);
static void cx_body(Compiler *c, Val body, Env *env);
static void cx_pattern(Compiler *c, Val pat, int subj_slot, Env *env);

/* ============================================================
 * Free variable collection
 * ============================================================ */

static void cx_collect_free(Compiler *c, Val expr, Env *env, FreeVars *fv) {
        if (val_is_int(expr) || val_is_nil(expr) || expr == val_true() ||
        val_is_string(expr) || val_is_bytes(expr))
        return;

    /* Check for false: TAG_FALSE */
    if ((expr >> 48) == TAG_FALSE) return;

    if (val_is_symbol(expr)) {
        const char *name = sym_name(c->vm, expr);
        int slot = env_find(env, name);
        if (slot >= 0)
            fv_add(fv, name, slot);
        return;
    }

    if (!val_is_pair(expr)) return;

    Val head = ast_car(expr);
    if (!val_is_symbol(head)) {
        /* General: recurse all sub-expressions */
        Val rest = expr;
        while (val_is_pair(rest)) {
            cx_collect_free(c, val_get_car(rest), env, fv);
            rest = val_get_cdr(rest);
        }
        return;
    }

    const char *tag = sym_name(c->vm, head);

    if (strcmp(tag, "quote") == 0) return;

    if (strcmp(tag, "define") == 0) return;

        if (strcmp(tag, "lambda") == 0) {
        /* Don't add lambda params to the env — they're bound, not free.
         * Just recurse into the body with the parent env. */
        Val body = ast_cdr(ast_cdr(expr));
        while (val_is_pair(body)) {
            cx_collect_free(c, val_get_car(body), env, fv);
            body = val_get_cdr(body);
        }
        return;
    }

    if (strcmp(tag, "if") == 0) {
        cx_collect_free(c, list_ref(expr, 1), env, fv);
        cx_collect_free(c, list_ref(expr, 2), env, fv);
        if (list_length(expr) > 3)
            cx_collect_free(c, list_ref(expr, 3), env, fv);
        return;
    }

    if (strcmp(tag, "begin") == 0) {
        Val body = ast_cdr(expr);
        while (val_is_pair(body)) {
            cx_collect_free(c, val_get_car(body), env, fv);
            body = val_get_cdr(body);
        }
        return;
    }

    if (strcmp(tag, "let") == 0) {
        Val second = list_ref(expr, 1);
        if (val_is_symbol(second)) {
            cx_collect_free(c, list_ref(expr, 2), env, fv);
            /* Body has the new binding — we just collect from body ignoring binding */
            Val body = ast_cdr(ast_cdr(ast_cdr(expr)));
            while (val_is_pair(body)) {
                cx_collect_free(c, val_get_car(body), env, fv);
                body = val_get_cdr(body);
            }
            return;
        }
        if (val_is_pair(second)) {
            Val bindings = second;
            while (val_is_pair(bindings)) {
                Val binding = val_get_car(bindings);
                cx_collect_free(c, list_ref(binding, 1), env, fv);
                bindings = val_get_cdr(bindings);
            }
            return;
        }
        return;
    }

    if (strcmp(tag, "match") == 0) {
        cx_collect_free(c, list_ref(expr, 1), env, fv);
        Val branches = ast_cdr(ast_cdr(expr));
        while (val_is_pair(branches)) {
            Val branch = val_get_car(branches);
            Val bbody = ast_cdr(branch);
            while (val_is_pair(bbody)) {
                cx_collect_free(c, val_get_car(bbody), env, fv);
                bbody = val_get_cdr(bbody);
            }
            branches = val_get_cdr(branches);
        }
        return;
    }

    /* General form: head + args */
    cx_collect_free(c, head, env, fv);
    Val args = ast_cdr(expr);
    while (val_is_pair(args)) {
        cx_collect_free(c, val_get_car(args), env, fv);
        args = val_get_cdr(args);
    }
}

/* ============================================================
 * Pattern compilation
 * ============================================================ */

static void cx_pattern(Compiler *c, Val pat, int subj_slot, Env *env) {
    /* Wildcard _ */
    if (val_is_symbol(pat) && sym_eq(c->vm, pat, "_"))
        return;

    /* Variable: always matches, bind it */
    if (val_is_symbol(pat) && !sym_eq(c->vm, pat, "nil") &&
        !sym_eq(c->vm, pat, "cons")) {
        const char *name = sym_name(c->vm, pat);
        int slot = c->next_slot++;
        emit_byte(&c->code, OP_LOAD);
        emit_int32(&c->code, subj_slot);
        emit_byte(&c->code, OP_STORE);
        emit_int32(&c->code, slot);
        env_push(env, name, slot);
        return;
    }

    /* Integer literal */
    if (val_is_int(pat)) {
        emit_byte(&c->code, OP_LOAD);
        emit_int32(&c->code, subj_slot);
        emit_byte(&c->code, OP_MATCH_INT);
        emit_int64(&c->code, val_get_int(pat));
        emit_byte(&c->code, OP_MATCH_JUMP);
        fail_jumps_add(c->code.len);
        emit_int32(&c->code, 0);
        return;
    }

    /* nil */
    if (val_is_nil(pat)) {
        emit_byte(&c->code, OP_LOAD);
        emit_int32(&c->code, subj_slot);
        emit_byte(&c->code, OP_MATCH_NIL);
        emit_byte(&c->code, OP_MATCH_JUMP);
        fail_jumps_add(c->code.len);
        emit_int32(&c->code, 0);
        return;
    }

    /* Quoted symbol: ('x ...) */
    if (val_is_pair(pat) && sym_eq(c->vm, ast_car(pat), "quote")) {
        Val quoted = list_ref(pat, 1);
        if (val_is_symbol(quoted)) {
            emit_byte(&c->code, OP_LOAD);
            emit_int32(&c->code, subj_slot);
            emit_byte(&c->code, OP_MATCH_SYM);
            emit_int32(&c->code, (int)val_get_symbol(quoted));
            emit_byte(&c->code, OP_MATCH_JUMP);
            fail_jumps_add(c->code.len);
            emit_int32(&c->code, 0);
            return;
        }
        if (val_is_nil(quoted)) {
            emit_byte(&c->code, OP_LOAD);
            emit_int32(&c->code, subj_slot);
            emit_byte(&c->code, OP_MATCH_NIL);
            emit_byte(&c->code, OP_MATCH_JUMP);
            fail_jumps_add(c->code.len);
            emit_int32(&c->code, 0);
            return;
        }
        return;
    }

    /* (cons a b) */
    if (val_is_pair(pat) && sym_eq(c->vm, ast_car(pat), "cons")) {
        Val pat_a = list_ref(pat, 1);
        Val pat_b = list_ref(pat, 2);

        emit_byte(&c->code, OP_LOAD);
        emit_int32(&c->code, subj_slot);
        emit_byte(&c->code, OP_MATCH_PAIR);
        emit_byte(&c->code, OP_MATCH_JUMP);
        fail_jumps_add(c->code.len);
        emit_int32(&c->code, 0);

        int car_slot = c->next_slot++;
        int cdr_slot = c->next_slot++;
        emit_byte(&c->code, OP_STORE);
        emit_int32(&c->code, car_slot);
        emit_byte(&c->code, OP_STORE);
        emit_int32(&c->code, cdr_slot);

        cx_pattern(c, pat_a, car_slot, env);
        cx_pattern(c, pat_b, cdr_slot, env);
        return;
    }

    /* Proper list pattern: (p1 p2 ... pN) */
    if (val_is_pair(pat)) {
        int nelems = list_length(pat);
        Val elems[64];
                {
            Val cur = pat;
            for (int i = 0; i < nelems && val_is_pair(cur); i++) {
                elems[i] = val_get_car(cur);
                cur = val_get_cdr(cur);
            }
        }

        /* Detect dotted pair: check the tail after the last element */
        Val tail = pat;
        for (int i = 0; i < nelems && val_is_pair(tail); i++)
            tail = val_get_cdr(tail);
        /* tail is now the cdr of the last pair (nil for proper list, symbol for dotted pair) */
        int has_dotted_tail = val_is_symbol(tail) && !sym_eq(c->vm, tail, "nil");

        int cur_slot = subj_slot;
        for (int i = 0; i < nelems; i++) {
            emit_byte(&c->code, OP_LOAD);
            emit_int32(&c->code, cur_slot);
            emit_byte(&c->code, OP_MATCH_PAIR);
            emit_byte(&c->code, OP_MATCH_JUMP);
            fail_jumps_add(c->code.len);
            emit_int32(&c->code, 0);

            int car_slot = c->next_slot++;
            int cdr_slot = c->next_slot++;
            emit_byte(&c->code, OP_STORE);
            emit_int32(&c->code, car_slot);
            emit_byte(&c->code, OP_STORE);
            emit_int32(&c->code, cdr_slot);

            cx_pattern(c, elems[i], car_slot, env);

            if (i < nelems - 1) {
                cur_slot = cdr_slot;
            } else {
                if (has_dotted_tail) {
                    /* Dotted pair: bind cdr to tail pattern */
                    cx_pattern(c, tail, cdr_slot, env);
                } else {
                    /* Proper list: check cdr is nil */
                    emit_byte(&c->code, OP_LOAD);
                    emit_int32(&c->code, cdr_slot);
                    emit_byte(&c->code, OP_MATCH_NIL);
                    emit_byte(&c->code, OP_MATCH_JUMP);
                    fail_jumps_add(c->code.len);
                    emit_int32(&c->code, 0);
                }
            }
        }
        return;
    }
}

/* ============================================================
 * Expression compilation
 * ============================================================ */

static void cx_call(Compiler *c, Val expr, Env *env, int tail) {
    Val head = ast_car(expr);
    Val args = ast_cdr(expr);
    int nargs = list_length(args);

    if (val_is_symbol(head)) {
        const char *name = sym_name(c->vm, head);

        /* Inline operators */
        const InlineOp *op = find_inline_op(name);
        if (op) {
            int swap = (strcmp(name, ">") == 0 || strcmp(name, ">=") == 0);
            if (swap) {
                cx_expr(c, list_ref(args, 1), env, 0);
                cx_expr(c, list_ref(args, 0), env, 0);
            } else {
                for (int i = 0; i < nargs; i++)
                    cx_expr(c, list_ref(args, i), env, 0);
            }
                        emit_byte(&c->code, op->op);
            return;
        }

        /* Check C function registry */
        for (int i = 0; i < c->vm->cfunc_count; i++) {
            if (strcmp(name, c->vm->cfuncs[i].name) == 0) {
                for (int j = 0; j < nargs; j++)
                    cx_expr(c, list_ref(args, j), env, 0);
                emit_byte(&c->code, OP_CCALL);
                emit_int32(&c->code, i);
                emit_byte(&c->code, (uint8_t)nargs);
                return;
            }
        }

                /* spawn */
        if (strcmp(name, "spawn") == 0) {
            Val arg0 = list_ref(args, 0);
            if (val_is_pair(arg0) && sym_eq(c->vm, ast_car(arg0), "quote")) {
                Val quoted = list_ref(arg0, 1);
                if (val_is_symbol(quoted)) {
                    int fid = comp_find_fn(c, sym_name(c->vm, quoted));
                    if (fid < 0) fid = 0;
                    emit_byte(&c->code, OP_SPAWN);
                    emit_int32(&c->code, fid);
                    return;
                }
            }
            cx_expr(c, arg0, env, 0);
            emit_byte(&c->code, OP_SPAWN_CLOS);
            return;
        }

        /* send: (send pid msg) → push msg, push pid, OP_SEND */
        if (strcmp(name, "send") == 0) {
            cx_expr(c, list_ref(args, 1), env, 0); /* msg */
            cx_expr(c, list_ref(args, 0), env, 0); /* pid */
            emit_byte(&c->code, OP_SEND);
            return;
        }

        if (strcmp(name, "recv") == 0) {
            emit_byte(&c->code, OP_RECV);
            return;
        }

        if (strcmp(name, "self") == 0) {
            emit_byte(&c->code, OP_SELF);
            return;
        }

        if (strcmp(name, "monitor") == 0) {
            cx_expr(c, list_ref(args, 0), env, 0);
            emit_byte(&c->code, OP_MONITOR);
            return;
        }
    }

    /* General function call: closure, args, CALL/TAIL_CALL */
    cx_expr(c, head, env, 0);
    for (int i = 0; i < nargs; i++)
        cx_expr(c, list_ref(args, i), env, 0);

    emit_byte(&c->code, tail ? OP_TAIL_CALL : OP_CALL);
    emit_int32(&c->code, nargs);
}

static void cx_expr(Compiler *c, Val expr, Env *env, int tail) {
    /* --- Literals --- */
    if (val_is_int(expr)) {
        int64_t n = val_get_int(expr);
        if (n >= -128 && n <= 127) {
            emit_byte(&c->code, OP_PUSH_INT8);
            emit_byte(&c->code, (uint8_t)(int8_t)n);
        } else {
            emit_byte(&c->code, OP_PUSH_INT);
            emit_int64(&c->code, n);
        }
        return;
    }
        if (val_is_nil(expr))    { emit_byte(&c->code, OP_PUSH_NIL);   return; }
    if (expr == val_true())  { emit_byte(&c->code, OP_PUSH_TRUE);  return; }
    if ((expr >> 48) == TAG_FALSE) { emit_byte(&c->code, OP_PUSH_FALSE); return; }

    /* --- Symbol (variable reference) --- */
    if (val_is_symbol(expr)) {
        const char *name = sym_name(c->vm, expr);
        int slot = env_find(env, name);
        if (slot >= 0) {
            emit_byte(&c->code, OP_LOAD);
            emit_int32(&c->code, slot);
            return;
        }
        int fid = comp_find_fn(c, name);
        if (fid >= 0) {
            emit_byte(&c->code, OP_CLOSURE);
            emit_int32(&c->code, fid);
            emit_int32(&c->code, 0);
            return;
        }
        emit_byte(&c->code, OP_PUSH_NIL);
        return;
    }

        /* --- String literal --- */
    if (val_is_string(expr)) {
        HeapString *s = val_get_string(expr);
        emit_byte(&c->code, OP_PUSH_STRING);
        emit_int32(&c->code, s->len);
        for (int i = 0; i < s->len; i++)
            emit_byte(&c->code, (uint8_t)s->data[i]);
        return;
    }

    /* --- Non-pair, non-immediate: push nil --- */
    if (!val_is_pair(expr)) {
        emit_byte(&c->code, OP_PUSH_NIL);
        return;
    }

            /* --- Compound form --- */
    Val head = ast_car(expr);
    int nforms = list_length(expr) - 1;

    /* (quote x) */
    if (sym_eq(c->vm, head, "quote")) {
        Val quoted = list_ref(expr, 1);
        if (val_is_symbol(quoted)) {
            emit_byte(&c->code, OP_PUSH_SYM);
            emit_int32(&c->code, (int)val_get_symbol(quoted));
        } else if (val_is_nil(quoted)) {
            emit_byte(&c->code, OP_PUSH_NIL);
        } else if (val_is_int(quoted)) {
            int64_t n = val_get_int(quoted);
            if (n >= -128 && n <= 127) {
                emit_byte(&c->code, OP_PUSH_INT8);
                emit_byte(&c->code, (uint8_t)(int8_t)n);
            } else {
                emit_byte(&c->code, OP_PUSH_INT);
                emit_int64(&c->code, n);
            }
                } else if (quoted == val_true()) {
            emit_byte(&c->code, OP_PUSH_TRUE);
        } else if ((quoted >> 48) == TAG_FALSE) {
            emit_byte(&c->code, OP_PUSH_FALSE);
        } else {
            emit_byte(&c->code, OP_PUSH_NIL);
        }
        return;
    }

                        /* (define ...) — shouldn't appear as expression; emit nil */
    if (sym_eq(c->vm, head, "define")) {
        emit_byte(&c->code, OP_PUSH_NIL);
        return;
    }

    /* (import "name") — compile-time directive, emit nothing */
    if (sym_eq(c->vm, head, "import")) {
        emit_byte(&c->code, OP_PUSH_NIL);
        return;
    }

            /* (if cond then else?) */
    if (sym_eq(c->vm, head, "if")) {
        cx_expr(c, list_ref(expr, 1), env, 0);

        emit_byte(&c->code, OP_JUMP_IF_FALSE);
        int patch_then = c->code.len;
        emit_int32(&c->code, 0);

        cx_expr(c, list_ref(expr, 2), env, tail);

        int has_else = (nforms >= 3);
        int patch_end = -1;
        if (has_else) {
            emit_byte(&c->code, OP_JUMP);
            patch_end = c->code.len;
            emit_int32(&c->code, 0);
        }

        patch_int32(&c->code, patch_then, c->code.len);

        if (has_else) {
            cx_expr(c, list_ref(expr, 3), env, tail);
            patch_int32(&c->code, patch_end, c->code.len);
        } else {
            emit_byte(&c->code, OP_PUSH_NIL);
        }
        return;
    }

    /* (begin e1 e2 ... eN) */
            if (sym_eq(c->vm, head, "begin")) {
        Val body = ast_cdr(expr);
        if (!val_is_pair(body))
            emit_byte(&c->code, OP_PUSH_NIL);
        else
            cx_body(c, body, env);
        return;
    }

    /* (lambda (params...) body...) */
        if (sym_eq(c->vm, head, "lambda")) {
        int fn_id = comp_alloc_fn_id(c);
        Val params = list_ref(expr, 1);
        Val body = ast_cdr(ast_cdr(expr));

        /* Build combined env: outer env + lambda params (params shadow outer) */
        Env combined;
        env_init(&combined);
        if (env) {
            for (int i = 0; i < env->count; i++)
                env_push(&combined, env->names[i], env->slots[i]);
        }
        int ps = 0;
        Val p = params;
        while (val_is_pair(p)) {
            env_push(&combined, sym_name(c->vm, val_get_car(p)), ps);
            ps++;
            p = val_get_cdr(p);
        }

        /* Collect free variables with combined env */
        FreeVars fv;
        fv_init(&fv);
        Val b = body;
        while (val_is_pair(b)) {
            cx_collect_free(c, val_get_car(b), &combined, &fv);
            b = val_get_cdr(b);
        }

        /* Filter out lambda params from free vars */
        FreeVars filtered;
        fv_init(&filtered);
        for (int i = 0; i < fv.count; i++) {
            int is_param = 0;
            Val p2 = params;
            while (val_is_pair(p2)) {
                if (strcmp(fv.names[i], sym_name(c->vm, val_get_car(p2))) == 0) {
                    is_param = 1;
                    break;
                }
                p2 = val_get_cdr(p2);
            }
            if (!is_param)
                fv_add(&filtered, fv.names[i], fv.slots[i]);
        }

        /* Emit OP_CLOSURE fn_id nfree [slots...] */
        emit_byte(&c->code, OP_CLOSURE);
        emit_int32(&c->code, fn_id);
        emit_int32(&c->code, filtered.count);
        for (int i = 0; i < filtered.count; i++)
            emit_int32(&c->code, filtered.slots[i]);

        /* Jump over the lambda body */
        emit_byte(&c->code, OP_JUMP);
        int jump_over = c->code.len;
        emit_int32(&c->code, 0); /* placeholder */

        /* Lambda body code follows */
        int entry = c->code.len;
        comp_record_entry(c, fn_id, entry);

        /* Build body env: params at 0..ps-1, free vars at ps..ps+nfree-1 */
        Env body_env;
        env_init(&body_env);
        int slot = 0;
        Val p3 = params;
        while (val_is_pair(p3)) {
            env_push(&body_env, sym_name(c->vm, val_get_car(p3)), slot);
            slot++;
            p3 = val_get_cdr(p3);
        }
        for (int i = 0; i < filtered.count; i++) {
            env_push(&body_env, filtered.names[i], slot);
            slot++;
        }

        int saved = c->next_slot;
        c->next_slot = slot;
        cx_body(c, body, &body_env);
        emit_byte(&c->code, OP_RET);
        c->next_slot = saved;

        /* Patch jump to skip over body */
        patch_int32(&c->code, jump_over, c->code.len);

        env_free(&combined);
        env_free(&body_env);
        fv_free(&fv);
        fv_free(&filtered);
        return;
    }

    /* (let var expr body...) or (let ((v1 e1) ...) body...) */
            if (sym_eq(c->vm, head, "let")) {
        Val second = list_ref(expr, 1);

        if (val_is_symbol(second)) {
            /* (let var expr body...) */
            const char *var_name = sym_name(c->vm, second);
            cx_expr(c, list_ref(expr, 2), env, 0);
            int slot = c->next_slot++;
            emit_byte(&c->code, OP_STORE);
            emit_int32(&c->code, slot);

            Env *target = env;
            Env local_env;
            if (!env) {
                env_init(&local_env);
                target = &local_env;
            }
            env_push(target, var_name, slot);

            Val body = ast_cdr(ast_cdr(ast_cdr(expr)));
            if (val_is_pair(body))
                cx_body(c, body, target);
            else
                emit_byte(&c->code, OP_PUSH_NIL);
            if (!env)
                env_free(&local_env);
            return;
        }

        if (val_is_pair(second)) {
            /* (let ((v1 e1) ...) body...) */
            Env *target = env;
            Env local_env;
            if (!env) {
                env_init(&local_env);
                target = &local_env;
            }
            Val bindings = second;
            while (val_is_pair(bindings)) {
                Val binding = val_get_car(bindings);
                const char *vname = sym_name(c->vm, list_ref(binding, 0));
                cx_expr(c, list_ref(binding, 1), env, 0);
                int slot = c->next_slot++;
                emit_byte(&c->code, OP_STORE);
                emit_int32(&c->code, slot);
                env_push(target, vname, slot);
                bindings = val_get_cdr(bindings);
            }
            Val body = ast_cdr(ast_cdr(expr));
            if (val_is_pair(body))
                cx_body(c, body, target);
            else
                emit_byte(&c->code, OP_PUSH_NIL);
            if (!env)
                env_free(&local_env);
            return;
        }

        emit_byte(&c->code, OP_PUSH_NIL);
        return;
    }

    /* (match expr (pat body...) ...) */
    if (sym_eq(c->vm, head, "match")) {
        /* Compile scrutinee */
        cx_expr(c, list_ref(expr, 1), env, 0);

        /* Store in temp slot */
        int subj_slot = c->next_slot++;
        emit_byte(&c->code, OP_STORE);
        emit_int32(&c->code, subj_slot);

        int end_jumps[64];
        int end_jump_count = 0;

        Val branches = ast_cdr(ast_cdr(expr));
        while (val_is_pair(branches)) {
            Val branch = val_get_car(branches);
            Val pat = ast_car(branch);
            Val body = ast_cdr(branch);

            int nslots_before = c->next_slot;

            /* Compile pattern */
            fail_jumps_reset();
            cx_pattern(c, pat, subj_slot, env);

            /* Compile body */
            cx_body(c, body, env);

            /* Restore slot count */
            c->next_slot = nslots_before;

            /* Jump to end of match */
            emit_byte(&c->code, OP_JUMP);
            int ej = c->code.len;
            emit_int32(&c->code, 0);
            if (end_jump_count < 64)
                end_jumps[end_jump_count++] = ej;

            /* Patch all pattern fail jumps to here (next branch) */
            fail_jumps_patch_all(&c->code, c->code.len);

            branches = val_get_cdr(branches);
        }

        /* No match: push nil */
        emit_byte(&c->code, OP_PUSH_NIL);
        int end_pos = c->code.len;

        /* Patch all end jumps */
        for (int i = 0; i < end_jump_count; i++)
            patch_int32(&c->code, end_jumps[i], end_pos);

        return;
    }

    /* General function call */
    cx_call(c, expr, env, tail);
}

/* ============================================================
 * Body compilation
 * ============================================================ */

static void cx_body(Compiler *c, Val body, Env *env) {
    if (!val_is_pair(body)) {
        emit_byte(&c->code, OP_PUSH_NIL);
        return;
    }
    Val cur = body;
    while (val_is_pair(cur)) {
        Val e = val_get_car(cur);
        Val rest = val_get_cdr(cur);
        if (val_is_pair(rest)) {
            cx_expr(c, e, env, 0);
            emit_byte(&c->code, OP_POP);
        } else {
            cx_expr(c, e, env, 1);
        }
        cur = rest;
    }
}

/* ============================================================
 * Top-level compilation
 * ============================================================ */

int compile_all(VM *vm, Val forms) {
    Compiler c;
    comp_init(&c, vm);

    /* Phase 1: register all (define (name ...) ...) function names */
    {
        Val cur = forms;
        while (val_is_pair(cur)) {
            Val form = val_get_car(cur);
            if (val_is_pair(form)) {
                Val head = ast_car(form);
                if (val_is_symbol(head) && sym_eq(vm, head, "define")) {
                    Val sig = list_ref(form, 1);
                    if (val_is_pair(sig)) {
                        Val name_val = ast_car(sig);
                        if (val_is_symbol(name_val))
                            comp_reg_fn(&c, sym_name(vm, name_val));
                    }
                }
            }
            cur = val_get_cdr(cur);
        }
    }

    /* Layout:
     *   OP_JUMP <past all define bodies>    ← top_fn_id entry
     *   [define body 1] OP_RET
     *   [define body 2] OP_RET
     *   ...
     *   [top-level expressions] OP_HALT
     */

    /* Top-level function id (entry point for VM) */
    int top_fn_id = comp_alloc_fn_id(&c);

    /* Emit initial jump */
    emit_byte(&c.code, OP_JUMP);
    int jump_over = c.code.len;
    emit_int32(&c.code, 0);

    /* Compile each define body */
    {
        Val cur = forms;
        while (val_is_pair(cur)) {
            Val form = val_get_car(cur);
            if (val_is_pair(form) && sym_eq(vm, ast_car(form), "define")) {
                Val sig = list_ref(form, 1);
                if (val_is_pair(sig)) {
                    Val name_val = ast_car(sig);
                    if (val_is_symbol(name_val)) {
                        int fn_id = comp_find_fn(&c, sym_name(vm, name_val));
                        int entry = c.code.len;
                        comp_record_entry(&c, fn_id, entry);

                        /* Build param env */
                        Env fn_env;
                        env_init(&fn_env);
                        Val params = ast_cdr(sig);
                        int ps = 0;
                        Val p = params;
                        while (val_is_pair(p)) {
                            env_push(&fn_env, sym_name(vm, val_get_car(p)), ps);
                            ps++;
                            p = val_get_cdr(p);
                        }

                        int saved = c.next_slot;
                        c.next_slot = ps;

                        Val body = ast_cdr(ast_cdr(form));
                        cx_body(&c, body, &fn_env);
                        emit_byte(&c.code, OP_RET);

                        c.next_slot = saved;
                        env_free(&fn_env);
                    }
                }
            }
            cur = val_get_cdr(cur);
        }
    }

    /* Patch jump to point here (start of top-level code) */
    int top_entry = c.code.len;
    patch_int32(&c.code, jump_over, top_entry);
    comp_record_entry(&c, top_fn_id, top_entry);

        /* Compile top-level non-define forms; if none, auto-spawn main */
    {
        int has_top = 0;
        Val cur = forms;
        while (val_is_pair(cur)) {
            Val form = val_get_car(cur);
            if (!(val_is_pair(form) && sym_eq(vm, ast_car(form), "define"))) {
                has_top = 1;
                cx_expr(&c, form, NULL, 0);
                emit_byte(&c.code, OP_POP);
            }
            cur = val_get_cdr(cur);
        }
        if (!has_top) {
            int main_fid = comp_find_fn(&c, "main");
            if (main_fid >= 0) {
                emit_byte(&c.code, OP_SPAWN);
                emit_int32(&c.code, main_fid);
            }
        }
    }

    emit_byte(&c.code, OP_PUSH_NIL);
    emit_byte(&c.code, OP_HALT);

    /* Build fn_table: fn_table[fn_id] = bytecode offset */
    int max_fn_id = c.next_fn_id;
    int *fn_table = malloc(sizeof(int) * (max_fn_id > 0 ? max_fn_id : 1));
    for (int i = 0; i < max_fn_id; i++)
        fn_table[i] = 0;

    for (int i = 0; i < c.entry_count; i++) {
        if (c.entries[i].fn_id < max_fn_id)
            fn_table[c.entries[i].fn_id] = c.entries[i].entry;
    }

        /* Install in VM */
    vm->code = c.code.code;
    vm->fn_table = fn_table;
    vm->fn_count = max_fn_id;
    vm->top_fn_id = top_fn_id;

    /* Transfer ownership — don't free code buffer */
    c.code.code = NULL;

    comp_free(&c);
    return 0;
}