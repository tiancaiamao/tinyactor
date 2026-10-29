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
 *   str.to_sym(s)        -> Symbol
 *   str.sym_to_str(sym)  -> String
 *   str.chr(n)           -> String
 *   str.split(s, sep)    -> List  (list of strings)
 *   str.join(lst, sep)   -> String
 *   str.trim(s)          -> String
 *   str.replace(s,old,new) -> String (all occurrences)
 *   str.upper(s)         -> String
 *   str.lower(s)         -> String
 *   str.pad_start(s,n,ch)  -> String (n, ch: Int)
 *   str.pad_end(s,n,ch)    -> String
 *   str.parse_float(s)   -> Option (Some(float) or None)
 *   str.from_float(f)    -> String
 *   str.from_base(s, b)  -> Int   (-1 on bad base/digit)
 *   str.to_base(n, b)    -> String ("" on bad base)
 *
 * Registration follows the buffer-module lazy-dylib convention (see
 * lib/buffer.c): the primitives are declared with bare names and
 * registered under the full dotted names by vm_load_self via
 * vm_register_module("str"), which the loader dlopen's on first
 * reference to a str.* symbol (find_cfunc_autoload in src/main.c).
 * Because the dylib loads lazily — after compile-time import resolution
 * — `import str` loads lib/str.ta (the external-fn signature layer for
 * the upper functions below: split/join/trim/replace/upper/lower/pad/
 * parsing). The low-level primitives (char_at/chr/length/substr/concat/
 * to_int/from_int/eq/index_of/to_sym/sym_to_str) are compiler-promised
 * builtins (typecheck.ta seeds precise types for them), so they stay
 * available without `import str` and are deliberately not redeclared in
 * lib/str.ta (an external-fn signature would shadow the precise promise).
 *
 * ASCII scope (v1): strings are byte arrays and "codepoint" counts are
 * byte counts. Everything below that transforms or measures characters
 * (trim/upper/lower/pad, empty-sep split) works on the ASCII range only
 * — bytes >= 0x80 pass through unchanged (upper/lower) or count as one
 * codepoint each (pad/length). Full UTF-8 semantics are deferred to the
 * planned unicode module.
 *
 * Semantics of the new functions:
 *   split: non-overlapping left-to-right sep matches; consecutive seps
 *     produce empty pieces ("a,,b" -> ["a","","b"]). Empty sep splits
 *     into 1-byte pieces (Go strings.Split semantics). sep longer than
 *     s -> [s]. Wrong arg types -> empty list (nil).
 *   join: sep between elements; a non-string element or improper list
 *     -> "" (whole result abandoned, not silently skipped).
 *   replace: old == "" -> s unchanged (no insert-between, unlike Go).
 *   trim: ASCII space \t \n \r \v \f from both ends.
 *   pad_start/pad_end: pad with repeats of ch (a byte value 0..255) up
 *     to n codepoints; n <= length -> s unchanged; invalid ch -> s
 *     unchanged.
 *   parse_float: full-string parse — both ends trimmed of ASCII
 *     whitespace, then the entire remainder must parse (strtod).
 *     Overflow -> inf (same as float arithmetic); garbage or empty
 *     input -> None. Returns Option directly from C: Some(f) is the
 *     pair ('Some . (f . nil)), None is the 'None symbol — the same
 *     runtime shape the compiler gives option.ta's constructors, so TA
 *     code can match on it with no lift layer.
 *   from_float: "%g" — 6 significant digits, trailing zeros trimmed
 *     ("1.5", "0.0001", "1e+06" from 1e06 up; "inf"/"nan" specials).
 *   from_base/to_base: bases 2/8/16 only. Unsigned 48-bit semantics:
 *     to_base renders n as an unsigned 48-bit two's-complement value
 *     (to_base(-1, 16) = "ffffffffffff"); from_base parses an unsigned
 *     magnitude and wraps it into int48 (>= 2^47 wraps negative). No
 *     sign character accepted. Digits are lowercase; from_base also
 *     accepts uppercase. Bad base or bad digit -> -1 / "".
 *
 * GC note: C module callbacks run with the GC gate closed by the VM
 * (do_ccall) and their allocations routed to the chunk arena, so
 * intermediate heap Vals held in C locals stay valid; the result
 * converges into the process heap at callback exit. Where a new heap
 * string is allocated from data that lives on the process heap, we
 * first copy the bytes into a malloc'd scratch buffer (GC-invisible)
 * — mirroring net.c's pattern — so the code stays correct even if
 * called outside the callback window; every malloc has a matching free
 * on all paths before return.
 */

#include "ta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Val str_char_at(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_string(args[0]))
        return val_int(-1);
    HeapString *hs = val_get_string(args[0]);
    int64_t i = val_get_int(args[1]);
    if (i < 0 || i >= hs->len)
        return val_int(-1);
    return val_int((unsigned char)hs->data[i]);
}

