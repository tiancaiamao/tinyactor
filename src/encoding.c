/*
 * encoding.c — base64 / base64url / hex / percent codec module
 *
 *   encoding.raw_b64_encode(s)        -> String  (RFC 4648, padded with '=')
 *   encoding.raw_b64_decode(s)        -> String | -1
 *   encoding.raw_b64_url_encode(s)    -> String  (-_ alphabet, padded with '=')
 *   encoding.raw_b64_url_decode(s)    -> String | -1
 *   encoding.raw_hex_encode(s)        -> String  (lowercase)
 *   encoding.raw_hex_decode(s)        -> String | -1
 *   encoding.raw_pct_encode(s)        -> String  (RFC 3986 unreserved passthrough)
 *   encoding.raw_pct_decode(s)        -> String | -1
 *
 * Registration follows the os-module pattern (see src/os.c): the raw
 * primitives are registered under their full dotted names WITHOUT a
 * vm_register_module("encoding") entry — a module-registry entry would
 * make `import encoding` a compile-time no-op (vm.is_builtin_module) and
 * lib/encoding.ta — the Result lift layer that owns the public API —
 * would never load. The raw_* names also matter for the opposite reason:
 * codegen prefers the cfunc table over TA functions for a dotted symbol
 * (compile_call's vm.cfunc_index fast path), so a C name equal to a
 * public TA name (encoding.hex_decode) would call the C primitive
 * directly and silently bypass the lift in lib/encoding.ta.
 *
 * Semantics:
 *   base64: standard alphabet (A-Za-z0-9+/) vs url-safe variant (- and _
 *     in place of + and /, RFC 4648 §5). Both encoders emit canonical
 *     '=' padding to a multiple of 4. Both decoders accept their own
 *     alphabet only and are padding-lenient: canonical "Zm9vYg==" and
 *     unpadded "Zm9vYg" both decode; a lone '=' in the middle, a
 *     trailing run longer than "==", or a dangling length (significant
 *     chars % 4 == 1) is a hard error.
 *   hex: encode is lowercase; decode accepts both cases. Odd input
 *     length or any non-hexdigit byte -> -1.
 *   percent: encode passes RFC 3986 unreserved bytes (A-Za-z0-9-_.~)
 *     through and emits uppercase %XX for everything else (bytes, not
 *     codepoints — multi-byte UTF-8 just becomes three escapes).
 *     decode maps %XX back to the raw byte; a '%' not followed by two
 *     hexdigits -> -1. '+' is NOT decoded as space (that is the
 *     form-urlencoding variant, not RFC 3986) — round-trips with
 *     percent_encode are exact.
 *
 * Signal vocabulary (docs/c-module.md): a non-string argument and every
 * malformed input decode to the hard error -1; the lift into Result
 * happens in lib/encoding.ta.
 *
 * GC/allocation note: same pattern as src/str.c — the output is built
 * in a malloc'd scratch buffer (GC-invisible), copied into the process
 * heap with one val_string, and the scratch is freed on every path.
 */

#include "ta.h"
#include <stdlib.h>
#include <string.h>

