/*
 * sexp.c — S-expression reader module for TinyActor VM
 *
 *   sexp.parse(text)  -> nil-terminated list of ALL top-level forms,
 *                        or -1 on a hard error (malformed input; details
 *                        via sexp.err(), which then returns non-empty).
 *   sexp.err()        -> string describing the last parse error, "" if the
 *                        previous sexp.parse succeeded.
 *
 * The tree is the lisp-kernel 对接 AST: pairs, symbols (interned), ints,
 * strings, and the nil/true/false literals.  Lists are nil-terminated —
 * `(a b c)` reads as (a . (b . (c . nil))), the standard sexp shape.
 *
 * Grammar (v1):
 *   form    := atom | list | quote
 *   list    := '(' form* ')'          — empty list is nil
 *   quote   := ''' form              — reads as the list (quote form)
 *   atom    := int | symbol | string
 *   comment := ';' to end of line
 * Ints fit int48 (the NaN-boxing payload width); an all-digit token that
 * overflows int48 is a hard error, not a symbol.  `nil` reads as the empty
 * list (TAG_NIL); `true`/`false` read as their tags.
 *
 * Deliberately absent in v1 (add when the lisp kernel needs them):
 * floats, `[` `]` as parens, quasiquote/unquote, #vector syntax.
 *
 * Memory model (docs/c-module.md §3): the whole parse runs inside one
 * cfunc call, i.e. under the per-proc gc_gate — C-local Vals stay valid
 * across the allocations that build the tree, no rooting needed.
 * Error text lives in a __thread buffer (a worker runs one proc at a
 * time and sexp.parse never yields, so sexp.err() on the same thread
 * always reads the error it just produced).
 */

#include "ta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Nesting cap: a textbook .lisp file is <100 deep; the cap only turns
 * hostile/degenerate input into a clean error instead of C stack death. */
#define SEXP_MAX_DEPTH 10000

/* int48 payload bounds (ta.h NaN-boxing) */
#define SEXP_INT48_MAX ((int64_t)(((uint64_t)1 << 47) - 1))
#define SEXP_INT48_MIN (-((int64_t)1 << 47))

/* Token length caps. A 31-char token covers any int48 spelling (with
 * leading zeros it may still be small, but 31 digits of padding is not
 * a real program); 255 caps symbol names for the intern scratch buffer. */
#define SEXP_INT_TOK 31
#define SEXP_SYM_TOK 255
#define SEXP_STR_TOK 512

typedef struct {
    VM *vm;
    const char *src;
    int len;
    int pos;
    int line;
    int col;
    int failed; /* set by parse_fail; checked by callers (the returned Val
                 * cannot double as an error signal: (-1) is a valid form) */
} Parser;

/* Per-worker error detail; "" (empty string) when the last parse was ok. */
static __thread char sexp_errbuf[128];

static void sexp_err_reset(void) { sexp_errbuf[0] = '\0'; }

static Val parse_fail(Parser *ps, const char *msg) {
    ps->failed = 1;
    snprintf(sexp_errbuf, sizeof(sexp_errbuf), "line %d, col %d: %s", ps->line, ps->col, msg);
    return val_int(-1);
}

static int at_eof(Parser *ps) { return ps->pos >= ps->len; }

static int peek(Parser *ps) { return ps->src[ps->pos]; }

static void advance(Parser *ps) {
    if (ps->src[ps->pos] == '\n') {
        ps->line++;
        ps->col = 1;
    } else {
        ps->col++;
    }
    ps->pos++;
}

static int is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static int is_delim(int c) {
    return is_space(c) || c == '(' || c == ')' || c == '\'' || c == '"' || c == ';';
}

/* Skip whitespace and ';' comments to the next live char (or EOF). */
static void skip_ws(Parser *ps) {
    while (!at_eof(ps)) {
        int c = peek(ps);
        if (is_space(c)) {
            advance(ps);
        } else if (c == ';') {
            while (!at_eof(ps) && peek(ps) != '\n')
                advance(ps);
        } else {
            break;
        }
    }
}

static Val parse_form(Parser *ps, int depth);

/* Parse a string literal starting at '"'. Raw newlines are allowed inside
 * (multiline strings, matching the TA tokenizer). Grows a scratch buffer;
 * val_string copies, then we free. */
static Val parse_string(Parser *ps) {
    advance(ps); /* opening quote */
    int cap = SEXP_STR_TOK;
    char *out = malloc((size_t)cap);
    if (!out)
        return parse_fail(ps, "out of memory");
    int n = 0;
    for (;;) {
        if (at_eof(ps)) {
            free(out);
            return parse_fail(ps, "unterminated string");
        }
        int c = peek(ps);
        if (c == '"') {
            advance(ps);
            break;
        }
        if (c == '\\') {
            advance(ps);
            if (at_eof(ps)) {
                free(out);
                return parse_fail(ps, "unterminated string");
            }
            int e = peek(ps);
            if (e == 'n')
                c = '\n';
            else if (e == 't')
                c = '\t';
            else if (e == 'r')
                c = '\r';
            else if (e == '"' || e == '\\')
                c = e;
            else {
                free(out);
                return parse_fail(ps, "unknown escape in string");
            }
            advance(ps);
        } else {
            advance(ps);
        }
        if (n + 1 > cap) {
            cap *= 2;
            char *no = realloc(out, (size_t)cap);
            if (!no) {
                free(out);
                return parse_fail(ps, "out of memory");
            }
            out = no;
        }
        out[n++] = (char)c;
    }
    Proc *p = tls_current_proc;
    Val v = val_string(p, out, n);
    free(out);
    return v;
}