/* str.chr: int -> string — inverse of str.char_at (byte value -> 1-char string). */
static Val str_chr_fn(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_int(args[0]))
        return val_string(p, "", 0);
    int64_t c = val_get_int(args[0]);
    if (c < 0 || c > 255)
        return val_string(p, "", 0);
    unsigned char buf[1];
    buf[0] = (unsigned char)c;
    return val_string(p, (const char *)buf, 1);
}

static Val str_length(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_string(args[0]))
        return val_int(0);
    return val_int(val_get_string(args[0])->len);
}

static Val str_substr(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_string(args[0]))
        return val_string(p, "", 0);
    HeapString *hs = val_get_string(args[0]);
    int64_t start = val_get_int(args[1]);
    int64_t slen = val_get_int(args[2]);
    if (start < 0 || slen < 0 || start > hs->len)
        return val_string(p, "", 0);
    if (start + slen > hs->len)
        slen = hs->len - start;

    char *tmp = malloc((size_t)(slen > 0 ? slen : 1));
    if (!tmp)
        return val_string(p, "", 0);
    memcpy(tmp, hs->data + start, (size_t)slen);
    Val result = val_string(p, tmp, (int)slen);
    free(tmp);
    return result;
}

static Val str_concat(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_string(args[0]) || !val_is_string(args[1]))
        return val_string(p, "", 0);
    HeapString *a = val_get_string(args[0]);
    HeapString *b = val_get_string(args[1]);
    int total = a->len + b->len;

    char *tmp = malloc((size_t)(total > 0 ? total : 1));
    if (!tmp)
        return val_string(p, "", 0);
    memcpy(tmp, a->data, (size_t)a->len);
    memcpy(tmp + a->len, b->data, (size_t)b->len);
    Val result = val_string(p, tmp, total);
    free(tmp);
    return result;
}

static Val str_to_int(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_string(args[0]))
        return val_int(0);
    HeapString *hs = val_get_string(args[0]);
    int i = 0, len = hs->len;
    while (i < len && (hs->data[i] == ' ' || hs->data[i] == '\t'))
        i++;
    if (i >= len)
        return val_int(0);

    int neg = 0;
    if (hs->data[i] == '-') {
        neg = 1;
        i++;
    } else if (hs->data[i] == '+') {
        i++;
    }

    int64_t val = 0;
    int seen = 0;
    while (i < len && hs->data[i] >= '0' && hs->data[i] <= '9') {
        val = val * 10 + (hs->data[i] - '0');
        seen = 1;
        i++;
    }
    if (!seen)
        return val_int(0);
    return val_int(neg ? -val : val);
}

static Val str_from_int(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    int64_t n = val_get_int(args[0]);
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%lld", (long long)n);
    return val_string(p, buf, len);
}

static Val str_eq_fn(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_string(args[0]) || !val_is_string(args[1]))
        return val_false();
    HeapString *a = val_get_string(args[0]);
    HeapString *b = val_get_string(args[1]);
    if (a->len != b->len)
        return val_false();
    return memcmp(a->data, b->data, (size_t)a->len) == 0 ? val_true() : val_false();
}

