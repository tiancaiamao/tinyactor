/*
 * sha256.c — SHA-256 digest C module for TinyActor VM (lazy dylib
 * lib/sha256.dylib, OpenSSL-backed like lib/tls.c).
 *
 *   sha256.raw_new()                    -> a   a fresh digest state
 *   sha256.raw_update(state, data)      -> a   the next state
 *   sha256.raw_update_at(state, data, off, len) -> a
 *   sha256.raw_final(state)             -> a   the hex digest
 *   sha256.raw_digest(data)             -> a   the hex digest of one buffer
 *
 * The digest state is a VALUE. The state is OpenSSL's own SHA256_CTX — a
 * public, copyable struct — so "the digest so far" is 112 bytes the
 * caller owns: this module copies the state in, updates it, copies it
 * back. Nothing is kept between calls: no slot table, no handle, no free
 * list, no lock, no global of any kind.
 *
 * The actor owns the state the way it owns any other value — it threads
 * it through its own code, the way a genserver threads its state.
 * Copying a hasher to another actor copies the bytes, so the receiver
 * continues the sender's digest instead of aliasing one context.
 *
 * The previous design kept the contexts in a global slot table and handed
 * out int handles. Two actors holding one handle had to be arbitrated
 * ("whoever finalises first wins"), which is what the table lock, the
 * busy pin and the generation counter were for. The state is data, so it
 * is a value, and the whole apparatus disappears: there is no shared
 * context to arbitrate over, and nothing to free.
 *
 * OpenSSL's low-level SHA256_* API is used because it is the only one
 * whose state is a plain struct (EVP_MD_CTX is opaque, so an EVP context
 * cannot be carried in a value). It is marked deprecated in OpenSSL 3.0,
 * but the struct layout is public ABI and the functions are still
 * exported; the build silences the warning for this module.
 */

#include "ta.h"
#include <openssl/sha.h>
#include <string.h>

/* The state is OpenSSL's SHA256_CTX: a public struct, so the digest so
 * far is just bytes we can copy in and out of a call. */
#define STATE_BYTES ((int)sizeof(SHA256_CTX))

/* state_value: the state as a TA value (bytes the caller owns). */
static Val state_value(const SHA256_CTX *c) {
    return val_bytes(tls_current_proc, (const uint8_t *)c, STATE_BYTES);
}

/* state_load: the state out of a TA value. 0 when the value is not a
 * state (wrong type or wrong length) — the caller reports the hard
 * error -1. */
static int state_load(Val v, SHA256_CTX *out) {
    if (!val_is_bytes(v))
        return 0;
    HeapBytes *b = val_get_bytes(v);
    if (b->len != STATE_BYTES)
        return 0;
    memcpy(out, b->data, STATE_BYTES);
    return 1;
}

/* hex: the digest as 64 lowercase hex chars (SHA-256 is 32 bytes). */
static Val sha_hex(const unsigned char *md, unsigned int mdlen) {
    const char digits[] = "0123456789abcdef";
    char hex[65];
    if (mdlen != 32)
        return val_nil();
    for (unsigned int i = 0; i < mdlen; i++) {
        hex[2 * i] = digits[md[i] >> 4];
        hex[2 * i + 1] = digits[md[i] & 0xf];
    }
    hex[64] = '\0';
    return val_string(tls_current_proc, hex, 64);
}

/* raw_new: a fresh state — the digest of the empty message. */
static Val sha_raw_new(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    SHA256_CTX c;
    SHA256_Init(&c);
    return state_value(&c);
}

/* raw_update_at: the state after hashing data[off, off+len). The bytes
 * are read in place — the document is never copied. */
static Val sha_raw_update_at(VM *vm, Val *args, int nargs) {
    (void)vm;
    if (nargs < 4 || !val_is_int(args[2]) || !val_is_int(args[3]))
        return val_int(-1);
    SHA256_CTX c;
    if (!state_load(args[0], &c))
        return val_int(-1);
    if (!val_is_string(args[1]))
        return val_int(-1);
    HeapString *s = val_get_string(args[1]);
    int64_t off = val_get_int(args[2]);
    int64_t len = val_get_int(args[3]);
    if (off < 0 || len < 0 || off + len > (int64_t)s->len)
        return val_int(-1);
    SHA256_Update(&c, s->data + off, (size_t)len);
    return state_value(&c);
}

/* raw_update: the state after hashing the whole string. */
static Val sha_raw_update(VM *vm, Val *args, int nargs) {
    if (nargs < 2 || !val_is_string(args[1]))
        return val_int(-1);
    HeapString *s = val_get_string(args[1]);
    Val rest[4];
    rest[0] = args[0];
    rest[1] = args[1];
    rest[2] = val_int(0);
    rest[3] = val_int(s->len);
    return sha_raw_update_at(vm, rest, 4);
}

/* raw_final: the digest of everything hashed so far. The state is a
 * value, so finalising does not consume it: finalising twice gives the
 * same digest. */
static Val sha_raw_final(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    SHA256_CTX c;
    if (!state_load(args[0], &c))
        return val_int(-1);
    unsigned char md[SHA256_DIGEST_LENGTH];
    SHA256_Final(md, &c);
    return sha_hex(md, sizeof md);
}

/* raw_digest: the digest of one buffer — no state at all. */
static Val sha_raw_digest(VM *vm, Val *args, int nargs) {
    (void)vm;
    if (nargs < 1 || !val_is_string(args[0]))
        return val_int(-1);
    HeapString *s = val_get_string(args[0]);
    SHA256_CTX c;
    SHA256_Init(&c);
    SHA256_Update(&c, s->data, (size_t)s->len);
    unsigned char md[SHA256_DIGEST_LENGTH];
    SHA256_Final(md, &c);
    return sha_hex(md, sizeof md);
}

static TaFunc sha256_funcs[] = {
    {"raw_new", sha_raw_new, 0},
    {"raw_update", sha_raw_update, 2},
    {"raw_update_at", sha_raw_update_at, 4},
    {"raw_final", sha_raw_final, 1},
    {"raw_digest", sha_raw_digest, 1},
    {NULL, NULL, 0},
};

/* Dynamic module entry: dlsym("vm_load_self") after dlopen (main.c link_unit). */
void vm_load_self(VM *vm) { vm_register_module(vm, "sha256", sha256_funcs, 5); }