/* Parse an atom: int, nil/true/false literal, or symbol. */
static Val parse_atom(Parser *ps) {
    int start = ps->pos;
    while (!at_eof(ps) && !is_delim(peek(ps)))
        advance(ps);
    int len = ps->pos - start;
    const char *tok = ps->src + start;

    /* Only ')' can reach here with an empty token: EOF and the other
     * delimiters dispatch before this call. */
    if (len == 0)
        return parse_fail(ps, "unexpected ')'");

    /* Int: optional sign + at least one digit, all digits. */
    int i = 0;
    if (tok[i] == '-' || tok[i] == '+')
        i++;
    int all_digits = (i < len);
    for (; i < len; i++)
        if (tok[i] < '0' || tok[i] > '9')
            all_digits = 0;
    if (all_digits) {
        if (len > SEXP_INT_TOK)
            return parse_fail(ps, "integer literal too long");
        char buf[SEXP_INT_TOK + 1];
        memcpy(buf, tok, (size_t)len);
        buf[len] = '\0';
        char *end = NULL;
        long long v = strtoll(buf, &end, 10);
        if (*end != '\0' || v < SEXP_INT48_MIN || v > SEXP_INT48_MAX)
            return parse_fail(ps, "integer literal out of int48 range");
        return val_int((int64_t)v);
    }

    if (len == 3 && memcmp(tok, "nil", 3) == 0)
        return val_nil();
    if (len == 4 && memcmp(tok, "true", 4) == 0)
        return val_true();
    if (len == 5 && memcmp(tok, "false", 5) == 0)
        return val_false();

    if (len > SEXP_SYM_TOK)
        return parse_fail(ps, "symbol too long");
    char buf[SEXP_SYM_TOK + 1];
    memcpy(buf, tok, (size_t)len);
    buf[len] = '\0';
    int idx = vm_intern_symbol(ps->vm, buf);
    if (idx < 0)
        return parse_fail(ps, "out of memory");
    return val_symbol((uint32_t)idx);
}

/* Parse a list starting just after '('. Consumes the closing ')'. */
static Val parse_list_tail(Parser *ps, int depth) {
    Val acc = val_nil();
    for (;;) {
        skip_ws(ps);
        if (at_eof(ps))
            return parse_fail(ps, "unbalanced '(' — missing ')'");
        if (peek(ps) == ')') {
            advance(ps);
            /* reverse the accumulated chain */
            Val list = val_nil();
            while (val_is_pair(acc)) {
                Proc *p = tls_current_proc;
                Val next = val_get_cdr(acc);
                list = val_pair(p, val_get_car(acc), list);
                acc = next;
            }
            return list;
        }
        Val form = parse_form(ps, depth);
        if (ps->failed)
            return form;
        acc = val_pair(tls_current_proc, form, acc);
    }
}

static Val parse_form(Parser *ps, int depth) {
    if (depth > SEXP_MAX_DEPTH)
        return parse_fail(ps, "nesting too deep");
    skip_ws(ps);
    if (at_eof(ps))
        return parse_fail(ps, "unexpected end of input");
    int c = peek(ps);
    if (c == '(') {
        advance(ps);
        return parse_list_tail(ps, depth + 1);
    }
    if (c == '\'') {
        advance(ps);
        Val form = parse_form(ps, depth + 1);
        if (ps->failed)
            return form;
        Proc *p = tls_current_proc;
        Val quote = val_symbol((uint32_t)vm_intern_symbol(ps->vm, "quote"));
        return val_pair(p, quote, val_pair(p, form, val_nil()));
    }
    if (c == '"')
        return parse_string(ps);
    return parse_atom(ps);
}

static Val sexp_parse(VM *vm, Val *args, int nargs) {
    (void)nargs;
    sexp_err_reset();
    if (!val_is_string(args[0])) {
        snprintf(sexp_errbuf, sizeof(sexp_errbuf), "parse: argument is not a string");
        return val_int(-1);
    }
    HeapString *hs = val_get_string(args[0]);
    Parser ps = {vm, hs->data, hs->len, 0, 1, 1, 0};

    Val acc = val_nil();
    for (;;) {
        skip_ws(&ps);
        if (at_eof(&ps))
            break;
        Val form = parse_form(&ps, 0);
        if (ps.failed)
            return val_int(-1);
        acc = val_pair(tls_current_proc, form, acc);
    }
    /* reverse the accumulated chain */
    Val list = val_nil();
    while (val_is_pair(acc)) {
        Proc *p = tls_current_proc;
        Val next = val_get_cdr(acc);
        list = val_pair(p, val_get_car(acc), list);
        acc = next;
    }
    return list;
}

/* Detail of the last sexp.parse error on this worker ("" if none). */
static Val sexp_err(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    return val_string(tls_current_proc, sexp_errbuf, (int)strlen(sexp_errbuf));
}

static TaFunc sexp_funcs[] = {{"parse", sexp_parse, 1}, {"err", sexp_err, 0}, {NULL, NULL, 0}};

void vm_register_sexp_module(VM *vm) { vm_register_module(vm, "sexp", sexp_funcs, 2); }