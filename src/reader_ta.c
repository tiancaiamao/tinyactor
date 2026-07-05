/* reader_ta.c — parses ML/Rust-style .ta syntax into pair-tree AST.
 *
 * Grammar -> AST mapping is documented in the task spec. Returns one
 * top-level form per call to reader_ta_read().
 */

#include "ta.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

/* ---- Local struct for annotation capture (replaces former typecheck.h FnAnnotation) ---- */
typedef struct {
    char param_types[16][64];  /* type annotation strings per param */
    int  nparams;
    char ret_type[64];          /* return type annotation */
    int  has_ret_annotation;
} Anno;

/* ---- provided by val.c ------------------------------------------------ */
Val val_int(int64_t i);
Val val_nil(void);
Val val_true(void);
Val val_false(void);
Val val_symbol(uint32_t idx);
Val val_pair(Proc *p, Val car, Val cdr);
Val val_string(Proc *p, const char *data, int len);
int  val_is_nil(Val v);
int  val_is_pair(Val v);
Val  val_get_car(Val v);
Val  val_get_cdr(Val v);

/* static inline HeapPair *val_as_pair(Val v);  -- from ta.h */

/* ---- scratch proc (same pattern as reader.c) -------------------------- */
/* The scratch proc holds the full pair-tree AST while a module is being
 * parsed. It has no GC roots (sp=0, no gc_roots), so a GC would clobber
 * the in-progress AST — it must be sized large enough that GC never
 * triggers. gc_to is allocated defensively so proc_grow cannot crash. */
static Proc *get_scratch(void) {
    static Proc *sp = NULL;
    if (!sp) {
        sp = calloc(1, sizeof(Proc));
        sp->mem_size = 1 << 23;        /* 8 MiB: covers large self-hosted modules */
        sp->mem = malloc(sp->mem_size);
        sp->gc_to = malloc(sp->mem_size);
        sp->sp = 0;
    }
    return sp;
}

/* ---- symbol interning (copy of reader.c's static version) ------------- */
static uint32_t intern_sym(VM *vm, const char *s, int len) {
    for (int i = 0; i < vm->sym_count; i++) {
        if ((int)strlen(vm->symbols[i]) == len &&
            memcmp(vm->symbols[i], s, len) == 0)
            return (uint32_t)i;
    }
    if (vm->sym_count >= vm->sym_cap) {
        vm->sym_cap = vm->sym_cap ? vm->sym_cap * 2 : 64;
        vm->symbols = realloc(vm->symbols, sizeof(char *) * vm->sym_cap);
    }
    char *cpy = malloc(len + 1);
    memcpy(cpy, s, len);
    cpy[len] = '\0';
    uint32_t idx = (uint32_t)vm->sym_count;
    vm->symbols[vm->sym_count++] = cpy;
    return idx;
}

static Val sym(VM *vm, const char *name) {
    return val_symbol(intern_sym(vm, name, (int)strlen(name)));
}

/* ---- list builder ----------------------------------------------------- */
static Val mk_list(Proc *sp, Val *items, int n) {
    Val result = val_nil();
    for (int i = n - 1; i >= 0; i--)
        result = val_pair(sp, items[i], result);
    return result;
}

/* Build (quote name) as a pair tree */
static Val mk_quoted(Proc *sp, VM *vm, const char *name, int len) {
    Val q = val_symbol(intern_sym(vm, "quote", 5));
    Val s = val_symbol(intern_sym(vm, name, len));
    return val_pair(sp, q, val_pair(sp, s, val_nil()));
}

/* ---- ADT constructor registry (sugar over pair trees) ---------------- */
typedef struct {
    char name[64];                /* Type name: "Msg"                    */
    char variants[32][64];        /* Variant names: "Ping", "Pong", ...  */
    int  variant_arities[32];     /* Number of params per variant        */
    int  n_variants;
} TypeInfo;

static TypeInfo types[64];
static int n_types = 0;

/* ---- Type annotation capture (converted to type-sig form in AST for the .ta typechecker) ---- */

static int is_upper_ident(const char *s, int len) {
    if (len <= 0) return 0;
    return isupper((unsigned char)s[0]);
}

static int is_constructor(const char *name, int len) {
    /* Search registered constructors */
    for (int t = 0; t < n_types; t++)
        for (int v = 0; v < types[t].n_variants; v++) {
            int vn = (int)strlen(types[t].variants[v]);
            if (vn == len && memcmp(types[t].variants[v], name, len) == 0)
                return 1;
        }
    /* Fallback: uppercase convention */
    return is_upper_ident(name, len);
}

/* Returns 1 if found, fills out_arity. Returns 0 if not a registered constructor. */
static int find_ctor_info(const char *name, int len, int *out_arity) {
    for (int t = 0; t < n_types; t++)
        for (int v = 0; v < types[t].n_variants; v++) {
            int vn = (int)strlen(types[t].variants[v]);
            if (vn == len && memcmp(types[t].variants[v], name, len) == 0) {
                if (out_arity) *out_arity = types[t].variant_arities[v];
                return 1;
            }
        }
    return 0;
}

/* ---- pattern inspection for exhaustiveness --------------------------- */

static int pat_is_wildcard(VM *vm, Val pat) {
    if (!val_is_symbol(pat)) return 0;
    uint32_t idx = val_get_symbol(pat);
    if (idx >= (uint32_t)vm->sym_count) return 0;
    return strcmp(vm->symbols[idx], "_") == 0;
}

/* Returns constructor name (pointer into vm->symbols) or NULL.
 * Handles nullary (quote Foo) and n-ary [(quote Foo), ...] patterns. */
