/*
 * str.c — String utility module for TinyActor VM
 *
 *   str.char_at(s, i)    -> Int   (ASCII code, or -1)
 *   str.length(s)        -> Int
 *   str.substr(s, st, n) -> String
 *   str.concat(a, b)     -> String
 *   str.to_int(s)        -> Int   (0 on parse failure)
 *   str.from_int(n)      -> String
 *   str.eq(a, b)         -> Int
 *   str.index_of(s, sub) -> Int   (first index, or -1)
 *
 * GC note: where a new heap string is allocated from data that lives
 * on the process heap, we first copy the bytes into a malloc'd
 * scratch buffer (GC-invisible) — mirroring net.c's pattern — so a
 * GC triggered inside val_string cannot move the source out from
 * under us.
 */

#include "ta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Val str_char_at(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;
    if (!val_is_string(args[0])) return val_int(-1);
    HeapString *hs = val_get_string(args[0]);
    int64_t i = val_get_int(args[1]);
    if (i < 0 || i >= hs->len) return val_int(-1);
    return val_int((unsigned char)hs->data[i]);
}

static Val str_length(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;
    if (!val_is_string(args[0])) return val_int(0);
    return val_int(val_get_string(args[0])->len);
}

static Val str_substr(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_string(args[0])) return val_string(p, "", 0);
    HeapString *hs = val_get_string(args[0]);
    int64_t start = val_get_int(args[1]);
    int64_t slen  = val_get_int(args[2]);
    if (start < 0 || slen < 0 || start > hs->len)
        return val_string(p, "", 0);
    if (start + slen > hs->len) slen = hs->len - start;

    char *tmp = malloc((size_t)(slen > 0 ? slen : 1));
    if (!tmp) return val_string(p, "", 0);
    memcpy(tmp, hs->data + start, (size_t)slen);
    Val result = val_string(p, tmp, (int)slen);
    free(tmp);
    return result;
}

static Val str_concat(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_string(args[0]) || !val_is_string(args[1]))
        return val_string(p, "", 0);
    HeapString *a = val_get_string(args[0]);
    HeapString *b = val_get_string(args[1]);
    int total = a->len + b->len;

    char *tmp = malloc((size_t)(total > 0 ? total : 1));
    if (!tmp) return val_string(p, "", 0);
    memcpy(tmp, a->data, (size_t)a->len);
    memcpy(tmp + a->len, b->data, (size_t)b->len);
    Val result = val_string(p, tmp, total);
    free(tmp);
    return result;
}

static Val str_to_int(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;
    if (!val_is_string(args[0])) return val_int(0);
    HeapString *hs = val_get_string(args[0]);
    int i = 0, len = hs->len;
    while (i < len && (hs->data[i] == ' ' || hs->data[i] == '\t')) i++;
    if (i >= len) return val_int(0);

    int neg = 0;
    if (hs->data[i] == '-') { neg = 1; i++; }
    else if (hs->data[i] == '+') { i++; }

    int64_t val = 0;
    int seen = 0;
    while (i < len && hs->data[i] >= '0' && hs->data[i] <= '9') {
        val = val * 10 + (hs->data[i] - '0');
        seen = 1;
        i++;
    }
    if (!seen) return val_int(0);
    return val_int(neg ? -val : val);
}

static Val str_from_int(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;
    Proc *p = tls_current_proc;
    int64_t n = val_get_int(args[0]);
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%lld", (long long)n);
    return val_string(p, buf, len);
}

static Val str_eq_fn(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;
    if (!val_is_string(args[0]) || !val_is_string(args[1]))
        return val_int(0);
    HeapString *a = val_get_string(args[0]);
    HeapString *b = val_get_string(args[1]);
    if (a->len != b->len) return val_int(0);
    return val_int(memcmp(a->data, b->data, (size_t)a->len) == 0 ? 1 : 0);
}

static Val str_index_of(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;
    if (!val_is_string(args[0]) || !val_is_string(args[1]))
        return val_int(-1);
    HeapString *hay = val_get_string(args[0]);
    HeapString *needle = val_get_string(args[1]);

    if (needle->len == 0) return val_int(0);
    if (needle->len > hay->len) return val_int(-1);

    for (int i = 0; i <= hay->len - needle->len; i++) {
        if (memcmp(hay->data + i, needle->data, (size_t)needle->len) == 0)
            return val_int(i);
    }
    return val_int(-1);
}

static Val str_to_sym(VM *vm, Val *args, int nargs) {
    (void)nargs;
    if (!val_is_string(args[0])) return val_nil();
    HeapString *s = val_get_string(args[0]);
    /* vm_intern_symbol needs null-terminated string */
    char buf[256];
    int len = s->len < 255 ? s->len : 255;
    memcpy(buf, s->data, (size_t)len);
    buf[len] = '\0';
    return val_symbol(vm_intern_symbol(vm, buf));
}

TaFunc str_funcs[] = {
    {"char_at",    str_char_at,    2},
    {"length",     str_length,     1},
    {"substr",     str_substr,     3},
    {"concat",     str_concat,     2},
    {"to_int",     str_to_int,     1},
    {"from_int",   str_from_int,   1},
    {"eq",         str_eq_fn,      2},
    {"index_of",   str_index_of,   2},
    {"to_sym",     str_to_sym,     1},
    {NULL, NULL, 0}
};

void vm_register_str_module(VM *vm) {
    vm_register_module(vm, "str", str_funcs, 9);
}