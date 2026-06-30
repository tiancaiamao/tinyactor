/*
 * typecheck.c — Hindley-Milner type checker for TinyActor
 *
 * Runs after parsing (reader_ta.c) and before compilation (compile.c).
 * Walks the pair-tree AST, infers types using Algorithm W (Robinson
 * unification), checks annotations, and reports errors.
 *
 * DESIGN PRINCIPLE: Be permissive. For untyped code, all types are fresh
 * variables and unification always succeeds. Errors are only reported
 * when concrete types conflict (e.g., int vs string in arithmetic).
 */

#include "ta.h"
#include "typecheck.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * Forward declarations
 * ============================================================ */

typedef struct TypeEnv TypeEnv;
static Type *infer_expr(TypeCtx *ctx, Val expr, TypeEnv *env);
static Type *infer_body(TypeCtx *ctx, Val body, TypeEnv *env);
static Type *prune(Type *t);
static void generalize_type(Type *t, TypeEnv *env);

/* ============================================================
 * Type allocation
 * ============================================================ */

/* Simple tracking: keep a list of all allocated types for cleanup. */
static Type **g_alloced = NULL;
static int g_alloc_count = 0;
static int g_alloc_cap = 0;

static Type *type_alloc(TypeKind kind) {
    Type *t = (Type *)calloc(1, sizeof(Type));
    if (!t) return NULL;
    t->kind = kind;
    if (g_alloc_count >= g_alloc_cap) {
        g_alloc_cap = g_alloc_cap ? g_alloc_cap * 2 : 512;
        g_alloced = (Type **)realloc(g_alloced, g_alloc_cap * sizeof(Type *));
    }
    g_alloced[g_alloc_count++] = t;
    return t;
}

static void type_free_all(void) {
    for (int i = 0; i < g_alloc_count; i++) {
        Type *t = g_alloced[i];
        if (t->kind == TY_ARROW && t->arrow.params) free(t->arrow.params);
        if (t->kind == TY_ADT && t->adt.args) free(t->adt.args);
        free(t);
    }
    free(g_alloced);
    g_alloced = NULL;
    g_alloc_count = 0;
    g_alloc_cap = 0;
}

static void type_reset(TypeCtx *ctx) {
    type_free_all();
    ctx->next_var_id = 0;
}

/* ============================================================
 * Type constructors
 * ============================================================ */

static Type *type_new_var(TypeCtx *ctx) {
    Type *t = type_alloc(TY_VAR);
    t->var.id = ctx->next_var_id++;
    t->var.instance = NULL;
    t->var.quantified = 0;
    return t;
}

static Type *type_new_con(const char *name) {
    Type *t = type_alloc(TY_CON);
    t->con.name = name;
    return t;
}

static Type *type_new_arrow(TypeCtx *ctx, Type **params, int nparams, Type *ret) {
    Type *t = type_alloc(TY_ARROW);
    t->arrow.params = params;
    t->arrow.nparams = nparams;
    t->arrow.ret = ret;
    (void)ctx;
    return t;
}

/* Built-in type singletons (cached) */
static Type *ty_int(TypeCtx *ctx)    { (void)ctx; return type_new_con("int"); }
static Type *ty_bool(TypeCtx *ctx)   { (void)ctx; return type_new_con("bool"); }
static Type *ty_string(TypeCtx *ctx) { (void)ctx; return type_new_con("string"); }
static Type *ty_pid(TypeCtx *ctx)    { (void)ctx; return type_new_con("pid"); }
static Type *ty_symbol(TypeCtx *ctx) { (void)ctx; return type_new_con("symbol"); }

/* ============================================================
 * Type display (for error messages)
 * ============================================================ */

/* Fill buf with a human-readable representation of t.
 * Uses a simple recursive approach with a position tracker. */
static void type_show_rec(Type *t, char *buf, int *pos, int bufsize) {
    t = prune(t);
    switch (t->kind) {
    case TY_VAR:
        if (t->var.quantified) {
            char c = 'a' + (t->var.id % 26);
            *pos += snprintf(buf + *pos, bufsize - *pos, "'%c", c);
        } else {
            *pos += snprintf(buf + *pos, bufsize - *pos, "t%d", t->var.id);
        }
        break;
    case TY_CON:
        *pos += snprintf(buf + *pos, bufsize - *pos, "%s", t->con.name);
        break;
    case TY_ARROW:
        if (t->arrow.nparams == 0) {
            *pos += snprintf(buf + *pos, bufsize - *pos, "() -> ");
            type_show_rec(t->arrow.ret, buf, pos, bufsize);
        } else if (t->arrow.nparams == 1) {
            type_show_rec(t->arrow.params[0], buf, pos, bufsize);
            *pos += snprintf(buf + *pos, bufsize - *pos, " -> ");
            type_show_rec(t->arrow.ret, buf, pos, bufsize);
        } else {
            *pos += snprintf(buf + *pos, bufsize - *pos, "(");
            for (int i = 0; i < t->arrow.nparams; i++) {
                if (i > 0) *pos += snprintf(buf + *pos, bufsize - *pos, ", ");
                type_show_rec(t->arrow.params[i], buf, pos, bufsize);
            }
            *pos += snprintf(buf + *pos, bufsize - *pos, ") -> ");
            type_show_rec(t->arrow.ret, buf, pos, bufsize);
        }
        break;
    case TY_ADT:
        *pos += snprintf(buf + *pos, bufsize - *pos, "%s", t->adt.name);
        break;
    }
}

