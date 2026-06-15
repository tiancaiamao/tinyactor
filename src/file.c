/*
 * file.c — File I/O module for TinyActor VM
 *
 *   file.read(path)    -> String | nil   (whole file as string)
 *   file.write(path, data) -> Int        (1 ok, 0 error; overwrite)
 *   file.exists(path)  -> Int            (1 exists, 0 missing)
 *
 * Uses standard C stdio. HeapString data is NUL-terminated (see
 * val_string in val.c), so hs->data is safe to pass to fopen/access.
 */

#include "ta.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static Val file_read(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;
    Proc *p = tls_current_proc;

    if (!val_is_string(args[0])) return val_nil();
    HeapString *hs = val_get_string(args[0]);

    FILE *f = fopen(hs->data, "rb");
    if (!f) return val_nil();

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return val_nil(); }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return val_nil(); }
    rewind(f);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return val_nil(); }

    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    Val result = val_string(p, buf, (int)got);
    free(buf);
    return result;
}

static Val file_write(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;

    if (!val_is_string(args[0]) || !val_is_string(args[1]))
        return val_int(0);

    HeapString *path = val_get_string(args[0]);
    HeapString *data = val_get_string(args[1]);

    FILE *f = fopen(path->data, "wb");
    if (!f) return val_int(0);

    size_t n = fwrite(data->data, 1, (size_t)data->len, f);
    int ok = (fclose(f) == 0) && ((int)n == data->len);
    return val_int(ok ? 1 : 0);
}

static Val file_exists(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;

    if (!val_is_string(args[0])) return val_int(0);
    HeapString *hs = val_get_string(args[0]);
    return val_int(access(hs->data, F_OK) == 0 ? 1 : 0);
}

TaFunc file_funcs[] = {
    {"read",   file_read,   1},
    {"write",  file_write,  2},
    {"exists", file_exists, 1},
    {NULL, NULL, 0}
};

void vm_register_file_module(VM *vm) {
    vm_register_module(vm, "file", file_funcs, 3);
}