static const char *pat_ctor_name(VM *vm, Val pat) {
    if (!val_is_pair(pat)) return NULL;
    Val head = val_get_car(pat);

    /* Case 1: nullary pattern (quote Foo): head is sym("quote") */
    if (val_is_symbol(head)) {
        uint32_t idx = val_get_symbol(head);
        if (idx < (uint32_t)vm->sym_count &&
            strcmp(vm->symbols[idx], "quote") == 0) {
            Val inner = val_get_car(val_get_cdr(pat));
            if (val_is_symbol(inner)) {
                uint32_t iidx = val_get_symbol(inner);
                if (iidx < (uint32_t)vm->sym_count)
                    return vm->symbols[iidx];
            }
        }
    }

    /* Case 2: n-ary pattern: head is itself (quote Foo) */
    if (val_is_pair(head)) {
        Val hhead = val_get_car(head);
        if (val_is_symbol(hhead)) {
            uint32_t idx = val_get_symbol(hhead);
            if (idx < (uint32_t)vm->sym_count &&
                strcmp(vm->symbols[idx], "quote") == 0) {
                Val inner = val_get_car(val_get_cdr(head));
                if (val_is_symbol(inner)) {
                    uint32_t iidx = val_get_symbol(inner);
                    if (iidx < (uint32_t)vm->sym_count)
                        return vm->symbols[iidx];
                }
            }
        }
    }

    return NULL;
}

/* Check if a match/receive's arms cover all ADT variants.
 * `arms` is a pair list where each element is (pattern body). */
static void check_exhaustiveness(VM *vm, Val arms, const char *kw_name) {
    int has_wildcard = 0;
    char found_ctors[64][64];
    int n_found = 0;

    Val cur = arms;
    while (val_is_pair(cur)) {
        Val arm = val_get_car(cur);
        Val pat = val_get_car(arm);  /* first element of (pattern body) */

        if (pat_is_wildcard(vm, pat)) {
            has_wildcard = 1;
        } else {
            const char *cn = pat_ctor_name(vm, pat);
            if (cn && n_found < 64) {
                strncpy(found_ctors[n_found], cn, 63);
                found_ctors[n_found][63] = '\0';
                n_found++;
            }
        }

        cur = val_get_cdr(cur);
    }

    if (has_wildcard) return;  /* exhaustive */

    /* For each registered type, check coverage only if a variant appears. */
    for (int t = 0; t < n_types; t++) {
        int type_used = 0;
        for (int v = 0; v < types[t].n_variants && !type_used; v++) {
            for (int f = 0; f < n_found; f++) {
                if (strcmp(types[t].variants[v], found_ctors[f]) == 0) {
                    type_used = 1;
                    break;
                }
            }
        }
        if (!type_used) continue;  /* this type not relevant to this match */

        for (int v = 0; v < types[t].n_variants; v++) {
            int present = 0;
            for (int f = 0; f < n_found; f++) {
                if (strcmp(types[t].variants[v], found_ctors[f]) == 0) {
                    present = 1;
                    break;
                }
            }
            if (!present) {
                fprintf(stderr, "warning: non-exhaustive %s: missing %s\n",
                        kw_name, types[t].variants[v]);
            }
        }
    }
}

/* ====================================================================== */
/* Tokenizer                                                              */
/* ====================================================================== */

typedef struct {
    const char *src;
    int   len;
    int   pos;
    int   had_newline;   /* set when a newline was consumed skipping ws */
} Lex;

static int is_ident_start(int c) {
    return isalpha(c) || c == '_';
}
static int is_ident_char(int c) {
    return isalnum(c) || c=='_' || c=='.' || c=='/' ||
           c=='+' || c=='-' || c=='<' || c=='>' ||
           c=='=' || c=='!' || c=='?' || c=='*' ||
           c=='%';
}

static void skip_ws(Lex *lx) {
    lx->had_newline = 0;
    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            lx->pos++;
        } else if (c == '\n') {
            lx->pos++;
            lx->had_newline = 1;
        } else if (c == '/' && lx->pos + 1 < lx->len &&
                   lx->src[lx->pos+1] == '/') {
            /* line comment to EOL */
            while (lx->pos < lx->len && lx->src[lx->pos] != '\n')
                lx->pos++;
        } else {
            break;
        }
    }
}

/* peek the next non-ws char (without consuming), -1 at EOF */
static int peek_char(Lex *lx) {
    Lex t = *lx;
    skip_ws(&t);
    if (t.pos >= t.len) return -1;
    return (unsigned char)t.src[t.pos];
}

/* ====================================================================== */
/* Forward declarations                                                   */
/* ====================================================================== */
static Val parse_expr(Lex *lx, VM *vm, Proc *sp);
static Val parse_form(Lex *lx, VM *vm, Proc *sp);
static Val parse_pattern(Lex *lx, VM *vm, Proc *sp);
static Val parse_atom_or_call(Lex *lx, VM *vm, Proc *sp);
static Val parse_braced(Lex *lx, VM *vm, Proc *sp);
static Val parse_call_args(Lex *lx, VM *vm, Proc *sp, Val head);
static int is_keyword(Lex *lx, const char *kw);

/* ====================================================================== */
/* Atoms                                                                  */
/* ====================================================================== */

/* parse a string literal starting at src[pos]=='"' ; advance past closing " */
static Val parse_string_lit(Lex *lx, VM *vm, Proc *sp) {
    lx->pos++; /* skip opening quote */
    /* Build decoded string handling escape sequences */
    char *buf = NULL;
    int buflen = 0, bufcap = 0;
    while (lx->pos < lx->len && lx->src[lx->pos] != '"') {
        char c = lx->src[lx->pos];
        if (c == '\\' && lx->pos + 1 < lx->len) {
            lx->pos++;
            char esc = lx->src[lx->pos];
            switch (esc) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '\\': c = '\\'; break;
                case '"': c = '"'; break;
                case '0': c = '\0'; break;
                default: c = esc; break;
            }
        }
        if (buflen >= bufcap) {
            bufcap = bufcap ? bufcap * 2 : 16;
            buf = realloc(buf, bufcap);
        }
        buf[buflen++] = c;
        lx->pos++;
    }
    if (lx->pos < lx->len) lx->pos++; /* skip closing quote */
    Val result = val_string(sp, buf ? buf : "", buflen);
    if (buf) free(buf);
    return result;
}

/* parse an identifier token starting at lx->pos. Returns malloc'd copy
 * in *out, *outlen; caller frees. */
static char *read_ident(Lex *lx, int *outlen) {
    int start = lx->pos;
    if (lx->pos < lx->len && is_ident_start((unsigned char)lx->src[lx->pos])) {
        while (lx->pos < lx->len && is_ident_char((unsigned char)lx->src[lx->pos]))
            lx->pos++;
    }
    int n = lx->pos - start;
    char *s = malloc(n + 1);
    memcpy(s, lx->src + start, n);
    s[n] = '\0';
    if (outlen) *outlen = n;
    return s;
}