static void type_show(Type *t, char *buf, int bufsize) {
    int pos = 0;
    type_show_rec(t, buf, &pos, bufsize);
}

/* ============================================================
 * Unification (Robinson algorithm)
 * ============================================================ */

static Type *prune(Type *t) {
    if (t->kind == TY_VAR && t->var.instance) {
        t->var.instance = prune(t->var.instance);
        return t->var.instance;
    }
    return t;
}

static int occurs_check(int var_id, Type *t) {
    t = prune(t);
    if (t->kind == TY_VAR) {
        return t->var.id == var_id;
    }
    if (t->kind == TY_ARROW) {
        for (int i = 0; i < t->arrow.nparams; i++)
            if (occurs_check(var_id, t->arrow.params[i])) return 1;
        return occurs_check(var_id, t->arrow.ret);
    }
    if (t->kind == TY_ADT) {
        for (int i = 0; i < t->adt.nargs; i++)
            if (occurs_check(var_id, t->adt.args[i])) return 1;
    }
    return 0;
}

/* Returns 0 on success, -1 on failure (type mismatch). */
static int unify(Type *t1, Type *t2) {
    t1 = prune(t1);
    t2 = prune(t2);

    if (t1 == t2) return 0;

    if (t1->kind == TY_VAR) {
        if (t2->kind == TY_VAR && t1->var.id == t2->var.id) return 0;
        if (occurs_check(t1->var.id, t2)) return -1;
        t1->var.instance = t2;
        return 0;
    }
    if (t2->kind == TY_VAR) {
        if (occurs_check(t2->var.id, t1)) return -1;
        t2->var.instance = t1;
        return 0;
    }

    if (t1->kind == TY_CON && t2->kind == TY_CON) {
        return (strcmp(t1->con.name, t2->con.name) == 0) ? 0 : -1;
    }

    if (t1->kind == TY_ARROW && t2->kind == TY_ARROW) {
        if (t1->arrow.nparams != t2->arrow.nparams) return -1;
        for (int i = 0; i < t1->arrow.nparams; i++) {
            if (unify(t1->arrow.params[i], t2->arrow.params[i]) != 0) return -1;
        }
        return unify(t1->arrow.ret, t2->arrow.ret);
    }

    if (t1->kind == TY_ADT && t2->kind == TY_ADT) {
        if (strcmp(t1->adt.name, t2->adt.name) != 0) return -1;
        if (t1->adt.nargs != t2->adt.nargs) return -1;
        for (int i = 0; i < t1->adt.nargs; i++) {
            if (unify(t1->adt.args[i], t2->adt.args[i]) != 0) return -1;
        }
        return 0;
    }

    /* Different type structures → mismatch */
    return -1;
}

/* ============================================================
 * Generalization & Instantiation
 * ============================================================ */

/* Collect all non-quantified free type variables in t. */
static void collect_free_vars(Type *t, int *ids, int *count, int cap) {
    t = prune(t);
    if (t->kind == TY_VAR) {
        if (!t->var.quantified) {
            int found = 0;
            for (int i = 0; i < *count; i++) {
                if (ids[i] == t->var.id) { found = 1; break; }
            }
            if (!found && *count < cap) {
                ids[(*count)++] = t->var.id;
            }
        }
    } else if (t->kind == TY_ARROW) {
        for (int i = 0; i < t->arrow.nparams; i++)
            collect_free_vars(t->arrow.params[i], ids, count, cap);
        collect_free_vars(t->arrow.ret, ids, count, cap);
    } else if (t->kind == TY_ADT) {
        for (int i = 0; i < t->adt.nargs; i++)
            collect_free_vars(t->adt.args[i], ids, count, cap);
    }
}

/* Generalize: implemented below as generalize_type */

/* Instantiate: replace quantified vars with fresh vars. */
static Type *instantiate(TypeCtx *ctx, Type *t);

/* Map from var id → replacement Type for instantiation */
typedef struct {
    int var_id;
    Type *replacement;
} VarMap;

