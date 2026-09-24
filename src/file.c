/*
 * file.c — File I/O module for TinyActor VM
 *
 *   file.read(path)       -> String | nil   (whole file as string)
 *   file.write(path, data) -> Int           (1 ok, 0 error; overwrite)
 *   file.exists(path)      -> Int           (1 exists, 0 missing)
 *   file.mkdir_p(path)     -> Int           (1 ok, 0 error, -1 invalid arg;
 *                                            recursively create dirs)
 *   file.list_dir(path)    -> list of String | -1 (directory entries; "."
 *                                            and ".." skipped)
 *   file.remove(path)      -> Int           (1 ok, -1 error; file or empty dir)
 *   file.rename(old, new)  -> Int           (1 ok, -1 error)
 *   file.cwd()             -> String | -1   (current working directory)
 *   file.stat(path)        -> list [size, mtime] | -1 (bytes, epoch seconds)
 *
 * Status convention: file.write/file.mkdir_p keep their historical 1/0/-1
 * status. The newer filesystem functions follow the unified signal
 * vocabulary (docs/c-module.md): -1 is the single hard-error signal
 * (syscall errno or bad argument), lifted to Result by lib/fs.ta.
 * file.stat returns a fixed-shape 2-element list [size_int, mtime_int]
 * instead of a dict because dicts land in a later batch; positional
 * access (car / car(cdr)) keeps it allocation-cheap and unambiguous.
 *
 * Uses standard C stdio. HeapString data is NUL-terminated (see
 * val_string in val.c), so hs->data is safe to pass to fopen/access.
 */

#define _DEFAULT_SOURCE /* expose POSIX strdup/mkdir/stat under -std=c99 */

#include "ta.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static Val file_read(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;

    if (!val_is_string(args[0]))
        return val_nil();
    HeapString *hs = val_get_string(args[0]);

    FILE *f = fopen(hs->data, "rb");
    if (!f)
        return val_nil();

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return val_nil();
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return val_nil();
    }
    rewind(f);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return val_nil();
    }

    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    Val result = val_string(p, buf, (int)got);
    free(buf);
    return result;
}

static Val file_write(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;

    if (!val_is_string(args[0]) || !val_is_string(args[1]))
        return val_int(-1);

    HeapString *path = val_get_string(args[0]);
    HeapString *data = val_get_string(args[1]);

    FILE *f = fopen(path->data, "wb");
    if (!f)
        return val_int(-1);

    size_t n = fwrite(data->data, 1, (size_t)data->len, f);
    int ok = (fclose(f) == 0) && ((int)n == data->len);
    return val_int(ok ? 1 : 0);
}

static Val file_exists(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;

    if (!val_is_string(args[0]))
        return val_int(0);
    HeapString *hs = val_get_string(args[0]);
    return val_int(access(hs->data, F_OK) == 0 ? 1 : 0);
}

/* make_dir(path): create a single directory component. Returns 1 when the
 * directory exists afterwards (freshly created or already present), 0 on
 * any real failure — including when the path already exists as a regular
 * file (EEXIST but not a directory). */
static int make_dir(const char *path) {
    if (mkdir(path, 0755) == 0)
        return 1;
    if (errno != EEXIST)
        return 0;
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

static Val file_mkdir_p(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;

    if (!val_is_string(args[0]))
        return val_int(-1);
    HeapString *hs = val_get_string(args[0]);
    if (hs->len == 0)
        return val_int(0);

    /* Duplicate the path so components can be split in place. */
    char *path = strdup(hs->data);
    if (!path)
        return val_int(0);

    int ok = 1;
    /* Create every intermediate directory along the way (mkdir -p style).
     * Skipping a leading '/' keeps absolute paths rooted correctly. */
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (!make_dir(path)) {
                ok = 0;
                break;
            }
            *p = '/';
        }
    }
    /* Create the final component, tolerating trailing slashes. */
    if (ok) {
        char *end = path + strlen(path);
        while (end > path + 1 && end[-1] == '/')
            end--;
        *end = '\0';
        if (!make_dir(path))
            ok = 0;
    }

    free(path);
    return val_int(ok ? 1 : 0);
}

/* Directory entries as a TA list of strings; "." and ".." are skipped.
 * Entries arrive in readdir order (unsorted) — lib/fs.ta / list.sort own
 * ordering. -1 = hard error (not a string, opendir failed). */
static Val file_list_dir(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_string(args[0]))
        return val_int(-1);
    HeapString *hs = val_get_string(args[0]);

    DIR *d = opendir(hs->data);
    if (!d)
        return val_int(-1);

    Proc *p = tls_current_proc;
    Val list = val_nil();
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        list = val_pair(p, val_string(p, ent->d_name, (int)strlen(ent->d_name)), list);
    }
    closedir(d);
    return list;
}

/* remove(2) deletes a file or an empty directory. 1 ok, -1 hard error. */
static Val file_remove(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;

    if (!val_is_string(args[0]))
        return val_int(-1);
    HeapString *hs = val_get_string(args[0]);
    return val_int(remove(hs->data) == 0 ? 1 : -1);
}

static Val file_rename(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;

    if (!val_is_string(args[0]) || !val_is_string(args[1]))
        return val_int(-1);
    HeapString *old = val_get_string(args[0]);
    HeapString *new = val_get_string(args[1]);
    return val_int(rename(old->data, new->data) == 0 ? 1 : -1);
}

static Val file_cwd(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;

    char buf[4096];
    if (!getcwd(buf, sizeof(buf)))
        return val_int(-1);
    return val_string(tls_current_proc, buf, (int)strlen(buf));
}

/* stat(2) as [size_bytes, mtime_epoch_seconds] — the fixed-shape list
 * documented in the module header. -1 = hard error. */
static Val file_stat(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;

    if (!val_is_string(args[0]))
        return val_int(-1);
    HeapString *hs = val_get_string(args[0]);
    struct stat st;
    if (stat(hs->data, &st) != 0)
        return val_int(-1);

    Proc *p = tls_current_proc;
    return val_pair(p, val_int((long)st.st_size),
                    val_pair(p, val_int((long)st.st_mtime), val_nil()));
}

TaFunc file_funcs[] = {{"read", file_read, 1},         {"write", file_write, 2},
                       {"exists", file_exists, 1},     {"mkdir_p", file_mkdir_p, 1},
                       {"list_dir", file_list_dir, 1}, {"remove", file_remove, 1},
                       {"rename", file_rename, 2},     {"cwd", file_cwd, 0},
                       {"stat", file_stat, 1},         {NULL, NULL, 0}};

void vm_register_file_module(VM *vm) { vm_register_module(vm, "file", file_funcs, 9); }