static Val str_index_of(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_string(args[0]) || !val_is_string(args[1]))
        return val_int(-1);
    HeapString *hay = val_get_string(args[0]);
    HeapString *needle = val_get_string(args[1]);

    if (needle->len == 0)
        return val_int(0);
    if (needle->len > hay->len)
        return val_int(-1);

    /* memchr jumps straight to the needle's first byte: a per-position
     * memcmp paid a call for every offset (a CRLFCRLF search in a
     * 131-byte request head measured ~270ns, ~6x what the scan needs). */
    const char *cur = hay->data;
    int left = hay->len - needle->len + 1;
    while (left > 0) {
        const char *hit = memchr(cur, needle->data[0], (size_t)left);
        if (!hit)
            return val_int(-1);
        if (memcmp(hit, needle->data, (size_t)needle->len) == 0)
            return val_int((int64_t)(hit - hay->data));
        left -= (int)(hit - cur) + 1;
        cur = hit + 1;
    }
    return val_int(-1);
}

/* str.index_of_char: first index >= start whose byte equals c (a byte
 * value), or -1. A plain scan primitive — callers compose it (e.g. the
 * JSON escaper takes the min over the bytes that need escaping). */
static Val str_index_of_char(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_string(args[0]))
        return val_int(-1);
    HeapString *hs = val_get_string(args[0]);
    int64_t start = val_get_int(args[1]);
    if (start < 0)
        start = 0;
    int64_t c = val_get_int(args[2]);
    for (int64_t i = start; i < hs->len; i++)
        if ((unsigned char)hs->data[i] == (unsigned char)c)
            return val_int(i);
    return val_int(-1);
}

/* str.index_of_ctrl: first index >= start of a control byte (< 0x20),
 * or -1. The range form of index_of_char — one pass instead of a
 * per-character TA loop. */
static Val str_index_of_ctrl(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_string(args[0]))
        return val_int(-1);
    HeapString *hs = val_get_string(args[0]);
    int64_t start = val_get_int(args[1]);
    if (start < 0)
        start = 0;
    for (int64_t i = start; i < hs->len; i++)
        if ((unsigned char)hs->data[i] < 0x20)
            return val_int(i);
    return val_int(-1);
}

/* str.cmp: three-way byte-lexicographic compare (-1 / 0 / 1), the
 * dict/sort comparator shape. One C pass — the previous TA loop made
 * one char_at call per character per comparison (issue #402). */
static Val str_cmp_fn(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_string(args[0]) || !val_is_string(args[1]))
        return val_int(0);
    HeapString *a = val_get_string(args[0]);
    HeapString *b = val_get_string(args[1]);
    int64_t n = a->len < b->len ? a->len : b->len;
    for (int64_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a->data[i];
        unsigned char cb = (unsigned char)b->data[i];
        if (ca != cb)
            return val_int(ca < cb ? -1 : 1);
    }
    if (a->len == b->len)
        return val_int(0);
    return val_int(a->len < b->len ? -1 : 1);
}

static Val str_to_sym(VM *vm, Val *args, int nargs) {
    (void)nargs;
    if (!val_is_string(args[0]))
        return val_nil();
    HeapString *s = val_get_string(args[0]);
    /* vm_intern_symbol needs null-terminated string */
    char buf[256];
    int len = s->len < 255 ? s->len : 255;
    memcpy(buf, s->data, (size_t)len);
    buf[len] = '\0';
    return val_symbol(vm_intern_symbol(vm, buf));
}

static Val sym_to_str(VM *vm, Val *args, int nargs) {
    (void)nargs;
    if (!val_is_symbol(args[0])) {
        /* Strict, like the cartype/cdrtype opcode errors: silently
         * returning nil let type errors surface far from their cause
         * (issue #101). Reachable only via dynamically-typed values
         * (FFI boundaries) — the static path is rejected by typecheck. */
        fprintf(stderr, "error: str.sym_to_str: expected symbol, got tag=0x%04llx (raw=0x%llx)\n",
                (unsigned long long)(args[0] >> 48), (unsigned long long)args[0]);
        return val_nil();
    }
    uint32_t idx = (uint32_t)val_get_symbol(args[0]);
    const char *name = vm_symbol_name(vm, idx);
    if (!name) {
        fprintf(stderr, "error: str.sym_to_str: symbol idx %u out of range\n", idx);
        return val_nil();
    }
    return val_string(tls_current_proc, name, (int)strlen(name));
}