static Val parse_integer(Lex *lx) {
    int start = lx->pos;
    while (lx->pos < lx->len && isdigit((unsigned char)lx->src[lx->pos]))
        lx->pos++;
    int n = lx->pos - start;
    char buf[32];
    if (n > 31) n = 31;
    memcpy(buf, lx->src + start, n);
    buf[n] = '\0';
    return val_int((int64_t)strtoll(buf, NULL, 10));
}

/* parse optional '-' then digits (handles negative literals) */
static Val parse_signed_int(Lex *lx) {
    int start = lx->pos;
    if (lx->pos < lx->len && lx->src[lx->pos] == '-') lx->pos++;
    while (lx->pos < lx->len && isdigit((unsigned char)lx->src[lx->pos]))
        lx->pos++;
    int n = lx->pos - start;
    char buf[32];
    if (n > 31) n = 31;
    memcpy(buf, lx->src + start, n);
    buf[n] = '\0';
    return val_int((int64_t)strtoll(buf, NULL, 10));
}

/* ====================================================================== */
/* Patterns (inside match/receive arms)                                   */
/* ====================================================================== */

static Val parse_pattern_list(Lex *lx, VM *vm, Proc *sp) {
    /* we are just past '[' */
    Val head = val_nil();
    Val *tail = &head;
    for (;;) {
        int c = peek_char(lx);
        if (c == ']' || c == -1) {
            skip_ws(lx);
            if (lx->pos < lx->len && lx->src[lx->pos] == ']') lx->pos++;
            break;
        }
        Val p = parse_pattern(lx, vm, sp);
        Val cell = val_pair(sp, p, val_nil());
        *tail = cell;
        tail = &((HeapPair *)val_as_pair(cell))->cdr;
        skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == ',') {
            lx->pos++;
        }
    }
    return head;
}

static Val parse_pattern(Lex *lx, VM *vm, Proc *sp) {
    skip_ws(lx);
    if (lx->pos >= lx->len) return val_nil();
    char c = lx->src[lx->pos];

    if (c == '\'') {
        lx->pos++;
        int n;
        char *id = read_ident(lx, &n);
        Val q = mk_list(sp, (Val[]){ sym(vm, "quote"), val_symbol(intern_sym(vm, id, n)) }, 2);
        free(id);
        return q;
    }
    if (c == '"') return parse_string_lit(lx, vm, sp);
    if (c == '[') {
        lx->pos++;
        return parse_pattern_list(lx, vm, sp);
    }
        if (c == '-' && lx->pos+1 < lx->len && isdigit((unsigned char)lx->src[lx->pos+1])) {
        return parse_signed_int(lx);
    }
    if (isdigit((unsigned char)c)) return parse_integer(lx);

            /* identifier (includes _ , nil, true, false, constructors) */
    int n;
    char *id = read_ident(lx, &n);

    /* cons pair pattern: cons(a, b) → (cons pat_a pat_b) for MATCH_PAIR */
    if (n == 4 && memcmp(id, "cons", 4) == 0) {
        skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == '(') {
            lx->pos++;
            Val items[3];
            int cnt = 0;
            items[cnt++] = sym(vm, "cons");
            for (;;) {
                skip_ws(lx);
                if (lx->pos >= lx->len) break;
                if (lx->src[lx->pos] == ')') { lx->pos++; break; }
                if (cnt < 3) items[cnt++] = parse_pattern(lx, vm, sp);
                skip_ws(lx);
                if (lx->pos < lx->len && lx->src[lx->pos] == ',') lx->pos++;
            }
            free(id);
            return mk_list(sp, items, cnt);
        }
    }

    /* constructor pattern */
    if (is_constructor(id, n)) {
        skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == '(') {
            /* n-ary: [(quote Name), subpat1, ...] */
            lx->pos++;
            Val items[16];
            int cnt = 0;
            items[cnt++] = mk_quoted(sp, vm, id, n);
            for (;;) {
                skip_ws(lx);
                if (lx->pos >= lx->len) break;
                if (lx->src[lx->pos] == ')') { lx->pos++; break; }
                if (cnt < 16) items[cnt++] = parse_pattern(lx, vm, sp);
                skip_ws(lx);
                if (lx->pos < lx->len && lx->src[lx->pos] == ',') lx->pos++;
            }
                        Val lst = mk_list(sp, items, cnt);
            {
                int expected_arity = -1;
                find_ctor_info(id, n, &expected_arity);
                int actual_arity = cnt - 1;  /* minus the quoted head */
                if (expected_arity >= 0 && actual_arity != expected_arity) {
                    fprintf(stderr, "error: pattern %.*s expects %d args, got %d\n",
                            n, id, expected_arity, actual_arity);
                }
            }
            free(id);
            return lst;
        }
        /* nullary: (quote Name) */
        Val v = mk_quoted(sp, vm, id, n);
        free(id);
        return v;
    }

    Val result;
    if (n == 3 && memcmp(id, "nil", 3) == 0)          result = val_nil();
    else if (n == 4 && memcmp(id, "true", 4) == 0)    result = val_true();
    else if (n == 5 && memcmp(id, "false", 5) == 0)   result = val_false();
    else                                              result = val_symbol(intern_sym(vm, id, n));
    free(id);
    return result;
}

/* ====================================================================== */
/* Binary ops                                                             */
/* ====================================================================== */

/* match a multi-char operator starting at lx->pos; returns matched length
 * (0 if none) and writes the symbol name into buf (max 4 chars) */
static int match_op(Lex *lx, char *buf) {
    const char *ops[] = {"<=", ">=", "==", "+", "-", "*", "/", "%", "<", ">", NULL};
    for (int i = 0; ops[i]; i++) {
        int n = (int)strlen(ops[i]);
        if (lx->pos + n <= lx->len && memcmp(lx->src + lx->pos, ops[i], n) == 0) {
            /* guard: '=' is not an op by itself; '==' yes */
            if (ops[i][0] == '=' && n == 1) continue;
            /* guard: '-' is not an op when followed by '>' (-> arrow) */
            if (ops[i][0] == '-' && n == 1 &&
                lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '>')
                continue;
            /* translate == to = for compiler compatibility */
            if (strcmp(ops[i], "==") == 0) {
                buf[0] = '='; buf[1] = '\0';
            } else {
                memcpy(buf, ops[i], n);
                buf[n] = '\0';
            }
            return n;
        }
    }
    return 0;
}

