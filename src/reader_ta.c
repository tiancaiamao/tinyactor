/* reader_ta.c — parses ML/Rust-style .ta syntax into pair-tree AST.
 *
 * Grammar -> AST mapping is documented in the task spec. Returns one
 * top-level form per call to reader_ta_read().
 */

#include "ta.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

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
static Proc *get_scratch(void) {
    static Proc *sp = NULL;
    if (!sp) {
        sp = calloc(1, sizeof(Proc));
        sp->mem_size = 32768;
        sp->mem = malloc(sp->mem_size);
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
           c=='=' || c=='!' || c=='?' || c=='*';
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
static Val parse_block(Lex *lx, VM *vm, Proc *sp);
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
    int start = lx->pos;
    while (lx->pos < lx->len && lx->src[lx->pos] != '"')
        lx->pos++;
    int slen = lx->pos - start;
    if (lx->pos < lx->len) lx->pos++; /* skip closing quote */
    return val_string(sp, lx->src + start, slen);
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

    /* identifier (includes _ , nil, true, false) */
    int n;
    char *id = read_ident(lx, &n);
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

    /* keywords handled by callers; here only plain identifier/call */
    Val result;
    if (n == 3 && memcmp(id, "nil", 3) == 0)        result = val_nil();
    else if (n == 4 && memcmp(id, "true", 4) == 0)  result = val_true();
    else if (n == 5 && memcmp(id, "false", 5) == 0) result = val_false();
    else                                            result = val_symbol(intern_sym(vm, id, n));
    free(id);

    /* function call? */
    int save = lx->pos;
    int save_nl = lx->had_newline;
    skip_ws(lx);
    if (lx->pos < lx->len && lx->src[lx->pos] == '(') {
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

/* parse_block: public-ish entry for a full { ... } block */
static Val parse_block(Lex *lx, VM *vm, Proc *sp) {
    return parse_braced(lx, vm, sp);
}

/* ====================================================================== */
/* Forms (statements within blocks / exprs)                               */
/* ====================================================================== */

static int read_keyword(Lex *lx, const char *kw) {
    skip_ws(lx);
    if (!is_keyword(lx, kw)) return 0;
    lx->pos += (int)strlen(kw);
    return 1;
}

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
            Val arm = mk_list(sp, (Val[]){ pat, body }, 2);
            Val cell = val_pair(sp, arm, val_nil());
            *tail = cell;
            tail = &((HeapPair *)val_as_pair(cell))->cdr;
        }
                if (lx->pos < lx->len && lx->src[lx->pos] == '}') lx->pos++;
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
            Val arm = mk_list(sp, (Val[]){ pat, body }, 2);
            Val cell = val_pair(sp, arm, val_nil());
            *tail = cell;
            tail = &((HeapPair *)val_as_pair(cell))->cdr;
        }
        if (lx->pos < lx->len && lx->src[lx->pos] == '}') lx->pos++;
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
static Val parse_toplevel_fn(Lex *lx, VM *vm, Proc *sp) {
    /* 'fn' already consumed */
        skip_ws(lx);
    int nn;
    char *name = read_ident(lx, &nn);
    Val name_sym = val_symbol(intern_sym(vm, name, nn));
    free(name);

    skip_ws(lx);
    /* params in ( ... ) */
    Val params = val_nil();
    Val *ptail = &params;
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
            Val cell = val_pair(sp, ps, val_nil());
            *ptail = cell;
            ptail = &((HeapPair *)val_as_pair(cell))->cdr;
            skip_ws(lx);
            if (lx->pos < lx->len && lx->src[lx->pos] == ',') lx->pos++;
        }
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

    return mk_list(sp, (Val[]){ sym(vm,"define"), sig, body }, 3);
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

    if (is_keyword(&lx, "fn")) {
        lx.pos += 2;
        Val v = parse_toplevel_fn(&lx, vm, sp);
        *pos = lx.pos;
        return v;
    }
    if (is_keyword(&lx, "import")) {
        lx.pos += 6;
        Val v = parse_toplevel_import(&lx, vm, sp);
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

        /* fall back: treat as a form */
    Val v = parse_form(&lx, vm, sp);
    *pos = lx.pos;
    return v;
}