static Type *instantiate_rec(TypeCtx *ctx, Type *t, VarMap *map, int *map_count, int map_cap) {
    t = prune(t);
    if (t->kind == TY_VAR) {
        if (t->var.quantified) {
            /* Check if we already have a replacement */
            for (int i = 0; i < *map_count; i++) {
                if (map[i].var_id == t->var.id) return map[i].replacement;
            }
            /* Create fresh var */
            Type *fresh = type_new_var(ctx);
            if (*map_count < map_cap) {
                map[*map_count].var_id = t->var.id;
                map[*map_count].replacement = fresh;
                (*map_count)++;
            }
            return fresh;
        }
        return t;  /* non-quantified var: keep as-is */
    }
    if (t->kind == TY_CON) return t;
    if (t->kind == TY_ARROW) {
        Type **params = (Type **)malloc(sizeof(Type *) * (t->arrow.nparams > 0 ? t->arrow.nparams : 1));
        for (int i = 0; i < t->arrow.nparams; i++)
            params[i] = instantiate_rec(ctx, t->arrow.params[i], map, map_count, map_cap);
        Type *ret = instantiate_rec(ctx, t->arrow.ret, map, map_count, map_cap);
        return type_new_arrow(ctx, params, t->arrow.nparams, ret);
    }
    if (t->kind == TY_ADT) {
        Type **args = NULL;
        if (t->adt.nargs > 0) {
            args = (Type **)malloc(sizeof(Type *) * t->adt.nargs);
            for (int i = 0; i < t->adt.nargs; i++)
                args[i] = instantiate_rec(ctx, t->adt.args[i], map, map_count, map_cap);
        }
        Type *result = type_alloc(TY_ADT);
        result->adt.name = t->adt.name;
        result->adt.args = args;
        result->adt.nargs = t->adt.nargs;
        return result;
    }
    return t;
}

static Type *instantiate(TypeCtx *ctx, Type *t) {
    VarMap map[64];
    int map_count = 0;
    return instantiate_rec(ctx, t, map, &map_count, 64);
}

/* ============================================================
 * Type environment
 * ============================================================ */

typedef struct {
    const char *name;
    Type *type;    /* stored type (may be generalized) */
} TypeBinding;

struct TypeEnv {
    TypeBinding bindings[256];
    int count;
    TypeEnv *parent;
};

static TypeEnv *env_new(TypeEnv *parent) {
    TypeEnv *e = (TypeEnv *)calloc(1, sizeof(TypeEnv));
    e->parent = parent;
    e->count = 0;
    return e;
}

static void env_add(TypeEnv *e, const char *name, Type *t) {
    if (e->count < 256) {
        e->bindings[e->count].name = name;
        e->bindings[e->count].type = t;
        e->count++;
    }
}

static void env_free_node(TypeEnv *e) {
    /* Only free this one node — parent envs are shared and freed
     * separately at the top level. */
    if (e) free(e);
}

/* Look up a name in the environment chain. Returns instantiated type or NULL. */
static Type *env_lookup(TypeCtx *ctx, TypeEnv *e, const char *name) {
    while (e) {
        for (int i = e->count - 1; i >= 0; i--) {
            if (strcmp(e->bindings[i].name, name) == 0) {
                return instantiate(ctx, e->bindings[i].type);
            }
        }
        e = e->parent;
    }
    return NULL;
}

/* Proper generalize: mark vars free in t but not free in env as quantified */
static void mark_quantified(Type *t, int *mark_ids, int mark_count) {
    t = prune(t);
    if (t->kind == TY_VAR) {
        for (int i = 0; i < mark_count; i++) {
            if (t->var.id == mark_ids[i]) {
                t->var.quantified = 1;
                break;
            }
        }
    } else if (t->kind == TY_ARROW) {
        for (int i = 0; i < t->arrow.nparams; i++)
            mark_quantified(t->arrow.params[i], mark_ids, mark_count);
        mark_quantified(t->arrow.ret, mark_ids, mark_count);
    } else if (t->kind == TY_ADT) {
        for (int i = 0; i < t->adt.nargs; i++)
            mark_quantified(t->adt.args[i], mark_ids, mark_count);
    }
}

/* Proper generalize: mark vars free in t but not free in env as quantified */
/* Forward declare */
static void generalize_type_excluding(Type *t, TypeEnv *env, const char *exclude_name);

static void generalize_type(Type *t, TypeEnv *env) {
    generalize_type_excluding(t, env, NULL);
}

static void generalize_type_excluding(Type *t, TypeEnv *env, const char *exclude_name) {
    int env_ids[256];
    int env_count = 0;
    TypeEnv *e = env;
    while (e) {
        for (int i = 0; i < e->count; i++) {
            if (exclude_name && strcmp(e->bindings[i].name, exclude_name) == 0)
                continue;  /* skip the binding being generalized */
            collect_free_vars(e->bindings[i].type, env_ids, &env_count, 256);
        }
        e = e->parent;
    }

    int t_ids[256];
    int t_count = 0;
    collect_free_vars(t, t_ids, &t_count, 256);

    int mark_ids[256];
    int mark_count = 0;
    for (int i = 0; i < t_count; i++) {
        int in_env = 0;
        for (int j = 0; j < env_count; j++) {
            if (t_ids[i] == env_ids[j]) { in_env = 1; break; }
        }
        if (!in_env && mark_count < 256) {
            mark_ids[mark_count++] = t_ids[i];
        }
    }

    mark_quantified(t, mark_ids, mark_count);
}

/* ============================================================
 * AST helpers
 * ============================================================ */