/* parse an operand: atom, call, parenthesized expr, quoted, string, braced block */
static Val parse_operand(Lex *lx, VM *vm, Proc *sp) {
    skip_ws(lx);
    if (lx->pos >= lx->len) return val_nil();
    char c = lx->src[lx->pos];

        if (c == '\'') {
        lx->pos++;
        int n;
        char *id = read_ident(lx, &n);
        /* If read_ident returned empty (e.g., '+, '-, '* , '<=), read
           operator chars as the symbol name. is_ident_start requires
           alpha/underscore, so operator-prefixed symbols are missed. */
        if (n == 0 && lx->pos < lx->len &&
            is_ident_char((unsigned char)lx->src[lx->pos])) {
            int start = lx->pos;
            while (lx->pos < lx->len && is_ident_char((unsigned char)lx->src[lx->pos]))
                lx->pos++;
            n = lx->pos - start;
            free(id);
            id = malloc(n + 1);
            memcpy(id, lx->src + start, n);
            id[n] = '\0';
        }
        Val v = mk_list(sp, (Val[]){ sym(vm, "quote"), val_symbol(intern_sym(vm, id, n)) }, 2);
        free(id);
        return v;
    }
    if (c == '"') return parse_string_lit(lx, vm, sp);
    if (c == '(') {
        lx->pos++;
        Val e = parse_expr(lx, vm, sp);
        skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == ')') lx->pos++;
        return e;
    }
    if (c == '{') {
        return parse_braced(lx, vm, sp);
    }
        if (c == '-' && lx->pos+1 < lx->len && isdigit((unsigned char)lx->src[lx->pos+1])) {
        return parse_signed_int(lx);
    }
    if (isdigit((unsigned char)c)) {
        return parse_integer(lx);
    }
    /* `fn { body }` or `fn(params) { body }` -> lambda (used by spawn) */
        /* `if cond { then } else { else }` — if expression in operand position */
    if (is_ident_start((unsigned char)c) && is_keyword(lx, "if")) {
        lx->pos += 2;
        Val cond = parse_expr(lx, vm, sp);
        Val then_b = parse_braced(lx, vm, sp);
        Val else_b = val_nil();
        int save = lx->pos;
        int save_nl = lx->had_newline;
        skip_ws(lx);
        if (lx->pos < lx->len && is_keyword(lx, "else")) {
            lx->pos += 4;
            else_b = parse_braced(lx, vm, sp);
        } else {
            lx->pos = save;
            lx->had_newline = save_nl;
        }
        return mk_list(sp, (Val[]){ sym(vm, "if"), cond, then_b, else_b }, 4);
    }
    /* `match scrut { ... }` — match expression in operand position */
    if (is_ident_start((unsigned char)c) && is_keyword(lx, "match")) {
        /* delegate to parse_form's match handler by re-entering parse_form */
        lx->pos -= 0; /* stay at current pos; parse_form will re-read */
        return parse_form(lx, vm, sp);
    }
    if (is_ident_start((unsigned char)c) && is_keyword(lx, "fn")) {
        lx->pos += 2;
        skip_ws(lx);
        Val params = val_nil();
        if (lx->pos < lx->len && lx->src[lx->pos] == '(') {
            lx->pos++;
            Val *ptail = &params;
            for (;;) {
                skip_ws(lx);
                if (lx->pos >= lx->len || lx->src[lx->pos] == ')') {
                    if (lx->pos < lx->len) lx->pos++;
                    break;
                }
                int pn;
                char *p = read_ident(lx, &pn);
                Val ps = val_symbol(intern_sym(vm, p, pn));
                                free(p);
                /* optional type annotation ": Type" */
                skip_ws(lx);
                if (lx->pos < lx->len && lx->src[lx->pos] == ':') {
                    lx->pos++;
                    skip_ws(lx);
                    while (lx->pos < lx->len && lx->src[lx->pos] != ',' && lx->src[lx->pos] != ')')
                        lx->pos++;
                }
                Val cell = val_pair(sp, ps, val_nil());
                *ptail = cell;
                ptail = &((HeapPair *)val_as_pair(cell))->cdr;
                skip_ws(lx);
                if (lx->pos < lx->len && lx->src[lx->pos] == ',') lx->pos++;
            }
        }
        Val body = parse_braced(lx, vm, sp);
        return mk_list(sp, (Val[]){ sym(vm, "lambda"), params, body }, 3);
    }
            if (is_ident_start((unsigned char)c)) {
        return parse_atom_or_call(lx, vm, sp);
    }
    /* unknown char: consume to avoid infinite loop */
    lx->pos++;
    return val_nil();
}

static Val parse_expr(Lex *lx, VM *vm, Proc *sp) {
    Val left = parse_operand(lx, vm, sp);
    for (;;) {
        /* remember position; ops may be preceded by spaces */
        int save = lx->pos;
        int save_nl = lx->had_newline;
        skip_ws(lx);
        if (lx->pos >= lx->len) break;
        char opbuf[4];
        int oplen = match_op(lx, opbuf);
        if (oplen == 0) {
            /* not an op; restore (don't lose ws state) */
            lx->pos = save;
            lx->had_newline = save_nl;
            break;
        }
        /* a newline before an op? treat as statement boundary -> stop */
        if (save_nl) {
            lx->pos = save;
            lx->had_newline = save_nl;
            break;
        }
        lx->pos += oplen;
        Val right = parse_operand(lx, vm, sp);
        Val head = sym(vm, opbuf);
        left = mk_list(sp, (Val[]){ head, left, right }, 3);
    }
    return left;
}

/* ====================================================================== */
/* Identifier + call                                                      */
/* ====================================================================== */