/* ---------- split / join / trim / replace / case / pad / float / base ---------- */

/* ASCII whitespace class shared by trim and parse_float. */
static int is_ascii_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/* trim ASCII whitespace from both ends; wrong type -> "". */
static Val str_trim(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_string(args[0]))
        return val_string(p, "", 0);
    HeapString *hs = val_get_string(args[0]);
    int start = 0, end = hs->len;
    while (start < end && is_ascii_space((unsigned char)hs->data[start]))
        start++;
    while (end > start && is_ascii_space((unsigned char)hs->data[end - 1]))
        end--;
    char *tmp = malloc((size_t)(end - start > 0 ? end - start : 1));
    if (!tmp)
        return val_string(p, "", 0);
    memcpy(tmp, hs->data + start, (size_t)(end - start));
    Val result = val_string(p, tmp, end - start);
    free(tmp);
    return result;
}

/* Split s on non-overlapping sep matches; empty sep -> 1-byte pieces.
 * Wrong types -> nil. GC safety: the whole list is built inside the
 * callback window (gate closed, chunk arena), so the intermediate pair
 * Vals accumulated in `acc` cannot move. */
static Val str_split(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_string(args[0]) || !val_is_string(args[1]))
        return val_nil();
    HeapString *s = val_get_string(args[0]);
    HeapString *sep = val_get_string(args[1]);

    if (sep->len == 0) {
        Val acc = val_nil();
        for (int i = s->len - 1; i >= 0; i--)
            acc = val_pair(p, val_string(p, s->data + i, 1), acc);
        return acc;
    }
    if (sep->len > s->len)
        return val_pair(p, val_string(p, s->data, s->len), val_nil());

    /* Record piece starts (piece k spans starts[k] .. next match). */
    int cap = s->len / sep->len + 2;
    int *starts = malloc((size_t)cap * sizeof(int));
    if (!starts)
        return val_nil();
    int npieces = 1;
    starts[0] = 0;
    int pos = 0;
    while (pos <= s->len - sep->len) {
        if (memcmp(s->data + pos, sep->data, (size_t)sep->len) == 0) {
            if (npieces >= cap)
                break; /* cannot happen: cap bounds non-overlapping matches */
            starts[npieces++] = pos + sep->len;
            pos += sep->len;
        } else {
            pos++;
        }
    }

    Val acc = val_nil();
    for (int k = npieces - 1; k >= 0; k--) {
        int start = starts[k];
        int end = (k + 1 < npieces) ? starts[k + 1] - sep->len : s->len;
        acc = val_pair(p, val_string(p, s->data + start, end - start), acc);
    }
    free(starts);
    return acc;
}

/* str.pairs: split s into (key . value) cons cells — one line per
 * line_sep, split at the FIRST kv_sep of the line, both sides trimmed of
 * ASCII space; a line without kv_sep (or with kv_sep first: no name) is
 * skipped. Arrival order is preserved, so folding the cells into a dict
 * keeps its last-one-wins rule on repeated keys.
 *
 * This is the header-block idiom (HTTP head, mail/meta blocks) as ONE
 * call: composed from substr/trim/index_of in TA it cost a round trip
 * per field (~5 per header, ~1.5us for a 6-header request). GC safety:
 * the list is built inside the callback window (gate closed, chunk
 * arena), so the Vals accumulated in `acc` cannot move. */