static const char *tc_sym_name(VM *vm, Val v) {
    if (!val_is_symbol(v)) return NULL;
    uint32_t idx = val_get_symbol(v);
    if (idx >= (uint32_t)vm->sym_count) return NULL;
    return vm->symbols[idx];
}

static int tc_sym_eq(VM *vm, Val v, const char *s) {
    const char *n = tc_sym_name(vm, v);
    return n && strcmp(n, s) == 0;
}

static Val tc_car(Val v) {
    return val_is_pair(v) ? val_get_car(v) : val_nil();
}

static Val tc_cdr(Val v) {
    return val_is_pair(v) ? val_get_cdr(v) : val_nil();
}

static int tc_list_length(Val v) {
    int n = 0;
    while (val_is_pair(v)) { n++; v = val_get_cdr(v); }
    return n;
}

static Val tc_list_ref(Val v, int i) {
    for (int k = 0; k < i && val_is_pair(v); k++)
        v = val_get_cdr(v);
    return tc_car(v);
}

/* ============================================================
 * Error reporting
 * ============================================================ */

static int g_type_error = 0;

static void type_error(const char *msg, Type *t1, Type *t2) {
    if (g_type_error) return;  /* only report first error */
    g_type_error = 1;
    char buf1[256], buf2[256];
    type_show(t1, buf1, sizeof(buf1));
    type_show(t2, buf2, sizeof(buf2));
    fprintf(stderr, "Type error: %s: %s vs %s\n", msg, buf1, buf2);
}

static void type_error_msg(const char *msg) {
    if (g_type_error) return;
    g_type_error = 1;
    fprintf(stderr, "Type error: %s\n", msg);
}

/* ============================================================
 * Parse annotation string → Type
 * ============================================================ */

static Type *parse_annotation(TypeCtx *ctx, const char *s) {
    if (!s || !s[0]) return NULL;
    /* Trim leading/trailing whitespace */
    while (*s && (*s == ' ' || *s == '\t')) s++;
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t')) len--;
    if (len == 0) return NULL;

    if (len == 3 && strncmp(s, "int", 3) == 0) return ty_int(ctx);
    if (len == 4 && strncmp(s, "bool", 4) == 0) return ty_bool(ctx);
    if (len == 4 && strncmp(s, "nil", 3) == 0 && len == 3) return type_new_var(ctx);
    if (len == 3 && strncmp(s, "nil", 3) == 0) return type_new_var(ctx);
    if (len == 6 && strncmp(s, "string", 6) == 0) return ty_string(ctx);
    if (len == 3 && strncmp(s, "pid", 3) == 0) return ty_pid(ctx);
    if (len == 6 && strncmp(s, "symbol", 6) == 0) return ty_symbol(ctx);
    if (len == 5 && strncmp(s, "bytes", 5) == 0) return type_new_con("bytes");

    /* Unknown annotation → treat as fresh var (permissive) */
    return type_new_var(ctx);
}

/* ============================================================
 * Type inference for expressions
 * ============================================================ */

/* Check if a string is a known inline operation name. */
static int is_arith_op(const char *name) {
    return strcmp(name, "+") == 0 || strcmp(name, "-") == 0 ||
           strcmp(name, "*") == 0 || strcmp(name, "/") == 0 ||
           strcmp(name, "%") == 0;
}

static int is_cmp_op(const char *name) {
    return strcmp(name, "<") == 0 || strcmp(name, "<=") == 0 ||
           strcmp(name, ">") == 0 || strcmp(name, ">=") == 0;
}

static int is_eq_op(const char *name) {
    return strcmp(name, "=") == 0 || strcmp(name, "==") == 0;
}

