/*
 * reader.c — S-expression reader for TinyActor
 *
 * Parses source text into Val AST (pair chains, integers, symbols, etc.).
 * Uses a scratch Proc for heap-allocated values (pairs, strings).
 * The scratch heap is reset on each reader_read() call, so the caller
 * (typically the compiler) must consume the AST before the next read.
 */

#include "ta.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ============================================================
 * Scratch Proc — heap arena for parsing
 * ============================================================ */

static Proc *get_scratch(void) {
    static Proc *sp = NULL;
    if (!sp) {
        sp = calloc(1, sizeof(Proc));
                sp->mem_size = 1 << 23;        /* 8 MiB: covers large self-hosted modules */
        sp->mem = malloc(sp->mem_size);
        sp->gc_to = malloc(sp->mem_size);
        sp->sp = 0;  /* no stack — full mem available for heap */
    }
        /* NOTE: do NOT reset heap_ptr here — callers (e.g. vm_load) may
     * hold references to previously returned ASTs.  The heap is reused
     * across reader_read() calls and invalidated only when the caller
     * is done with all ASTs (e.g. after compilation). */
    return sp;
}

/* ============================================================
 * Symbol interning
 * ============================================================ */

static uint32_t intern_sym(VM *vm, const char *s, int len) {
    /* Linear scan — symbol tables are small during loading */
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

/* ============================================================
 * Lexer helpers
 * ============================================================ */

static void skip_ws(const char *src, int *pos) {
    while (src[*pos]) {
        if (src[*pos] == ';') {
            /* comment: skip to end of line */
            while (src[*pos] && src[*pos] != '\n') (*pos)++;
        } else if (isspace((unsigned char)src[*pos])) {
            (*pos)++;
        } else {
            break;
        }
    }
}

static int is_ident_char(char c) {
    if (isalnum((unsigned char)c)) return 1;
    switch (c) {
    case '+': case '-': case '*': case '/': case '%':
    case '=': case '<': case '>': case '?': case '!': case '_':
    case '.':
        return 1;
    }
    return 0;
}

/* ============================================================
 * Recursive descent parser
 * ============================================================ */

static Val read_expr(VM *vm, Proc *sp, const char *src, int *pos);

/* Read a string literal "..." with basic escape handling. */
static Val read_string_lit(Proc *sp, const char *src, int *pos) {
    (*pos)++; /* skip opening " */
    char buf[4096];
    int len = 0;
    while (src[*pos] && src[*pos] != '"') {
        if (src[*pos] == '\\' && src[*pos + 1]) {
            (*pos)++;
            switch (src[*pos]) {
            case 'n':  buf[len++] = '\n'; break;
            case 't':  buf[len++] = '\t'; break;
            case '\\': buf[len++] = '\\'; break;
            case '"':  buf[len++] = '"';  break;
            default:   buf[len++] = src[*pos]; break;
            }
        } else {
            buf[len++] = src[*pos];
        }
        (*pos)++;
    }
    if (src[*pos] == '"') (*pos)++;
    return val_string(sp, buf, len);
}

/* Read list elements after opening '('.
 * Handles: (a b c) → pair chain, (a . b) → dotted pair. */
static Val read_list(VM *vm, Proc *sp, const char *src, int *pos) {
    skip_ws(src, pos);
    if (src[*pos] == ')') {
        (*pos)++;
        return val_nil();
    }

    Val first = read_expr(vm, sp, src, pos);
    skip_ws(src, pos);

    /* Dotted pair: (a . b) — dot must be followed by whitespace or ')' */
    if (src[*pos] == '.' &&
        (isspace((unsigned char)src[*pos + 1]) || src[*pos + 1] == ')')) {
        (*pos)++;
        skip_ws(src, pos);
        Val cdr = read_expr(vm, sp, src, pos);
        skip_ws(src, pos);
        if (src[*pos] == ')') (*pos)++;
        return val_pair(sp, first, cdr);
    }

    /* Normal list: recurse for remaining elements */
    Val rest = read_list(vm, sp, src, pos);
    return val_pair(sp, first, rest);
}

/* Read one S-expression starting at src[*pos], advance *pos. */
static Val read_expr(VM *vm, Proc *sp, const char *src, int *pos) {
    skip_ws(src, pos);
    char c = src[*pos];

    if (c == '\0') return val_nil(); /* EOF */

    /* List: ( ... ) */
    if (c == '(') {
        (*pos)++;
        return read_list(vm, sp, src, pos);
    }

    /* Quote shorthand: 'x → (quote x) */
    if (c == '\'') {
        (*pos)++;
        Val v = read_expr(vm, sp, src, pos);
        if (val_is_nil(v)) return v; /* 'nil → nil directly */
        Val qsym = val_symbol(intern_sym(vm, "quote", 5));
        return val_pair(sp, qsym, val_pair(sp, v, val_nil()));
    }

    /* String literal */
    if (c == '"') return read_string_lit(sp, src, pos);

    /* Boolean literals */
    if (c == '#') {
        (*pos)++;
        if (src[*pos] == 't') { (*pos)++; return val_true(); }
        if (src[*pos] == 'f') { (*pos)++; return val_false(); }
        return val_nil(); /* unknown # sequence */
    }

    /* Number: digit, or sign followed by digit */
    if (isdigit((unsigned char)c) ||
        ((c == '-' || c == '+') && isdigit((unsigned char)src[*pos + 1]))) {
        int neg = 0;
        if (c == '-') { neg = 1; (*pos)++; }
        else if (c == '+') { (*pos)++; }
        int64_t n = 0;
        while (isdigit((unsigned char)src[*pos])) {
            n = n * 10 + (src[*pos] - '0');
            (*pos)++;
        }
        return val_int(neg ? -n : n);
    }

    /* Identifier (symbol, operator, etc.) */
    int start = *pos;
    while (is_ident_char(src[*pos])) (*pos)++;
    int len = *pos - start;
    if (len == 0) { (*pos)++; return val_nil(); } /* skip unknown char */

    /* nil keyword → nil value */
    if (len == 3 && memcmp(src + start, "nil", 3) == 0)
        return val_nil();

    return val_symbol(intern_sym(vm, src + start, len));
}

/* ============================================================
 * Public interface
 * ============================================================ */

Val reader_read(VM *vm, const char *src, int *pos) {
    Proc *sp = get_scratch();
    return read_expr(vm, sp, src, pos);
}