static Val str_pairs(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_string(args[0]) || !val_is_string(args[1]) || !val_is_string(args[2]))
        return val_nil();
    HeapString *s = val_get_string(args[0]);
    HeapString *lsep = val_get_string(args[1]);
    HeapString *ksep = val_get_string(args[2]);
    if (lsep->len == 0 || ksep->len == 0 || s->len == 0)
        return val_nil();

    /* bounds[nf] = (kstart, kend, vstart, vend); lines >= 1 byte apart. */
    int cap = s->len / lsep->len + 2;
    int *bounds = malloc((size_t)cap * 4 * sizeof(int));
    if (!bounds)
        return val_nil();
    int nf = 0;
    int pos = 0;
    while (pos <= s->len) {
        int eol = pos;
        int last = s->len - lsep->len;
        while (eol <= last && memcmp(s->data + eol, lsep->data, (size_t)lsep->len) != 0)
            eol++;
        if (eol > last)
            eol = s->len; /* final line, no trailing separator */
        int colon = -1;
        for (int i = pos; i <= eol - ksep->len; i++)
            if (memcmp(s->data + i, ksep->data, (size_t)ksep->len) == 0) {
                colon = i;
                break;
            }
        if (colon > pos) {
            int a = pos, b = colon;
            int v = colon + ksep->len, w = eol;
            while (a < b && is_ascii_space((unsigned char)s->data[a]))
                a++;
            while (b > a && is_ascii_space((unsigned char)s->data[b - 1]))
                b--;
            while (v < w && is_ascii_space((unsigned char)s->data[v]))
                v++;
            while (w > v && is_ascii_space((unsigned char)s->data[w - 1]))
                w--;
            if (nf < cap) {
                bounds[nf * 4 + 0] = a;
                bounds[nf * 4 + 1] = b;
                bounds[nf * 4 + 2] = v;
                bounds[nf * 4 + 3] = w;
                nf++;
            }
        }
        if (eol >= s->len)
            break;
        pos = eol + lsep->len;
    }

    Val acc = val_nil();
    for (int k = nf - 1; k >= 0; k--) {
        Val key = val_string(p, s->data + bounds[k * 4 + 0], bounds[k * 4 + 1] - bounds[k * 4 + 0]);
        Val val = val_string(p, s->data + bounds[k * 4 + 2], bounds[k * 4 + 3] - bounds[k * 4 + 2]);
        acc = val_pair(p, val_pair(p, key, val), acc);
    }
    free(bounds);
    return acc;
}

/* Join a proper list of strings with sep; any non-string element or an
 * improper tail abandons the whole result -> "". */
static Val str_join(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_string(args[1]))
        return val_string(p, "", 0);
    HeapString *sep = val_get_string(args[1]);

    int total = 0, count = 0;
    Val node = args[0];
    while (val_is_pair(node)) {
        Val elem = val_get_car(node);
        if (!val_is_string(elem))
            return val_string(p, "", 0);
        total += val_get_string(elem)->len;
        count++;
        node = val_get_cdr(node);
    }
    if (!val_is_nil(node))
        return val_string(p, "", 0);
    if (count > 0)
        total += (count - 1) * sep->len;

    char *tmp = malloc((size_t)(total > 0 ? total : 1));
    if (!tmp)
        return val_string(p, "", 0);
    int off = 0, k = 0;
    node = args[0];
    while (val_is_pair(node)) {
        HeapString *elem = val_get_string(val_get_car(node));
        /* sep before every element except the first — tracked by index,
         * not by `off`, or an empty first element would swallow its sep. */
        if (k > 0) {
            memcpy(tmp + off, sep->data, (size_t)sep->len);
            off += sep->len;
        }
        memcpy(tmp + off, elem->data, (size_t)elem->len);
        off += elem->len;
        k++;
        node = val_get_cdr(node);
    }
    Val result = val_string(p, tmp, total);
    free(tmp);
    return result;
}

/* Replace every non-overlapping occurrence of old with new. old == ""
 * -> s unchanged (documented divergence from Go's insert-between).
 * Wrong types -> "". */
static Val str_replace(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_string(args[0]) || !val_is_string(args[1]) || !val_is_string(args[2]))
        return val_string(p, "", 0);
    HeapString *s = val_get_string(args[0]);
    HeapString *old = val_get_string(args[1]);
    HeapString *new = val_get_string(args[2]);
    if (old->len == 0)
        return val_string(p, s->data, s->len);

    /* First pass: count matches to size the scratch buffer exactly. */
    int count = 0;
    for (int i = 0; i <= s->len - old->len;) {
        if (memcmp(s->data + i, old->data, (size_t)old->len) == 0) {
            count++;
            i += old->len;
        } else {
            i++;
        }
    }
    long rlen = (long)s->len - (long)count * ((long)old->len - (long)new->len);
    char *tmp = malloc((size_t)(rlen > 0 ? rlen : 1));
    if (!tmp)
        return val_string(p, "", 0);

    int off = 0, i = 0;
    while (i < s->len) {
        if (i <= s->len - old->len && memcmp(s->data + i, old->data, (size_t)old->len) == 0) {
            memcpy(tmp + off, new->data, new->len);
            off += new->len;
            i += old->len;
        } else {
            tmp[off++] = s->data[i++];
        }
    }
    Val result = val_string(p, tmp, off);
    free(tmp);
    return result;
}

