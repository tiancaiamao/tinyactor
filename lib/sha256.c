/*
 * sha256.c — SHA-256 digest C module for TinyActor VM (lazy dylib
 * lib/sha256.dylib, OpenSSL-backed like lib/tls.c).
 *
 *   sha256.raw_new()                  -> a        handle (nil on failure)
 *   sha256.raw_update(h, s)           -> Int      1 ok / 0 error
 *   sha256.raw_update_at(h, s, off, len) -> Int    1 ok / 0 error
 *   sha256.raw_final(h)               -> a        hex digest (nil on error)
 *   sha256.raw_digest(s)              -> a        hex digest (nil on error)
 *
  * The handle is a TA Bytes object whose payload is the EVP_MD_CTX pointer:
 * no static state, no registry, no lock — two actors hashing at the same
 * time share nothing, so the module is safe by construction rather than by
 * convention. The C side owns the context and frees it in raw_final, the
 * only owner of the pointer; raw_final zeroes the handle, so a stale handle
 * is inert (every call on it fails) instead of a use-after-free. An actor
 * that dies mid-digest leaks its context until the process exits (same trade
 * as lib/tls.c's slots).
 *
 * update reads the string's bytes in place (HeapString.data), so a big
 * document is hashed without copying it — that is what the offset/length
 * pair buys: the caller hashes a document in blocks without ever
 * materialising a slice of it.
 */

#include "ta.h"
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

static EVP_MD_CTX *sha_ctx(Val h) {
    if (!val_is_bytes(h))
        return NULL;
    HeapBytes *hb = val_get_bytes(h);
    if (hb->len != (int)sizeof(EVP_MD_CTX *))
        return NULL;
    EVP_MD_CTX *c = NULL;
    memcpy(&c, hb->data, sizeof(c));
    return c;
}

/* sha_ctx_clear: drop the context the handle points at. The handle's
 * payload is zeroed, so a second final() sees a NULL context instead of
 * freeing it twice — the caller keeps a stale handle, it does not get a
 * double free. */
static void sha_ctx_clear(Val h, EVP_MD_CTX *c) {
    EVP_MD_CTX_free(c);
    HeapBytes *hb = val_get_bytes(h);
    memset(hb->data, 0, hb->len);
}

/* hex: the digest as 64 lowercase hex chars (SHA-256 is 32 bytes). */
static Val sha_hex(Proc *p, const unsigned char *md, unsigned int mdlen) {
    static const char digits[] = "0123456789abcdef";
    char hex[65];
    if (mdlen != 32)
        return val_nil();
    for (unsigned int i = 0; i < mdlen; i++) {
        hex[2 * i] = digits[md[i] >> 4];
        hex[2 * i + 1] = digits[md[i] & 0xf];
    }
    hex[64] = '\0';
    return val_string(p, hex, 64);
}

static Val sha_new(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    if (!c)
        return val_nil();
    if (EVP_DigestInit_ex(c, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(c);
        return val_nil();
    }
    Proc *p = tls_current_proc;
    return val_bytes(p, (const uint8_t *)&c, sizeof(c));
}

/* update_at: hash s[off, off+len). The whole string is off=0, len=length. */
static Val sha_update_at(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    EVP_MD_CTX *c = sha_ctx(args[0]);
    if (!c || !val_is_string(args[1]) || !val_is_int(args[2]) || !val_is_int(args[3]))
        return val_int(0);
    HeapString *hs = val_get_string(args[1]);
    int64_t off = val_get_int(args[2]);
    int64_t len = val_get_int(args[3]);
    if (off < 0 || len < 0 || off + len > hs->len)
        return val_int(0);
    if (len == 0)
        return val_int(1);
    if (EVP_DigestUpdate(c, hs->data + off, (size_t)len) != 1)
        return val_int(0);
    return val_int(1);
}

static Val sha_update(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    EVP_MD_CTX *c = sha_ctx(args[0]);
    if (!c || !val_is_string(args[1]))
        return val_int(0);
    HeapString *hs = val_get_string(args[1]);
    if (hs->len == 0)
        return val_int(1);
    if (EVP_DigestUpdate(c, hs->data, (size_t)hs->len) != 1)
        return val_int(0);
    return val_int(1);
}

static Val sha_final(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    EVP_MD_CTX *c = sha_ctx(args[0]);
    if (!c)
        return val_nil();
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdlen = 0;
    int ok = EVP_DigestFinal_ex(c, md, &mdlen) == 1;
    sha_ctx_clear(args[0], c);
    if (!ok)
        return val_nil();
    return sha_hex(tls_current_proc, md, mdlen);
}

static Val sha_digest(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_string(args[0]))
        return val_nil();
    HeapString *hs = val_get_string(args[0]);
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdlen = 0;
    if (EVP_Digest(hs->data, (size_t)hs->len, md, &mdlen, EVP_sha256(), NULL) != 1)
        return val_nil();
    return sha_hex(tls_current_proc, md, mdlen);
}

static TaFunc sha_funcs[] = {
    {"raw_new", sha_new, 0},     {"raw_update", sha_update, 2}, {"raw_update_at", sha_update_at, 4},
    {"raw_final", sha_final, 1}, {"raw_digest", sha_digest, 1}, {NULL, NULL, 0}};

/* Lazy module entry: the loader dlsym's "vm_load_self" after dlopen
 * (main.c find_cfunc_autoload). os.c/tls.c registration pattern: dotted
 * names, NO module-registry entry — a registry entry for "sha256" would
 * make `import sha256` a compile-time no-op and lib/sha256.ta (the facade)
 * would never load. */
void vm_load_self(VM *vm) {
    for (int i = 0; sha_funcs[i].name != NULL; i++) {
        char qualified[64];
        snprintf(qualified, sizeof(qualified), "sha256.%s", sha_funcs[i].name);
        vm_register(vm, qualified, sha_funcs[i].fn, sha_funcs[i].nargs);
    }
}