static Type *infer_expr(TypeCtx *ctx, Val expr, TypeEnv *env) {
    VM *vm = ctx->vm;

    /* --- Integer literal --- */
    if (val_is_int(expr)) {
        return ty_int(ctx);
    }

    /* --- nil --- */
    if (val_is_nil(expr)) {
        /* Fresh var — nil can be anything (permissive) */
        return type_new_var(ctx);
    }

    /* --- true / false --- */
    if (expr == val_true()) return ty_bool(ctx);
    {
        Val false_val = ((uint64_t)TAG_FALSE << 48);
        if (expr == false_val) return ty_bool(ctx);
    }

    /* --- String literal --- */
    if (val_is_string(expr)) {
        return ty_string(ctx);
    }

    /* --- Symbol (variable reference) --- */
    if (val_is_symbol(expr)) {
        const char *name = tc_sym_name(vm, expr);
        if (!name) return type_new_var(ctx);
        Type *t = env_lookup(ctx, env, name);
        if (t) return t;
        /* Unbound symbol — permissive: return fresh var */
        return type_new_var(ctx);
    }

    /* --- Non-pair, non-immediate: fresh var --- */
    if (!val_is_pair(expr)) {
        return type_new_var(ctx);
    }

    /* --- Compound form --- */
    Val head = tc_car(expr);
    int nargs = tc_list_length(expr) - 1;

    /* (quote x) */
    if (tc_sym_eq(vm, head, "quote")) {
        Val quoted = tc_list_ref(expr, 1);
        if (val_is_symbol(quoted)) return ty_symbol(ctx);
        if (val_is_int(quoted)) return ty_int(ctx);
        if (val_is_nil(quoted)) return type_new_var(ctx);
        return type_new_var(ctx);
    }

    /* (define ...) — shouldn't appear as expression */
    if (tc_sym_eq(vm, head, "define") || tc_sym_eq(vm, head, "define_pub")) {
        return type_new_var(ctx);
    }

    /* (import ...) — compile-time directive */
    if (tc_sym_eq(vm, head, "import")) {
        return type_new_var(ctx);
    }

    /* (type) — ADT declaration, no-op */
    if (tc_sym_eq(vm, head, "type")) {
        return type_new_var(ctx);
    }

    /* (if cond then else?) */
    if (tc_sym_eq(vm, head, "if")) {
        Type *cond_t = infer_expr(ctx, tc_list_ref(expr, 1), env);
        unify(cond_t, ty_bool(ctx));  /* cond should be bool */

        Type *then_t = infer_expr(ctx, tc_list_ref(expr, 2), env);
        if (nargs >= 3) {
            Type *else_t = infer_expr(ctx, tc_list_ref(expr, 3), env);
            if (unify(then_t, else_t) != 0) {
                type_error("if branches have incompatible types", then_t, else_t);
            }
        }
        return then_t;
    }

    /* (and ...) (or ...) (begin ...) */
    if (tc_sym_eq(vm, head, "and") || tc_sym_eq(vm, head, "or")) {
        Type *result = ty_bool(ctx);
        for (int i = 1; i <= nargs; i++) {
            result = infer_expr(ctx, tc_list_ref(expr, i), env);
        }
        return result;
    }
    if (tc_sym_eq(vm, head, "begin")) {
        Val body = tc_cdr(expr);
        if (!val_is_pair(body)) return type_new_var(ctx);
        Type *result = type_new_var(ctx);
        while (val_is_pair(body)) {
            result = infer_expr(ctx, val_get_car(body), env);
            body = val_get_cdr(body);
        }
        return result;
    }

    /* (lambda (params...) body...) */
    if (tc_sym_eq(vm, head, "lambda")) {
        Val params = tc_list_ref(expr, 1);
        Val body = tc_cdr(tc_cdr(expr));

        int nparams = tc_list_length(params);
        Type **param_types = (Type **)malloc(sizeof(Type *) * (nparams > 0 ? nparams : 1));
        TypeEnv *lambda_env = env_new(env);

        int idx = 0;
        Val p = params;
        while (val_is_pair(p)) {
            const char *pname = tc_sym_name(vm, val_get_car(p));
            param_types[idx] = type_new_var(ctx);
            if (pname) env_add(lambda_env, pname, param_types[idx]);
            idx++;
            p = val_get_cdr(p);
        }

        Type *ret_type = type_new_var(ctx);
        Val b = body;
        while (val_is_pair(b)) {
            ret_type = infer_expr(ctx, val_get_car(b), lambda_env);
            b = val_get_cdr(b);
        }
        env_free_node(lambda_env);

        return type_new_arrow(ctx, param_types, nparams, ret_type);
    }

    /* (let var expr body...) */
    if (tc_sym_eq(vm, head, "let")) {
        Val second = tc_list_ref(expr, 1);
        if (val_is_symbol(second)) {
            const char *var_name = tc_sym_name(vm, second);
            Type *val_type = infer_expr(ctx, tc_list_ref(expr, 2), env);
            TypeEnv *let_env = env_new(env);
            generalize_type(val_type, env);
            if (var_name) env_add(let_env, var_name, val_type);

            Val body = tc_cdr(tc_cdr(tc_cdr(expr)));
            Type *result = type_new_var(ctx);
            while (val_is_pair(body)) {
                result = infer_expr(ctx, val_get_car(body), let_env);
                body = val_get_cdr(body);
            }
            env_free_node(let_env);
            return result;
        }
        /* (let ((v1 e1) ...) body...) */
        if (val_is_pair(second)) {
            TypeEnv *let_env = env_new(env);
            Val bindings = second;
            while (val_is_pair(bindings)) {
                Val binding = val_get_car(bindings);
                const char *vname = tc_sym_name(vm, tc_list_ref(binding, 0));
                Type *vt = infer_expr(ctx, tc_list_ref(binding, 1), env);
                generalize_type(vt, env);
                if (vname) env_add(let_env, vname, vt);
                bindings = val_get_cdr(bindings);
            }
            Val body = tc_cdr(tc_cdr(expr));
            Type *result = type_new_var(ctx);
            while (val_is_pair(body)) {
                result = infer_expr(ctx, val_get_car(body), let_env);
                body = val_get_cdr(body);
            }
            env_free_node(let_env);
            return result;
        }
        return type_new_var(ctx);
    }

    /* (match scrut (pat body...) ...) */
    if (tc_sym_eq(vm, head, "match")) {
        Type *scrut_t = infer_expr(ctx, tc_list_ref(expr, 1), env);
        Type *result_t = type_new_var(ctx);

        Val branches = tc_cdr(tc_cdr(expr));
        while (val_is_pair(branches)) {
            Val branch = val_get_car(branches);
            Val body = tc_cdr(branch);

            /* Infer body type (patterns are permissive — we don't bind
             * pattern variables to types for now) */
            Type *arm_t = infer_body(ctx, body, env);
            if (unify(result_t, arm_t) != 0) {
                type_error("match arms have incompatible types", result_t, arm_t);
            }

            branches = val_get_cdr(branches);
        }
        (void)scrut_t;  /* scrutinee type doesn't constrain anything */
        return result_t;
    }

    /* (receive (pat body...) ...) or (receive) */
    if (tc_sym_eq(vm, head, "receive")) {
        /* Messages can be any type — return fresh var */
        return type_new_var(ctx);
    }

    /* (spawn fn) */
    if (tc_sym_eq(vm, head, "spawn")) {
        /* Argument should be a function, but be permissive */
        Type *fn_t = infer_expr(ctx, tc_list_ref(expr, 1), env);
        (void)fn_t;
        return ty_pid(ctx);
    }

    /* (send pid msg) */
    if (tc_sym_eq(vm, head, "send")) {
        /* pid should be pid type, msg can be anything */
        Type *pid_t = infer_expr(ctx, tc_list_ref(expr, 1), env);
        (void)pid_t;  /* permissive: don't force pid type */
        if (nargs >= 2) {
            infer_expr(ctx, tc_list_ref(expr, 2), env);
        }
        return type_new_var(ctx);  /* send returns nil/ack */
    }

    /* (self) */
    if (tc_sym_eq(vm, head, "self")) {
        return ty_pid(ctx);
    }

    /* (monitor pid) */
    if (tc_sym_eq(vm, head, "monitor")) {
        if (nargs >= 1) {
            Type *pid_t = infer_expr(ctx, tc_list_ref(expr, 1), env);
            (void)pid_t;
        }
        return ty_int(ctx);
    }

    /* (recv) — bare receive without patterns */
    if (tc_sym_eq(vm, head, "recv")) {
        return type_new_var(ctx);
    }

    /* --- Inline operators --- */
    if (val_is_symbol(head)) {
        const char *op_name = tc_sym_name(vm, head);

        /* Arithmetic: +, -, *, /, % → int -> int -> int */
        if (is_arith_op(op_name)) {
            for (int i = 0; i < nargs; i++) {
                Type *arg_t = infer_expr(ctx, tc_list_ref(expr, 1 + i), env);
                if (unify(arg_t, ty_int(ctx)) != 0) {
                    type_error("arithmetic operand is not int", arg_t, ty_int(ctx));
                }
            }
            return ty_int(ctx);
        }

        /* Comparison: <, <=, >, >= → int -> int -> bool */
        if (is_cmp_op(op_name)) {
            for (int i = 0; i < nargs; i++) {
                Type *arg_t = infer_expr(ctx, tc_list_ref(expr, 1 + i), env);
                if (unify(arg_t, ty_int(ctx)) != 0) {
                    type_error("comparison operand is not int", arg_t, ty_int(ctx));
                }
            }
            return ty_bool(ctx);
        }

                /* Equality: =, == → accept any operand types (structural value equality),
         * result bool. We infer each operand for its side effects but do NOT
         * require them to share a type. */
        if (is_eq_op(op_name)) {
            for (int i = 0; i < nargs; i++) {
                infer_expr(ctx, tc_list_ref(expr, 1 + i), env);
            }
            return ty_bool(ctx);
        }

        /* cons → fresh var (permissive) */
        if (strcmp(op_name, "cons") == 0) {
            for (int i = 0; i < nargs; i++)
                infer_expr(ctx, tc_list_ref(expr, 1 + i), env);
            return type_new_var(ctx);
        }

        /* car, cdr → fresh var (permissive) */
        if (strcmp(op_name, "car") == 0 || strcmp(op_name, "cdr") == 0) {
            for (int i = 0; i < nargs; i++)
                infer_expr(ctx, tc_list_ref(expr, 1 + i), env);
            return type_new_var(ctx);
        }

        /* null?, pair?, int?, string?, bytes?, pid? → bool */
        if (strcmp(op_name, "null?") == 0 || strcmp(op_name, "pair?") == 0 ||
            strcmp(op_name, "int?") == 0 || strcmp(op_name, "string?") == 0 ||
            strcmp(op_name, "bytes?") == 0 || strcmp(op_name, "pid?") == 0) {
            for (int i = 0; i < nargs; i++)
                infer_expr(ctx, tc_list_ref(expr, 1 + i), env);
            return ty_bool(ctx);
        }

        /* string-length → string -> int */
        if (strcmp(op_name, "string-length") == 0) {
            for (int i = 0; i < nargs; i++)
                infer_expr(ctx, tc_list_ref(expr, 1 + i), env);
            return ty_int(ctx);
        }

        /* string-concat → string -> string -> string */
        if (strcmp(op_name, "string-concat") == 0) {
            for (int i = 0; i < nargs; i++)
                infer_expr(ctx, tc_list_ref(expr, 1 + i), env);
            return ty_string(ctx);
        }

        /* string-slice → string -> int -> int -> string */
        if (strcmp(op_name, "string-slice") == 0) {
            for (int i = 0; i < nargs; i++)
                infer_expr(ctx, tc_list_ref(expr, 1 + i), env);
            return ty_string(ctx);
        }

        /* string-eq → string -> string -> bool */
        if (strcmp(op_name, "string-eq") == 0) {
            for (int i = 0; i < nargs; i++)
                infer_expr(ctx, tc_list_ref(expr, 1 + i), env);
            return ty_bool(ctx);
        }

        /* print → any -> nil (permissive) */
        if (strcmp(op_name, "print") == 0) {
            for (int i = 0; i < nargs; i++)
                infer_expr(ctx, tc_list_ref(expr, 1 + i), env);
            return type_new_var(ctx);  /* returns nil, but fresh var is safe */
        }
    }

    /* --- General function call: (fn arg1 arg2 ...) --- */
    if (val_is_symbol(head)) {
        const char *fn_name = tc_sym_name(vm, head);

        /* Look up function type in environment */
        Type *fn_type = env_lookup(ctx, env, fn_name);
        if (fn_type) {
            /* Unify with arrow type and check args */
            if (fn_type->kind == TY_ARROW) {
                int expected_nargs = fn_type->arrow.nparams;
                for (int i = 0; i < nargs && i < expected_nargs; i++) {
                    Type *arg_t = infer_expr(ctx, tc_list_ref(expr, 1 + i), env);
                    if (unify(arg_t, fn_type->arrow.params[i]) != 0) {
                        type_error("argument type mismatch", arg_t, fn_type->arrow.params[i]);
                    }
                }
                return fn_type->arrow.ret;
            }
            /* If not arrow, unify with an arrow type (permissive) */
            Type **params = (Type **)malloc(sizeof(Type *) * (nargs > 0 ? nargs : 1));
            for (int i = 0; i < nargs; i++) {
                Type *arg_t = infer_expr(ctx, tc_list_ref(expr, 1 + i), env);
                params[i] = arg_t;
            }
            Type *ret = type_new_var(ctx);
            Type *arrow = type_new_arrow(ctx, params, nargs, ret);
            if (unify(fn_type, arrow) != 0) {
                /* Permissive: return fresh var instead of erroring */
            }
            return ret;
        }
    }

    /* --- Unknown function call: infer args, return fresh var --- */
    {
        infer_expr(ctx, head, env);
        for (int i = 0; i < nargs; i++)
            infer_expr(ctx, tc_list_ref(expr, 1 + i), env);
        return type_new_var(ctx);
    }
}