static const char B64_STD[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B64_URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static Val b64_encode_impl(Val *args, const char *alpha) {
    Proc *p = tls_current_proc;
    if (!val_is_string(args[0]))
        return val_int(-1);
    HeapString *hs = val_get_string(args[0]);
    int64_t n = hs->len;
    int64_t out_len = ((n + 2) / 3) * 4;
    char *tmp = malloc((size_t)out_len + 1);
    if (!tmp)
        return val_int(-1);
    int64_t o = 0;
    for (int64_t i = 0; i < n; i += 3) {
        uint32_t trip = ((uint32_t)(unsigned char)hs->data[i] << 16) |
                        ((i + 1 < n ? (uint32_t)(unsigned char)hs->data[i + 1] : 0) << 8) |
                        (i + 2 < n ? (uint32_t)(unsigned char)hs->data[i + 2] : 0);
        tmp[o++] = alpha[(trip >> 18) & 63];
        tmp[o++] = alpha[(trip >> 12) & 63];
        tmp[o++] = (i + 1 < n) ? alpha[(trip >> 6) & 63] : '=';
        tmp[o++] = (i + 2 < n) ? alpha[trip & 63] : '=';
    }
    Val result = val_string(p, tmp, out_len);
    free(tmp);
    return result;
}

/* strchr is unsafe for NUL bytes (it would report the terminator), so
 * the reverse lookup is a guard + strchr. */
static int b64_index(unsigned char c, const char *alpha) {
    if (c == '\0')
        return -1;
    const char *q = strchr(alpha, (char)c);
    return q ? (int)(q - alpha) : -1;
}

/* Shared decode: own alphabet only, padding-lenient (see header comment). */
static Val b64_decode_impl(Val *args, const char *alpha) {
    Proc *p = tls_current_proc;
    if (!val_is_string(args[0]))
        return val_int(-1);
    HeapString *hs = val_get_string(args[0]);

    /* Strip a single trailing '=' run (at most two); any '=' elsewhere
     * is caught by the alphabet lookup below. */
    int64_t len = hs->len;
    int pad = 0;
    while (len > 0 && hs->data[len - 1] == '=' && pad < 2) {
        len--;
        pad++;
    }
    if (hs->len > 0 && len > 0 && hs->data[len - 1] == '=')
        return val_int(-1); /* "x===" — run longer than two */
    if (len % 4 == 1)
        return val_int(-1); /* dangling character */
    if (pad > 0 && (len + pad) % 4 != 0)
        return val_int(-1); /* "QQ=" — padding does not complete a quad */

    char *tmp = malloc((size_t)((len / 4) * 3 + 2));
    if (!tmp)
        return val_int(-1);
    int64_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (int64_t i = 0; i < len; i++) {
        int v = b64_index((unsigned char)hs->data[i], alpha);
        if (v < 0) {
            free(tmp);
            return val_int(-1);
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits == 24) {
            tmp[o++] = (char)(acc >> 16);
            tmp[o++] = (char)(acc >> 8);
            tmp[o++] = (char)acc;
            acc = 0;
            bits = 0;
        }
    }
    if (bits == 12)
        tmp[o++] = (char)(acc >> 4);
    else if (bits == 18) {
        tmp[o++] = (char)(acc >> 10);
        tmp[o++] = (char)(acc >> 2);
    }
    Val result = val_string(p, tmp, o);
    free(tmp);
    return result;
}

static Val enc_b64_encode(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    return b64_encode_impl(args, B64_STD);
}

static Val enc_b64_decode(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    return b64_decode_impl(args, B64_STD);
}

static Val enc_b64_url_encode(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    return b64_encode_impl(args, B64_URL);
}

static Val enc_b64_url_decode(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    return b64_decode_impl(args, B64_URL);
}

/* ============================================================
 * hex
 * ============================================================ */

static int hex_index(unsigned char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static Val enc_hex_encode(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_string(args[0]))
        return val_int(-1);
    HeapString *hs = val_get_string(args[0]);
    static const char digits[] = "0123456789abcdef";
    char *tmp = malloc((size_t)hs->len * 2 + 1);
    if (!tmp)
        return val_int(-1);
    for (int i = 0; i < hs->len; i++) {
        unsigned char c = (unsigned char)hs->data[i];
        tmp[2 * i] = digits[c >> 4];
        tmp[2 * i + 1] = digits[c & 15];
    }
    Val result = val_string(p, tmp, (int64_t)hs->len * 2);
    free(tmp);
    return result;
}

static Val enc_hex_decode(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_string(args[0]))
        return val_int(-1);
    HeapString *hs = val_get_string(args[0]);
    if (hs->len % 2 != 0)
        return val_int(-1);
    char *tmp = malloc((size_t)hs->len / 2 + 1);
    if (!tmp)
        return val_int(-1);
    for (int i = 0; i < hs->len; i += 2) {
        int hi = hex_index((unsigned char)hs->data[i]);
        int lo = hex_index((unsigned char)hs->data[i + 1]);
        if (hi < 0 || lo < 0) {
            free(tmp);
            return val_int(-1);
        }
        tmp[i / 2] = (char)((hi << 4) | lo);
    }
    Val result = val_string(p, tmp, (int64_t)hs->len / 2);
    free(tmp);
    return result;
}

/* ============================================================
 * percent (RFC 3986)
 * ============================================================ */

static int is_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || c == '.' || c == '~';
}

static Val enc_percent_encode(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_string(args[0]))
        return val_int(-1);
    HeapString *hs = val_get_string(args[0]);
    static const char digits[] = "0123456789ABCDEF";
    char *tmp = malloc((size_t)hs->len * 3 + 1);
    if (!tmp)
        return val_int(-1);
    int64_t o = 0;
    for (int i = 0; i < hs->len; i++) {
        unsigned char c = (unsigned char)hs->data[i];
        if (is_unreserved(c)) {
            tmp[o++] = (char)c;
        } else {
            tmp[o++] = '%';
            tmp[o++] = digits[c >> 4];
            tmp[o++] = digits[c & 15];
        }
    }
    Val result = val_string(p, tmp, o);
    free(tmp);
    return result;
}

static Val enc_percent_decode(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!val_is_string(args[0]))
        return val_int(-1);
    HeapString *hs = val_get_string(args[0]);
    char *tmp = malloc((size_t)hs->len + 1);
    if (!tmp)
        return val_int(-1);
    int64_t o = 0;
    for (int i = 0; i < hs->len; i++) {
        unsigned char c = (unsigned char)hs->data[i];
        if (c != '%') {
            tmp[o++] = (char)c;
            continue;
        }
        if (i + 2 >= hs->len) { /* dangling "%", "%2", "%2G"... */
            free(tmp);
            return val_int(-1);
        }
        int hi = hex_index((unsigned char)hs->data[i + 1]);
        int lo = hex_index((unsigned char)hs->data[i + 2]);
        if (hi < 0 || lo < 0) {
            free(tmp);
            return val_int(-1);
        }
        tmp[o++] = (char)((hi << 4) | lo);
        i += 2;
    }
    Val result = val_string(p, tmp, o);
    free(tmp);
    return result;
}

TaFunc encoding_funcs[] = {{"raw_b64_encode", enc_b64_encode, 1},
                           {"raw_b64_decode", enc_b64_decode, 1},
                           {"raw_b64_url_encode", enc_b64_url_encode, 1},
                           {"raw_b64_url_decode", enc_b64_url_decode, 1},
                           {"raw_hex_encode", enc_hex_encode, 1},
                           {"raw_hex_decode", enc_hex_decode, 1},
                           {"raw_pct_encode", enc_percent_encode, 1},
                           {"raw_pct_decode", enc_percent_decode, 1},
                           {NULL, NULL, 0}};

/* Full dotted names only, no module-registry entry — see the header
 * comment (and the matching note in src/os.c). */
void vm_register_encoding_module(VM *vm) {
    for (int i = 0; encoding_funcs[i].name != NULL; i++) {
        char qualified[64];
        snprintf(qualified, sizeof(qualified), "encoding.%s", encoding_funcs[i].name);
        vm_register(vm, qualified, encoding_funcs[i].fn, encoding_funcs[i].nargs);
    }
}