/* ASCII-only case mapping (A-Z <-> a-z); other bytes pass through. */
static Val str_upper(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_string(args[0]))
        return val_string(p, "", 0);
    HeapString *hs = val_get_string(args[0]);
    char *tmp = malloc((size_t)(hs->len > 0 ? hs->len : 1));
    if (!tmp)
        return val_string(p, "", 0);
    for (int i = 0; i < hs->len; i++) {
        unsigned char c = (unsigned char)hs->data[i];
        tmp[i] = (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
    Val result = val_string(p, tmp, hs->len);
    free(tmp);
    return result;
}

static Val str_lower(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_string(args[0]))
        return val_string(p, "", 0);
    HeapString *hs = val_get_string(args[0]);
    char *tmp = malloc((size_t)(hs->len > 0 ? hs->len : 1));
    if (!tmp)
        return val_string(p, "", 0);
    for (int i = 0; i < hs->len; i++) {
        unsigned char c = (unsigned char)hs->data[i];
        tmp[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    Val result = val_string(p, tmp, hs->len);
    free(tmp);
    return result;
}

/* Pad with repeats of ch (a single byte value) to n codepoints.
 * n <= length -> s unchanged; invalid ch -> s unchanged. */
static Val pad(Proc *p, HeapString *hs, int64_t n, int64_t ch, int at_start) {
    if (ch < 0 || ch > 255)
        return val_string(p, hs->data, hs->len);
    int64_t padlen = n - hs->len;
    if (padlen <= 0)
        return val_string(p, hs->data, hs->len);
    int total = (int)(hs->len + padlen);
    char *tmp = malloc((size_t)total);
    if (!tmp)
        return val_string(p, hs->data, hs->len);
    char *body = at_start ? tmp + padlen : tmp;
    memcpy(body, hs->data, (size_t)hs->len);
    if (at_start)
        memset(tmp, (int)ch, (size_t)padlen);
    else
        memset(tmp + hs->len, (int)ch, (size_t)padlen);
    Val result = val_string(p, tmp, total);
    free(tmp);
    return result;
}

static Val str_pad_start(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_string(args[0]) || !val_is_int(args[1]) || !val_is_int(args[2]))
        return val_string(p, "", 0);
    return pad(p, val_get_string(args[0]), val_get_int(args[1]), val_get_int(args[2]), 1);
}

static Val str_pad_end(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_string(args[0]) || !val_is_int(args[1]) || !val_is_int(args[2]))
        return val_string(p, "", 0);
    return pad(p, val_get_string(args[0]), val_get_int(args[1]), val_get_int(args[2]), 0);
}

/* Full-string float parse -> Option. 'Some/'None are interned once at
 * registration (vm_register_str_module), matching the compiler's
 * constructor symbols so TA match works on the result. */
static int sym_some, sym_none;

static Val str_parse_float(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_string(args[0]))
        return val_symbol((uint32_t)sym_none);
    HeapString *hs = val_get_string(args[0]);

    int start = 0, end = hs->len;
    while (start < end && is_ascii_space((unsigned char)hs->data[start]))
        start++;
    while (end > start && is_ascii_space((unsigned char)hs->data[end - 1]))
        end--;

    /* strtod needs a NUL terminator; HeapString has none. */
    char *tmp = malloc((size_t)(end - start + 1));
    if (!tmp)
        return val_symbol((uint32_t)sym_none);
    memcpy(tmp, hs->data + start, (size_t)(end - start));
    tmp[end - start] = '\0';
    char *endp = NULL;
    double d = strtod(tmp, &endp);
    int ok = (endp == tmp + (end - start)) && (endp != tmp);
    free(tmp);
    if (!ok)
        return val_symbol((uint32_t)sym_none);
    return val_pair(p, val_symbol((uint32_t)sym_some), val_pair(p, val_float(d), val_nil()));
}