/* Infer type of a body (sequence of expressions, last one determines type) */
static Type *infer_body(TypeCtx *ctx, Val body, TypeEnv *env) {
    if (!val_is_pair(body)) return type_new_var(ctx);
    Type *result = type_new_var(ctx);
    while (val_is_pair(body)) {
        result = infer_expr(ctx, val_get_car(body), env);
        body = val_get_cdr(body);
    }
    return result;
}

/* ============================================================
 * Function definition checking
 * ============================================================ */

/* External: get annotations from reader */
extern FnAnnotation *reader_get_annotations(int *count);

static void check_define(TypeCtx *ctx, Val form, TypeEnv *global_env) {
    VM *vm = ctx->vm;
    Val sig = tc_list_ref(form, 1);
    if (!val_is_pair(sig)) return;

    Val name_val = tc_car(sig);
    if (!val_is_symbol(name_val)) return;
    const char *fn_name = tc_sym_name(vm, name_val);

    /* Get annotations */
    FnAnnotation *annos = NULL;
    int anno_count = 0;
    annos = reader_get_annotations(&anno_count);

    FnAnnotation *my_anno = NULL;
    for (int i = 0; i < anno_count; i++) {
        if (strcmp(annos[i].name, fn_name) == 0) {
            my_anno = &annos[i];
            break;
        }
    }

    /* Build param types */
    Val params = tc_cdr(sig);
    int nparams = tc_list_length(params);

        /* Look up the STORED function type (not instantiated — we need to
     * constrain the actual type, not a fresh copy). */
    Type *fn_type = NULL;
    {
        TypeEnv *e = global_env;
        while (e) {
            for (int i = e->count - 1; i >= 0; i--) {
                if (strcmp(e->bindings[i].name, fn_name) == 0) {
                    fn_type = e->bindings[i].type;  /* direct pointer, no instantiate */
                    break;
                }
            }
            break;  /* global_env has no parent */
        }
    }
    if (!fn_type) return;  /* shouldn't happen */

    /* Get parameter types from pre-registered type */
    Type **param_types = NULL;
    if (fn_type->kind == TY_ARROW && fn_type->arrow.nparams == nparams) {
        param_types = fn_type->arrow.params;
    }

    /* Build function body environment */
    TypeEnv *fn_env = env_new(global_env);
    int idx = 0;
    Val p = params;
    while (val_is_pair(p)) {
        const char *pname = tc_sym_name(vm, val_get_car(p));
        if (pname && param_types) {
            env_add(fn_env, pname, param_types[idx]);
        }
        idx++;
        p = val_get_cdr(p);
    }

    /* Infer body type */
    Val body = tc_cdr(tc_cdr(form));
    Type *body_type = infer_body(ctx, body, fn_env);
    env_free_node(fn_env);

    /* If we have annotations, check them */
    if (my_anno) {
        /* Check return type annotation */
        if (my_anno->has_ret_annotation) {
            Type *annotated_ret = parse_annotation(ctx, my_anno->ret_type);
            if (annotated_ret && annotated_ret->kind == TY_CON) {
                Type *pruned_body = prune(body_type);
                if (pruned_body->kind == TY_VAR) {
                    /* Unannotated body: bind to annotation */
                    if (pruned_body->kind == TY_VAR) {
                        pruned_body->var.instance = annotated_ret;
                    }
                } else {
                    /* Check body matches annotation */
                    if (unify(body_type, annotated_ret) != 0) {
                        type_error("return type mismatch", body_type, annotated_ret);
                    }
                }
            }
        }
    }

    /* Unify body type with function return type */
    if (fn_type->kind == TY_ARROW) {
        if (unify(fn_type->arrow.ret, body_type) != 0) {
            /* Permissive: don't report this error for now */
        }
    }
}

