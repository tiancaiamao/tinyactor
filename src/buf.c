/*
 * buf.c — Mutable byte buffer module for TinyActor VM
 *
 * TinyActor values are immutable, so buffers are kept in a global
 * C table and addressed by integer handle.
 *
 *   buf.new()              -> Int   (handle)
 *   buf.push_byte(b, n)    -> Int   (1 ok, 0 full/bad handle)
 *   buf.push_int32(b, n)   -> Int   (4 bytes, big-endian)
 *   buf.push_int64(b, n)   -> Int   (8 bytes, big-endian)
 *   buf.push_string(b, s)  -> Int   (raw string bytes)
 *   buf.write_to(b, path)  -> Int   (1 ok)
 *   buf.length(b)          -> Int
 *   buf.get_byte(b, i)     -> Int   (byte, or -1 OOB)
 *   buf.from_file(path)    -> Int   (handle, or -1 on error)
 *
 * All buffer data lives in malloc'd memory (GC-invisible), so it is
 * stable across GC. String args are read and copied before any work.
 */

#include "ta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BUFFERS 256

typedef struct {
    uint8_t *data;
    int len;
    int cap;
} Buffer;

static Buffer buffers[MAX_BUFFERS];
static int next_buffer = 0;

/* Validate a handle and return its Buffer, or NULL if invalid.
 * Slots are never freed, so the [0, next_buffer) range fully
 * determines validity. */
static Buffer *buf_get(int64_t handle) {
    if (handle < 0 || handle >= next_buffer) return NULL;
    return &buffers[handle];
}

/* Ensure `add` bytes fit; grows on demand. Returns 1 ok, 0 OOM. */
static int buf_ensure(Buffer *b, int add) {
    if (b->len + add <= b->cap) return 1;
    int newcap = b->cap ? b->cap : 16;
    while (newcap < b->len + add) newcap *= 2;
    uint8_t *nd = realloc(b->data, (size_t)newcap);
    if (!nd) return 0;
    b->data = nd;
    b->cap = newcap;
    return 1;
}

static Val buf_new(VM *vm, Val *args, int nargs) {
    (void)vm; (void)args; (void)nargs;
    if (next_buffer >= MAX_BUFFERS) return val_int(-1);
    int h = next_buffer++;
    buffers[h].data = NULL;
    buffers[h].len = 0;
    buffers[h].cap = 0;
    return val_int(h);
}

static Val buf_push_byte(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;
    Buffer *b = buf_get(val_get_int(args[0]));
    if (!b) return val_int(0);
    if (!buf_ensure(b, 1)) return val_int(0);
    b->data[b->len++] = (uint8_t)(val_get_int(args[1]) & 0xFF);
    return val_int(1);
}

static Val buf_push_int32(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;
    Buffer *b = buf_get(val_get_int(args[0]));
    if (!b) return val_int(0);
    if (!buf_ensure(b, 4)) return val_int(0);
    uint32_t u = (uint32_t)val_get_int(args[1]);
    b->data[b->len++] = (uint8_t)((u >> 24) & 0xFF);
    b->data[b->len++] = (uint8_t)((u >> 16) & 0xFF);
    b->data[b->len++] = (uint8_t)((u >> 8)  & 0xFF);
    b->data[b->len++] = (uint8_t)(u & 0xFF);
    return val_int(1);
}

static Val buf_push_int64(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;
    Buffer *b = buf_get(val_get_int(args[0]));
    if (!b) return val_int(0);
    if (!buf_ensure(b, 8)) return val_int(0);
    uint64_t u = (uint64_t)val_get_int(args[1]);
    for (int i = 0; i < 8; i++)
        b->data[b->len++] = (uint8_t)((u >> (56 - 8 * i)) & 0xFF);
    return val_int(1);
}

static Val buf_push_string(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;
    Buffer *b = buf_get(val_get_int(args[0]));
    if (!b) return val_int(0);
    if (!val_is_string(args[1])) return val_int(0);
    HeapString *hs = val_get_string(args[1]);
    if (!buf_ensure(b, hs->len)) return val_int(0);
    memcpy(b->data + b->len, hs->data, (size_t)hs->len);
    b->len += hs->len;
    return val_int(1);
}

static Val buf_write_to(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;
    Buffer *b = buf_get(val_get_int(args[0]));
    if (!b) return val_int(0);
    if (!val_is_string(args[1])) return val_int(0);
    HeapString *path = val_get_string(args[1]);

    FILE *f = fopen(path->data, "wb");
    if (!f) return val_int(0);
    size_t n = fwrite(b->data, 1, (size_t)b->len, f);
    int ok = (fclose(f) == 0) && ((int)n == b->len);
    return val_int(ok ? 1 : 0);
}

static Val buf_length(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;
    Buffer *b = buf_get(val_get_int(args[0]));
    if (!b) return val_int(-1);
    return val_int(b->len);
}

static Val buf_get_byte(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;
    Buffer *b = buf_get(val_get_int(args[0]));
    if (!b) return val_int(-1);
    int64_t i = val_get_int(args[1]);
    if (i < 0 || i >= b->len) return val_int(-1);
    return val_int(b->data[i]);
}

static Val buf_set_byte(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;
    Buffer *b = buf_get(val_get_int(args[0]));
    if (!b) return val_int(0);
    int64_t i = val_get_int(args[1]);
    if (i < 0 || i >= b->len) return val_int(0);
    b->data[i] = (uint8_t)(val_get_int(args[2]) & 0xFF);
    return val_int(1);
}

static Val buf_from_file(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;
    if (!val_is_string(args[0])) return val_int(-1);
    HeapString *path = val_get_string(args[0]);

    FILE *f = fopen(path->data, "rb");
    if (!f) return val_int(-1);
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return val_int(-1); }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return val_int(-1); }
    rewind(f);

    if (next_buffer >= MAX_BUFFERS) { fclose(f); return val_int(-1); }
    int h = next_buffer++;
    Buffer *b = &buffers[h];
    b->cap = (int)sz;
    b->len = 0;
    b->data = malloc((size_t)(sz > 0 ? sz : 1));
    if (!b->data) { fclose(f); return val_int(-1); }
    b->len = (int)fread(b->data, 1, (size_t)sz, f);
    fclose(f);
    return val_int(h);
}

TaFunc buf_funcs[] = {
    {"new",          buf_new,          0},
    {"push_byte",    buf_push_byte,    2},
    {"push_int32",   buf_push_int32,   2},
    {"push_int64",   buf_push_int64,   2},
    {"push_string",  buf_push_string,  2},
    {"write_to",     buf_write_to,     2},
    {"length",       buf_length,       1},
    {"get_byte",     buf_get_byte,     2},
    {"set_byte",     buf_set_byte,     3},
    {"from_file",    buf_from_file,    1},
    {NULL, NULL, 0}
};

void vm_register_buf_module(VM *vm) {
    vm_register_module(vm, "buf", buf_funcs, 10);
}