/* "%g": 6 significant digits, trailing zeros trimmed, exponent from
 * 1e05 offsets ("1.5", "0.0001", "1e+06"). Non-float input is widened
 * like val_to_double; anything else -> "0". */
static Val str_from_float(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    double d = 0.0;
    if (val_is_float(args[0]))
        d = val_get_float(args[0]);
    else if (val_is_int(args[0]))
        d = (double)val_get_int(args[0]);
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%g", d);
    return val_string(p, buf, len);
}

static int digit_val(unsigned char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Parse an unsigned base-2/8/16 magnitude, wrap into int48 via val_int.
 * No sign accepted; bad base, empty string or any bad digit -> -1. */
static Val str_from_base(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_string(args[0]) || !val_is_int(args[1]))
        return val_int(-1);
    HeapString *hs = val_get_string(args[0]);
    int64_t base = val_get_int(args[1]);
    if ((base != 2 && base != 8 && base != 16) || hs->len == 0)
        return val_int(-1);
    uint64_t acc = 0;
    for (int i = 0; i < hs->len; i++) {
        int d = digit_val((unsigned char)hs->data[i]);
        if (d < 0 || d >= base)
            return val_int(-1);
        acc = acc * (uint64_t)base + (uint64_t)d;
    }
    return val_int((int64_t)acc); /* val_int wraps into int48 */
}

/* Render n as an unsigned 48-bit two's-complement value in base
 * 2/8/16, lowercase digits. Bad base -> "". */
static Val str_to_base(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_int(args[0]) || !val_is_int(args[1]))
        return val_string(p, "", 0);
    int64_t base = val_get_int(args[1]);
    if (base != 2 && base != 8 && base != 16)
        return val_string(p, "", 0);
    uint64_t u = (uint64_t)val_get_int(args[0]) & 0xFFFFFFFFFFFFull; /* 48 bits */
    char buf[50];                                                    /* 48 binary digits + NUL */
    int i = 49;
    buf[i] = '\0';
    do {
        buf[--i] = "0123456789abcdef"[u % (uint64_t)base];
        u /= (uint64_t)base;
    } while (u > 0);
    return val_string(p, buf + i, 49 - i);
}

TaFunc str_funcs[] = {{"char_at", str_char_at, 2},
                      {"chr", str_chr_fn, 1},
                      {"length", str_length, 1},
                      {"substr", str_substr, 3},
                      {"concat", str_concat, 2},
                      {"to_int", str_to_int, 1},
                      {"from_int", str_from_int, 1},
                      {"eq", str_eq_fn, 2},
                      {"index_of", str_index_of, 2},
                      {"index_of_char", str_index_of_char, 3},
                      {"index_of_ctrl", str_index_of_ctrl, 2},
                      {"cmp", str_cmp_fn, 2},
                      {"to_sym", str_to_sym, 1},
                      {"sym_to_str", sym_to_str, 1},
                      {"split", str_split, 2},
                      {"pairs", str_pairs, 3},
                      {"join", str_join, 2},
                      {"trim", str_trim, 1},
                      {"replace", str_replace, 3},
                      {"upper", str_upper, 1},
                      {"lower", str_lower, 1},
                      {"pad_start", str_pad_start, 3},
                      {"pad_end", str_pad_end, 3},
                      {"parse_float", str_parse_float, 1},
                      {"from_float", str_from_float, 1},
                      {"from_base", str_from_base, 2},
                      {"to_base", str_to_base, 2},
                      {NULL, NULL, 0}};

void vm_load_self(VM *vm) {
    /* Pre-intern the Option constructor symbols at init time (see the
     * symbol-table concurrency note in docs/c-module.md). */
    sym_some = vm_intern_symbol(vm, "Some");
    sym_none = vm_intern_symbol(vm, "None");
    /* sizeof keeps the count in sync with str_funcs — a hardcoded count
     * silently truncates entries appended to the table. */
    vm_register_module(vm, "str", str_funcs, (int)(sizeof(str_funcs) / sizeof(str_funcs[0]) - 1));
}