/* ============================================================
 * Top-level program checking
 * ============================================================ */

int typecheck_program(VM *vm, Val forms) {
    TypeCtx ctx;
    ctx.vm = vm;
    ctx.next_var_id = 0;
    g_type_error = 0;

    /* Build global environment: pre-register all function types */
    TypeEnv *global_env = env_new(NULL);

    /* First pass: create arrow types for all defined functions */
    {
        Val cur = forms;
        while (val_is_pair(cur)) {
            Val form = val_get_car(cur);
            if (val_is_pair(form)) {
                Val head = tc_car(form);
                if (tc_sym_eq(vm, head, "define") || tc_sym_eq(vm, head, "define_pub")) {
                    Val sig = tc_list_ref(form, 1);
                    if (val_is_pair(sig)) {
                        Val name_val = tc_car(sig);
                        if (val_is_symbol(name_val)) {
                            const char *fn_name = tc_sym_name(vm, name_val);
                            Val params = tc_cdr(sig);
                            int nparams = tc_list_length(params);
                            Type **param_types = (Type **)malloc(sizeof(Type *) * (nparams > 0 ? nparams : 1));
                            for (int i = 0; i < nparams; i++) {
                                param_types[i] = type_new_var(&ctx);
                            }
                            Type *ret_type = type_new_var(&ctx);
                            Type *arrow = type_new_arrow(&ctx, param_types, nparams, ret_type);
                            env_add(global_env, fn_name, arrow);
                        }
                    }
                }
            }
            cur = val_get_cdr(cur);
        }
    }

    /* Second pass: infer function bodies */
    {
        Val cur = forms;
        while (val_is_pair(cur)) {
            Val form = val_get_car(cur);
            if (val_is_pair(form)) {
                                Val head = tc_car(form);
                if (tc_sym_eq(vm, head, "define") || tc_sym_eq(vm, head, "define_pub")) {
                    check_define(&ctx, form, global_env);
                    /* Generalize this function's type immediately so that
                     * later functions calling it get polymorphic instantiation. */
                    Val sig = tc_list_ref(form, 1);
                    if (val_is_pair(sig)) {
                        const char *fn_name = tc_sym_name(vm, tc_car(sig));
                        if (fn_name) {
                            for (int i = 0; i < global_env->count; i++) {
                                if (strcmp(global_env->bindings[i].name, fn_name) == 0) {
                                                                        generalize_type_excluding(global_env->bindings[i].type, global_env, fn_name);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            cur = val_get_cdr(cur);
        }
    }

    /* Third pass: check top-level non-define expressions */
    {
        Val cur = forms;
        while (val_is_pair(cur)) {
            Val form = val_get_car(cur);
            if (val_is_pair(form)) {
                Val head = tc_car(form);
                int is_define = tc_sym_eq(vm, head, "define") || tc_sym_eq(vm, head, "define_pub");
                int is_import = tc_sym_eq(vm, head, "import");
                int is_type = tc_sym_eq(vm, head, "type");
                if (!is_define && !is_import && !is_type) {
                    infer_expr(&ctx, form, global_env);
                }
            }
            cur = val_get_cdr(cur);
        }
    }

    int result = g_type_error ? -1 : 0;

    env_free_node(global_env);
    type_free_all();

    return result;
}