static Val parse_atom_or_call(Lex *lx, VM *vm, Proc *sp) {
    int n;
    char *id = read_ident(lx, &n);
    int ctor = is_constructor(id, n);

    /* keywords handled by callers; here only plain identifier/call */
    Val result;
    if (n == 3 && memcmp(id, "nil", 3) == 0)        result = val_nil();
    else if (n == 4 && memcmp(id, "true", 4) == 0)  result = val_true();
    else if (n == 5 && memcmp(id, "false", 5) == 0) result = val_false();
    else                                            result = val_symbol(intern_sym(vm, id, n));

    /* function call? (peek without consuming) */
    int save = lx->pos;
    int save_nl = lx->had_newline;
    skip_ws(lx);
    int is_call = (lx->pos < lx->len && lx->src[lx->pos] == '(');

    if (ctor) {
        if (is_call) {
            /* n-ary constructor: (cons (quote Name) (cons arg1 ... nil)) */
            lx->pos++; /* consume '(' */
            Val items[64];
            int cnt = 0;
            for (;;) {
                skip_ws(lx);
                if (lx->pos >= lx->len) break;
                if (lx->src[lx->pos] == ')') { lx->pos++; break; }
                if (cnt < 64) items[cnt++] = parse_expr(lx, vm, sp);
                skip_ws(lx);
                if (lx->pos < lx->len && lx->src[lx->pos] == ',') lx->pos++;
            }
                                    /* build args as nested cons: (cons arg1 (cons arg2 ... nil)) */
            Val tail = val_nil();
            for (int i = cnt - 1; i >= 0; i--)
                tail = mk_list(sp, (Val[]){ sym(vm,"cons"), items[i], tail }, 3);
            {
                int expected_arity = -1;
                find_ctor_info(id, n, &expected_arity);
                if (expected_arity >= 0 && cnt != expected_arity) {
                    fprintf(stderr, "error: constructor %.*s expects %d args, got %d\n",
                            n, id, expected_arity, cnt);
                }
            }
            Val quoted_head = mk_quoted(sp, vm, id, n);
            Val ret = mk_list(sp, (Val[]){ sym(vm,"cons"), quoted_head, tail }, 3);
            free(id);
            return ret;
        }
        /* nullary constructor: (quote Name) */
        lx->pos = save;
        lx->had_newline = save_nl;
        Val ret = mk_quoted(sp, vm, id, n);
        free(id);
        return ret;
    }

    free(id);
    if (is_call) {
        return parse_call_args(lx, vm, sp, result);
    }
    lx->pos = save;
    lx->had_newline = save_nl;
    return result;
}

/* parse "(arg, arg, ...)" — we are at '('; head is the function symbol */
static Val parse_call_args(Lex *lx, VM *vm, Proc *sp, Val head) {
    lx->pos++; /* skip '(' */
    Val items[64];
    int cnt = 0;
    for (;;) {
        skip_ws(lx);
        if (lx->pos >= lx->len) break;
        if (lx->src[lx->pos] == ')') { lx->pos++; break; }
        if (cnt < 64)
            items[cnt++] = parse_expr(lx, vm, sp);
        else
            parse_expr(lx, vm, sp); /* discard excess */
        skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == ',') lx->pos++;
    }
        /* build (head . args) */
    Val args = mk_list(sp, items, cnt);
    return val_pair(sp, head, args);
}

/* ====================================================================== */
/* Blocks { }                                                             */
/* ====================================================================== */

/* parse the inner content of a block (without braces), returning the AST
 * for that sequence of forms. Stops at '}' or EOF or a top-level boundary.
 *
 * let-nesting: a `let x = e` form at the front of remaining forms wraps the
 * rest as its body. */
static int is_keyword(Lex *lx, const char *kw) {
    int n = (int)strlen(kw);
    if (lx->pos + n > lx->len) return 0;
    if (memcmp(lx->src + lx->pos, kw, n) != 0) return 0;
    /* followed by non-ident char or end */
    if (lx->pos + n < lx->len && is_ident_char((unsigned char)lx->src[lx->pos+n]))
        return 0;
    return 1;
}

static Val parse_form(Lex *lx, VM *vm, Proc *sp); /* fwd */

/* Parse remaining block body starting now; returns AST of remaining forms. */
static Val parse_block_rest(Lex *lx, VM *vm, Proc *sp) {
    skip_ws(lx);
    if (lx->pos >= lx->len) return val_nil();
    if (lx->src[lx->pos] == '}') return val_nil();

    Val first = parse_form(lx, vm, sp);

    /* if first was a let, it wraps the rest */
    if (val_is_pair(first)) {
        Val h = val_get_car(first);
        /* crude check: head symbol "let" — compare by symbol index is hard
         * without strcmp; rely on a flag set in parse_form. Instead we
         * re-detect by structure: (let var expr) has 3 elements and head
         * equals interned "let". We'll detect via vm lookup. */
        /* We'll just check if there are remaining forms; if head is 'let'
         * we wrap. Detection done via intern below. */
        Val letsym = sym(vm, "let");
        if (h == letsym) {
            Val rest = parse_block_rest(lx, vm, sp);
            /* first = (let var expr); make (let var expr rest) */
            Val v = val_get_car(val_get_cdr(first));         /* var */
            Val e = val_get_car(val_get_cdr(val_get_cdr(first))); /* expr */
            return mk_list(sp, (Val[]){ letsym, v, e, rest }, 4);
        }
    }

    skip_ws(lx);
    if (lx->pos >= lx->len || lx->src[lx->pos] == '}') {
        return first;
    }
    /* 2+ forms: wrap in begin */
    Val items[64];
    int cnt = 0;
    items[cnt++] = first;
    while (cnt < 64) {
        skip_ws(lx);
        if (lx->pos >= lx->len || lx->src[lx->pos] == '}') break;
        items[cnt++] = parse_form(lx, vm, sp);
    }
    Val head = sym(vm, "begin");
    Val lst = mk_list(sp, items, cnt);
    return val_pair(sp, head, lst);
}

static Val parse_braced(Lex *lx, VM *vm, Proc *sp) {
    skip_ws(lx);
    if (lx->pos < lx->len && lx->src[lx->pos] == '{') lx->pos++;
    Val body = parse_block_rest(lx, vm, sp);
    skip_ws(lx);
    if (lx->pos < lx->len && lx->src[lx->pos] == '}') lx->pos++;
    return body;
}

/* ====================================================================== */
/* Forms (statements within blocks / exprs)                               */
/* ====================================================================== */

static Val parse_form(Lex *lx, VM *vm, Proc *sp) {
    skip_ws(lx);
    if (lx->pos >= lx->len) return val_nil();
    int start = lx->pos;

        /* let x = expr */
    if (is_keyword(lx, "let")) {
        lx->pos += 3;
        skip_ws(lx);
        int vn;
        char *var = read_ident(lx, &vn);
        skip_ws(lx);
        /* optional type annotation ": Type" */
        if (lx->pos < lx->len && lx->src[lx->pos] == ':') {
            lx->pos++;
            skip_ws(lx);
            while (lx->pos < lx->len && lx->src[lx->pos] != '=')
                lx->pos++;
        }
        if (lx->pos < lx->len && lx->src[lx->pos] == '=') lx->pos++;
        Val e = parse_expr(lx, vm, sp);
        Val v = val_symbol(intern_sym(vm, var, vn));
        free(var);
        return mk_list(sp, (Val[]){ sym(vm,"let"), v, e }, 3);
    }

    /* if cond { b1 } else { b2 } */
    if (is_keyword(lx, "if")) {
        lx->pos += 2;
        Val cond = parse_expr(lx, vm, sp);
        Val then_b = parse_braced(lx, vm, sp);
        Val else_b = val_nil();
        int save = lx->pos;
        int save_nl = lx->had_newline;
        skip_ws(lx);
        if (lx->pos < lx->len && is_keyword(lx, "else")) {
            lx->pos += 4;
            else_b = parse_braced(lx, vm, sp);
        } else {
            lx->pos = save;
            lx->had_newline = save_nl;
        }
        /* wrap multi-form branches in begin */
        if (val_is_pair(then_b) && val_get_car(then_b) == sym(vm,"begin")) {
            /* keep as-is */
        }
        return mk_list(sp, (Val[]){ sym(vm,"if"), cond, then_b, else_b }, 4);
    }

    /* match scrutinee { ... } */
    if (is_keyword(lx, "match")) {
        lx->pos += 5;
        Val scrut = parse_expr(lx, vm, sp);
        /* expect { */
        skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == '{') lx->pos++;
        Val arms = val_nil();
        Val *tail = &arms;
        for (;;) {
            skip_ws(lx);
            if (lx->pos >= lx->len || lx->src[lx->pos] == '}') break;
                        Val pat = parse_pattern(lx, vm, sp);
            skip_ws(lx);
            /* Optional if guard: `pat if guard_expr -> body` */
            Val guard = val_nil();
            if (lx->pos < lx->len && is_keyword(lx, "if")) {
                lx->pos += 2;  /* skip "if" */
                skip_ws(lx);
                guard = parse_expr(lx, vm, sp);
                skip_ws(lx);
            }
            /* '->' */
            if (lx->pos+1 < lx->len && lx->src[lx->pos]=='-' && lx->src[lx->pos+1]=='>') {
                lx->pos += 2;
            }
            /* body: braced block or single expr */
            Val body;
            skip_ws(lx);
            if (lx->pos < lx->len && lx->src[lx->pos] == '{') {
                body = parse_braced(lx, vm, sp);
            } else {
                body = parse_expr(lx, vm, sp);
            }
            Val arm;
            if (!val_is_nil(guard)) {
                arm = mk_list(sp, (Val[]){ pat, guard, body }, 3);
            } else {
                arm = mk_list(sp, (Val[]){ pat, body }, 2);
            }
            Val cell = val_pair(sp, arm, val_nil());
            *tail = cell;
            tail = &((HeapPair *)val_as_pair(cell))->cdr;
        }
                        if (lx->pos < lx->len && lx->src[lx->pos] == '}') lx->pos++;
        check_exhaustiveness(vm, arms, "match");
        return val_pair(sp, sym(vm,"match"), val_pair(sp, scrut, arms));
    }

    /* receive { ... } */
    if (is_keyword(lx, "receive")) {
        lx->pos += 7;
        skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == '{') lx->pos++;
        Val arms = val_nil();
        Val *tail = &arms;
        for (;;) {
            skip_ws(lx);
            if (lx->pos >= lx->len || lx->src[lx->pos] == '}') break;
                        Val pat = parse_pattern(lx, vm, sp);
            skip_ws(lx);
            /* Optional if guard: `pat if guard_expr -> body` */
            Val guard = val_nil();
            if (lx->pos < lx->len && is_keyword(lx, "if")) {
                lx->pos += 2;  /* skip "if" */
                skip_ws(lx);
                guard = parse_expr(lx, vm, sp);
                skip_ws(lx);
            }
            if (lx->pos+1 < lx->len && lx->src[lx->pos]=='-' && lx->src[lx->pos+1]=='>') {
                lx->pos += 2;
            }
            Val body;
            skip_ws(lx);
            if (lx->pos < lx->len && lx->src[lx->pos] == '{') {
                body = parse_braced(lx, vm, sp);
            } else {
                body = parse_expr(lx, vm, sp);
            }
            Val arm;
            if (!val_is_nil(guard)) {
                arm = mk_list(sp, (Val[]){ pat, guard, body }, 3);
            } else {
                arm = mk_list(sp, (Val[]){ pat, body }, 2);
            }
            Val cell = val_pair(sp, arm, val_nil());
            *tail = cell;
            tail = &((HeapPair *)val_as_pair(cell))->cdr;
        }
                if (lx->pos < lx->len && lx->src[lx->pos] == '}') lx->pos++;
        check_exhaustiveness(vm, arms, "receive");
                return val_pair(sp, sym(vm,"receive"), arms);
    }

    /* spawn(fn { body }) handled via spawn keyword? Spec: spawn(fn { body }).
     * We'll detect identifier "spawn" followed by "(fn". */
    /* Otherwise: a normal expression. */
    (void)start;
    return parse_expr(lx, vm, sp);
}

/* ====================================================================== */
/* Top-level forms                                                        */
/* ====================================================================== */

/* `fn name(params) { body }` -> (define (name params...) body...) */
static Val parse_toplevel_fn(Lex *lx, VM *vm, Proc *sp, int is_pub) {
    /* 'fn' already consumed */
        skip_ws(lx);
    int nn;
    char *name = read_ident(lx, &nn);
    Val name_sym = val_symbol(intern_sym(vm, name, nn));

            /* Set up annotation entry for this function */
    Anno anno_s = {0};
    Anno *anno = &anno_s;
    free(name);

    skip_ws(lx);
    /* params in ( ... ) */
    Val params = val_nil();
    Val *ptail = &params;
    int anno_param_idx = 0;
    if (lx->pos < lx->len && lx->src[lx->pos] == '(') {
        lx->pos++;
        for (;;) {
            skip_ws(lx);
            if (lx->pos >= lx->len || lx->src[lx->pos] == ')') {
                if (lx->pos < lx->len) lx->pos++;
                break;
            }
                        int pn;
            char *p = read_ident(lx, &pn);
            Val ps = val_symbol(intern_sym(vm, p, pn));
            free(p);
            /* optional type annotation ": Type" */
            skip_ws(lx);
            if (lx->pos < lx->len && lx->src[lx->pos] == ':') {
                lx->pos++;
                skip_ws(lx);
                /* capture annotation text */
                int astart = lx->pos;
                while (lx->pos < lx->len && lx->src[lx->pos] != ',' && lx->src[lx->pos] != ')')
                    lx->pos++;
                                if (anno_param_idx < 16) {
                    int alen = lx->pos - astart;
                    if (alen >= 64) alen = 63;
                    memcpy(anno->param_types[anno_param_idx], lx->src + astart, alen);
                    anno->param_types[anno_param_idx][alen] = '\0';
                }
            }
            anno_param_idx++;
            Val cell = val_pair(sp, ps, val_nil());
            *ptail = cell;
            ptail = &((HeapPair *)val_as_pair(cell))->cdr;
            skip_ws(lx);
            if (lx->pos < lx->len && lx->src[lx->pos] == ',') lx->pos++;
        }
    }
        anno->nparams = anno_param_idx;

    /* optional return type annotation: "-> Type" */
    skip_ws(lx);
    if (lx->pos + 1 < lx->len && lx->src[lx->pos] == '-' && lx->src[lx->pos+1] == '>') {
        lx->pos += 2;
        skip_ws(lx);
        /* capture return type annotation */
        int rstart = lx->pos;
        while (lx->pos < lx->len && lx->src[lx->pos] != '{')
            lx->pos++;
                        int rlen = lx->pos - rstart;
        if (rlen >= 64) rlen = 63;
        memcpy(anno->ret_type, lx->src + rstart, rlen);
        anno->ret_type[rlen] = '\0';
        anno->has_ret_annotation = 1;
    }
        /* signature: (name . params) */
    Val sig = val_pair(sp, name_sym, params);

    /* body block { ... } */
    skip_ws(lx);
    Val body = val_nil();
    if (lx->pos < lx->len && lx->src[lx->pos] == '{') {
        lx->pos++;
        body = parse_block_rest(lx, vm, sp);
        skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == '}') lx->pos++;
    }

            Val define_form = mk_list(sp, (Val[]){ sym(vm, is_pub ? "define_pub" : "define"), sig, body }, 3);

        /* Build type-sig form if any annotations exist */
    int has_annot = anno->has_ret_annotation;
    if (!has_annot) {
        for (int i = 0; i < anno->nparams; i++) {
            if (anno->param_types[i][0] != '\0') { has_annot = 1; break; }
        }
    }
    if (!has_annot) return define_form;

    /* Build param types list: symbol for annotated, nil for untyped */
    Val ptypes = val_nil();
    Val *pttail = &ptypes;
    for (int i = 0; i < anno->nparams; i++) {
        Val tv;
        if (anno->param_types[i][0] != '\0') {
            char *s = anno->param_types[i];
            int slen = (int)strlen(s);
            while (slen > 0 && (s[slen-1] == ' ' || s[slen-1] == '\t' ||
                                s[slen-1] == '\n' || s[slen-1] == '\r'))
                slen--;
            tv = val_symbol(intern_sym(vm, s, slen));
        } else {
            tv = val_nil();
        }
        Val cell = val_pair(sp, tv, val_nil());
        *pttail = cell;
        pttail = &((HeapPair *)val_as_pair(cell))->cdr;
    }

    /* Return type: symbol or nil */
    Val ret_type_val;
    if (anno->has_ret_annotation) {
        char *s = anno->ret_type;
        int slen = (int)strlen(s);
        while (slen > 0 && (s[slen-1] == ' ' || s[slen-1] == '\t' ||
                            s[slen-1] == '\n' || s[slen-1] == '\r'))
            slen--;
        ret_type_val = val_symbol(intern_sym(vm, s, slen));
    } else {
        ret_type_val = val_nil();
    }

    Val typesig_form = mk_list(sp, (Val[]){ sym(vm, "type-sig"), name_sym, ptypes, ret_type_val }, 4);
    return mk_list(sp, (Val[]){ sym(vm, "begin"), typesig_form, define_form }, 3);
}

/* `const NAME = expr` -> (const NAME expr) */
static Val parse_toplevel_const(Lex *lx, VM *vm, Proc *sp) {
    /* 'const' already consumed */
    skip_ws(lx);
    int nn;
    char *name = read_ident(lx, &nn);
    Val name_sym = val_symbol(intern_sym(vm, name, nn));
    free(name);
    skip_ws(lx);
    if (lx->pos < lx->len && lx->src[lx->pos] == '=') lx->pos++;
    skip_ws(lx);
    Val val_expr = parse_expr(lx, vm, sp);
    return mk_list(sp, (Val[]){ sym(vm,"const"), name_sym, val_expr }, 3);
}

/* `import net` -> (import "net") */
static Val parse_toplevel_import(Lex *lx, VM *vm, Proc *sp) {
    /* 'import' already consumed */
    skip_ws(lx);
    int mn;
    char *mod = read_ident(lx, &mn);
    Val s = val_string(sp, mod, mn);
    free(mod);
    return mk_list(sp, (Val[]){ sym(vm,"import"), s }, 2);
}

/* `type Name { V1(T,...); V2 }` -> (type)
 * Registers constructors for later sugar expansion. Returns a no-op form. */
static Val parse_toplevel_type(Lex *lx, VM *vm, Proc *sp) {
    /* 'type' already consumed */
    (void)sp;
    skip_ws(lx);
    int tn;
    char *tname = read_ident(lx, &tn);

    /* record type into registry */
    int ti = -1;
    if (n_types < 64 && tn > 0 && tn < 64) {
        ti = n_types;
        memcpy(types[ti].name, tname, tn);
        types[ti].name[tn] = '\0';
        types[ti].n_variants = 0;
        n_types++;
    }
    free(tname);

    skip_ws(lx);
    if (lx->pos < lx->len && lx->src[lx->pos] == '{') lx->pos++;
    for (;;) {
        skip_ws(lx);
        if (lx->pos >= lx->len) break;
        if (lx->src[lx->pos] == '}') { lx->pos++; break; }
        int vn;
        char *vname = read_ident(lx, &vn);

        /* count arity from args "(T1, T2)" if present; empty () -> 0 args */
        int arity = 0;
        skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == '(') {
            int depth = 0, commas = 0, content = 0;
            while (lx->pos < lx->len) {
                char d = lx->src[lx->pos];
                if (d == '(') { depth++; }
                else if (d == ')') { depth--; lx->pos++; if (depth == 0) break; continue; }
                else if (depth == 1 && d == ',') commas++;
                else if (depth >= 1 && !isspace((unsigned char)d)) content = 1;
                lx->pos++;
            }
            arity = content ? (commas + 1) : 0;
        }

        if (ti >= 0 && types[ti].n_variants < 32 && vn > 0 && vn < 64) {
            int nv = types[ti].n_variants;
            memcpy(types[ti].variants[nv], vname, vn);
            types[ti].variants[nv][vn] = '\0';
            types[ti].variant_arities[nv] = arity;
            types[ti].n_variants++;
        }
        free(vname);
        skip_ws(lx);
        if (lx->pos < lx->len) {
            char c = lx->src[lx->pos];
            if (c == ';' || c == ',') lx->pos++;
        }
    }
    return val_pair(sp, sym(vm, "type"), val_nil());
}

/* spawn: spec example `spawn(fn { body })` -> (spawn (lambda () body...)) */
static Val parse_spawn(Lex *lx, VM *vm, Proc *sp) {
    /* 'spawn' consumed; expect '(' 'fn' ... ')' or a lambda expr. */
    skip_ws(lx);
    if (lx->pos < lx->len && lx->src[lx->pos] == '(') {
        lx->pos++;
    }
    skip_ws(lx);
    Val lam;
    if (is_keyword(lx, "fn")) {
        lx->pos += 2;
        /* optional params? spec uses fn { body } */
        skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == '(') {
            /* skip params */
            lx->pos++;
            while (lx->pos < lx->len && lx->src[lx->pos] != ')') lx->pos++;
            if (lx->pos < lx->len) lx->pos++;
        }
        Val body = parse_braced(lx, vm, sp);
        lam = mk_list(sp, (Val[]){ sym(vm,"lambda"), val_nil(), body }, 3);
    } else {
        lam = parse_expr(lx, vm, sp);
    }
    skip_ws(lx);
    if (lx->pos < lx->len && lx->src[lx->pos] == ')') lx->pos++;
    return mk_list(sp, (Val[]){ sym(vm,"spawn"), lam }, 2);
}

/* `send(pid, msg)` -> (send pid msg) */
static Val parse_send(Lex *lx, VM *vm, Proc *sp) {
    /* 'send' consumed */
    skip_ws(lx);
    if (lx->pos < lx->len && lx->src[lx->pos] == '(') lx->pos++;
    Val pid = parse_expr(lx, vm, sp);
    skip_ws(lx);
    if (lx->pos < lx->len && lx->src[lx->pos] == ',') lx->pos++;
    Val msg = parse_expr(lx, vm, sp);
    skip_ws(lx);
    if (lx->pos < lx->len && lx->src[lx->pos] == ')') lx->pos++;
    return mk_list(sp, (Val[]){ sym(vm,"send"), pid, msg }, 3);
}

/* ====================================================================== */
/* Public entry                                                           */
/* ====================================================================== */

Val reader_ta_read(VM *vm, const char *src, int *pos) {
    Proc *sp = get_scratch();
    Lex lx;
    lx.src = src;
    lx.len = (int)strlen(src);
    lx.pos = *pos;
    lx.had_newline = 0;

    skip_ws(&lx);
    if (lx.pos >= lx.len) { *pos = lx.pos; return val_nil(); }

            if (is_keyword(&lx, "pub")) {
        lx.pos += 3;  /* consume "pub" */
        skip_ws(&lx);
        if (is_keyword(&lx, "fn")) {
            lx.pos += 2;
            Val v = parse_toplevel_fn(&lx, vm, sp, 1);
            *pos = lx.pos;
            return v;
        }
        if (is_keyword(&lx, "type")) {
            /* pub type = same as type (types are always exported) */
            lx.pos += 4;
            Val v = parse_toplevel_type(&lx, vm, sp);
            *pos = lx.pos;
            return v;
        }
        fprintf(stderr, "error: 'pub' must be followed by 'fn' or 'type'\n");
        *pos = lx.pos;
        return val_nil();
    }
    if (is_keyword(&lx, "fn")) {
        lx.pos += 2;
        Val v = parse_toplevel_fn(&lx, vm, sp, 0);
        *pos = lx.pos;
        return v;
    }
        if (is_keyword(&lx, "import")) {
        lx.pos += 6;
        Val v = parse_toplevel_import(&lx, vm, sp);
        *pos = lx.pos;
        return v;
    }
    if (is_keyword(&lx, "type")) {
        lx.pos += 4;
        Val v = parse_toplevel_type(&lx, vm, sp);
        *pos = lx.pos;
        return v;
    }
    /* spawn / send may appear at top level too */
    if (is_keyword(&lx, "spawn")) {
        lx.pos += 5;
        Val v = parse_spawn(&lx, vm, sp);
        *pos = lx.pos;
        return v;
    }
        if (is_keyword(&lx, "send")) {
        lx.pos += 4;
        Val v = parse_send(&lx, vm, sp);
        *pos = lx.pos;
        return v;
    }
    if (is_keyword(&lx, "const")) {
        lx.pos += 5;  /* consume "const" */
        skip_ws(&lx);
        Val v = parse_toplevel_const(&lx, vm, sp);
        *pos = lx.pos;
        return v;
    }

        /* fall back: treat as a form */
    Val v = parse_form(&lx, vm, sp);
    *pos = lx.pos;
    return v;
}