/*
 * prof.c — sampling profiler for TinyActor (--profile flag).
 *
 * Linux requires _POSIX_C_SOURCE for clock_gettime/CLOCK_MONOTONIC/strdup
 * under -std=c99; macOS exposes them by default, which is why the build
 * only breaks on Linux. Must come before any #include.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * Design: deterministic reduction-boundary sampling, not a signal/profiling
 * timer. The worker loop records the wall-clock time (CLOCK_MONOTONIC) of
 * each 64-instruction block and attributes that whole block's duration to
 * the TA call stack observed at the block's end boundary.
 *
 * Why this beats SIGPROF/SETITIMER:
 *   - Race-free: the sample is taken on the worker thread itself, between
 *     vm_step calls, when the Proc is fully consistent (pc at an instruction
 *     boundary, mem stable — no realloc in flight). No cross-thread reads of
 *     proc memory, so proc_die's free(p->mem) can't be raced.
 *   - C calls are captured: a long str/buf/net builtin call shows up as time
 *     attributed to the TA function that made the call (the boundary after
 *     the C call lands in the caller's frame). SIGPROF would show it as a
 *     mystery C-frame instead.
 *   - Portable and deterministic (no async-signal-safety concerns).
 *
 * Output (written by prof_finish):
 *   <base>.json    — Speedscope format: open in https://www.speedscope.app
 *   <base>.folded  — Brendan Gregg folded stacks: flamegraph.pl compatible
 *   stderr summary — total time, samples, top-10 hot functions
 *
 * vm->prof_on is set once in prof_init (before vm_run starts workers) and
 * cleared in prof_finish; worker_loop hoists it into a local, so the hot
 * path pays ~zero when profiling is off.
 */

#include "ta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PROF_MAX_DEPTH 64 /* call-stack frames captured per sample */
#define PROF_HT_INIT_CAP 1024
#define PROF_ENTRY_INIT_CAP 64

typedef struct {
    int *frames; /* fn ids, root..leaf (outermost first) */
    int depth;
    uint64_t weight_ns; /* accumulated block time for this stack */
    uint64_t samples;   /* number of blocks attributed */
} ProfEntry;

typedef struct {
    uint64_t hash;
    int entry; /* index into ProfState.entries; -1 = empty */
} ProfSlot;

typedef struct ProfState {
    pthread_mutex_t lock; /* protects entries/slots (multi-worker) */
    ProfEntry *entries;
    int entry_count, entry_cap;
    ProfSlot *slots;
    int slot_cap, slot_used;
    char *out_base; /* output file base: "<base>.json", "<base>.folded" */
    uint64_t total_ns;
    uint64_t total_samples;
    uint64_t wall_start_ns; /* prof_init timestamp — for coverage context */
} ProfState;

/* ------------------------------------------------------------------ */
/* time                                                               */
/* ------------------------------------------------------------------ */

uint64_t prof_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
/* stack walking                                                      */
/* ------------------------------------------------------------------ */

/* pc -> owning fn_id via binary search over fn_table (sorted offsets). */
static int prof_fn_of_pc(const Proc *p, int pc) {
    int lo = 0, hi = p->fn_count - 1, ans = 0;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (p->fn_table[mid] <= pc) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return ans;
}

/* Walk the TA call stack. Frame layout (see OP_CALL in vm.c):
 *   st[fp+0..]     args + locals + temporaries
 *   st[fp-1]       closure
 *   st[fp-2]       ret_pc  (-1 sentinel in the root frame)
 *   st[fp-3]       old_fp  (caller's fp; fp grows *more negative* with
 *                           depth, so a caller always has old_fp > fp)
 *   st[fp-4]       caller_sp
 * Returns depth; out[] filled leaf..root (current fn first, outermost last). */
static int prof_walk_stack(const VM *vm, const Proc *p, int *out, int max_depth) {
    int depth = 0;
    int fp = p->fp;
    int mem_words = p->mem_size / (int)sizeof(Val);
    const Val *st = (const Val *)(p->mem + p->mem_size);

    out[depth++] = prof_fn_of_pc(p, p->pc);
    while (depth < max_depth) {
        /* frame header must be within the stack */
        if (fp - 4 < -mem_words)
            break;
        Val rv = st[fp - 2];
        Val of = st[fp - 3];
        if (!val_is_int(rv) || !val_is_int(of))
            break;
        int ret_pc = (int)val_get_int(rv);
        int old_fp = (int)val_get_int(of);
        if (ret_pc < 0) /* root frame sentinel */
            break;
        if (ret_pc >= vm->code_len)
            break;
        if (old_fp <= fp) /* caller fp must be greater */
            break;
        out[depth++] = prof_fn_of_pc(p, ret_pc);
        fp = old_fp;
    }
    return depth;
}

/* ------------------------------------------------------------------ */
/* hash map: frames[] -> ProfEntry                                    */
/* ------------------------------------------------------------------ */

static uint64_t prof_hash_frames(const int *frames, int depth) {
    uint64_t h = 1469598103934665603ULL; /* FNV-1a offset basis */
    for (int i = 0; i < depth; i++) {
        h ^= (uint32_t)frames[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void prof_rehash(ProfState *ps) {
    int newcap = ps->slot_cap * 2;
    ProfSlot *ns = calloc((size_t)newcap, sizeof(ProfSlot));
    if (!ns)
        return; /* keep old table; still correct, just slower */
    for (int i = 0; i < newcap; i++)
        ns[i].entry = -1;
    for (int i = 0; i < ps->slot_cap; i++) {
        if (ps->slots[i].entry < 0)
            continue;
        ProfEntry *e = &ps->entries[ps->slots[i].entry];
        uint64_t h = prof_hash_frames(e->frames, e->depth);
        int j = (int)(h & (newcap - 1));
        while (ns[j].entry >= 0)
            j = (j + 1) & (newcap - 1);
        ns[j] = ps->slots[i];
    }
    free(ps->slots);
    ps->slots = ns;
    ps->slot_cap = newcap;
}

static ProfEntry *prof_find_or_add(ProfState *ps, const int *frames, int depth, uint64_t hash) {
    if (ps->slot_cap == 0) {
        ps->slot_cap = PROF_HT_INIT_CAP;
        ps->slots = malloc((size_t)ps->slot_cap * sizeof(ProfSlot));
        if (!ps->slots) {
            ps->slot_cap = 0;
            return NULL;
        }
        for (int i = 0; i < ps->slot_cap; i++)
            ps->slots[i].entry = -1;
    }
    if ((ps->slot_used + 1) * 10 >= ps->slot_cap * 7)
        prof_rehash(ps);

    int mask = ps->slot_cap - 1;
    int j = (int)(hash & mask);
    while (ps->slots[j].entry >= 0) {
        ProfEntry *e = &ps->entries[ps->slots[j].entry];
        if (e->depth == depth && memcmp(e->frames, frames, (size_t)depth * sizeof(int)) == 0)
            return e;
        j = (j + 1) & mask;
    }
    /* new entry */
    if (ps->entry_count == ps->entry_cap) {
        int newcap = ps->entry_cap ? ps->entry_cap * 2 : PROF_ENTRY_INIT_CAP;
        ProfEntry *ne = realloc(ps->entries, (size_t)newcap * sizeof(ProfEntry));
        if (!ne)
            return NULL;
        ps->entries = ne;
        ps->entry_cap = newcap;
    }
    ProfEntry *e = &ps->entries[ps->entry_count];
    e->frames = malloc((size_t)depth * sizeof(int));
    if (!e->frames)
        return NULL;
    memcpy(e->frames, frames, (size_t)depth * sizeof(int));
    e->depth = depth;
    e->weight_ns = 0;
    e->samples = 0;
    ps->slots[j].hash = hash;
    ps->slots[j].entry = ps->entry_count++;
    ps->slot_used++;
    return e;
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

void prof_init(VM *vm, const char *out_path) {
    if (vm->prof)
        return;
    ProfState *ps = calloc(1, sizeof(ProfState));
    if (!ps)
        return;
    pthread_mutex_init(&ps->lock, NULL);
    ps->out_base = strdup(out_path ? out_path : "profile");
    if (!ps->out_base) {
        pthread_mutex_destroy(&ps->lock);
        free(ps);
        return;
    }
    vm->prof = ps;
    vm->prof_on = 1;
    ps->wall_start_ns = prof_now_ns();
}

void prof_collect(VM *vm, Proc *p, uint64_t dt_ns) {
    ProfState *ps = vm->prof;
    if (!ps || dt_ns == 0 || p->mem == NULL)
        return;

    int frames[PROF_MAX_DEPTH];
    int depth = prof_walk_stack(vm, p, frames, PROF_MAX_DEPTH);
    if (depth <= 0)
        return;
    uint64_t h = prof_hash_frames(frames, depth);

    pthread_mutex_lock(&ps->lock);
    ProfEntry *e = prof_find_or_add(ps, frames, depth, h);
    if (e) {
        e->weight_ns += dt_ns;
        e->samples++;
        ps->total_ns += dt_ns;
        ps->total_samples++;
    }
    pthread_mutex_unlock(&ps->lock);
}

/* ------------------------------------------------------------------ */
/* output dump                                                        */
/* ------------------------------------------------------------------ */

static const char *prof_fn_name(const VM *vm, int fid) {
    if (vm->fn_names && fid >= 0 && fid < vm->fn_names_count && vm->fn_names[fid])
        return vm->fn_names[fid];
    return NULL;
}

/* Intern a frame name into the speedscope frames table; returns its index. */
static int prof_intern_frame(const VM *vm, const char ***names, int *n, int *cap, int fid) {
    const char *name = prof_fn_name(vm, fid);
    char fallback[32];
    if (!name) {
        snprintf(fallback, sizeof(fallback), "fn#%d", fid);
        name = fallback;
    }
    for (int i = 0; i < *n; i++) {
        if (strcmp((*names)[i], name) == 0)
            return i;
    }
    if (*n == *cap) {
        int newcap = *cap ? *cap * 2 : 64;
        const char **nn = realloc((void *)*names, (size_t)newcap * sizeof(char *));
        if (!nn)
            return *n > 0 ? *n - 1 : 0;
        *names = nn;
        *cap = newcap;
    }
    (*names)[*n] = strdup(name);
    return (*n)++;
}

static int cmp_entry_desc(const void *a, const void *b) {
    const ProfEntry *ea = a, *eb = b;
    if (ea->weight_ns < eb->weight_ns)
        return 1;
    if (ea->weight_ns > eb->weight_ns)
        return -1;
    return 0;
}

static void prof_write_json(FILE *f, const VM *vm, const ProfState *ps) {
    /* pass 1: intern unique frame names */
    const char **names = NULL;
    int nnames = 0, ncap = 0;
    for (int i = 0; i < ps->entry_count; i++) {
        const ProfEntry *e = &ps->entries[i];
        for (int d = 0; d < e->depth; d++)
            prof_intern_frame(vm, &names, &nnames, &ncap, e->frames[d]);
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"$schema\": \"https://www.speedscope.app/file-format-schema.json\",\n");
    fprintf(f, "  \"shared\": {\n");
    fprintf(f, "    \"frames\": [\n");
    for (int i = 0; i < nnames; i++) {
        fprintf(f, "      {\"name\": \"");
        for (const char *c = names[i]; *c; c++) {
            if (*c == '"' || *c == '\\')
                fputc('\\', f);
            fputc(*c, f);
        }
        fprintf(f, "\"}%s\n", i + 1 < nnames ? "," : "");
    }
    fprintf(f, "    ]\n");
    fprintf(f, "  },\n");
    fprintf(f, "  \"profiles\": [\n");
    fprintf(f, "    {\n");
    fprintf(f, "      \"type\": \"sampled\",\n");
    fprintf(f, "      \"name\": \"tinyactor\",\n");
    fprintf(f, "      \"unit\": \"microseconds\",\n");
    fprintf(f, "      \"startValue\": 0,\n");
    fprintf(f, "      \"endValue\": %llu,\n", (unsigned long long)(ps->total_ns / 1000));
    fprintf(f, "      \"samples\": [\n");
    for (int i = 0; i < ps->entry_count; i++) {
        const ProfEntry *e = &ps->entries[i];
        fprintf(f, "        [");
        /* frames[] is leaf..root; speedscope wants root..leaf */
        for (int d = e->depth - 1; d >= 0; d--) {
            int fid = e->frames[d];
            const char *name = prof_fn_name(vm, fid);
            char fallback[32];
            if (!name) {
                snprintf(fallback, sizeof(fallback), "fn#%d", fid);
                name = fallback;
            }
            int idx = -1;
            for (int k = 0; k < nnames; k++) {
                if (strcmp(names[k], name) == 0) {
                    idx = k;
                    break;
                }
            }
            fprintf(f, "%s%d%s", d == e->depth - 1 ? "" : ",", idx, d > 0 ? " " : "");
        }
        fprintf(f, "]%s\n", i + 1 < ps->entry_count ? "," : "");
    }
    fprintf(f, "      ],\n");
    fprintf(f, "      \"weights\": [\n");
    for (int i = 0; i < ps->entry_count; i++) {
        fprintf(f, "        %llu%s\n", (unsigned long long)(ps->entries[i].weight_ns / 1000),
                i + 1 < ps->entry_count ? "," : "");
    }
    fprintf(f, "      ]\n");
    fprintf(f, "    }\n");
    fprintf(f, "  ],\n");
    fprintf(f, "  \"activeProfileIndex\": 0\n");
    fprintf(f, "}\n");
}

static void prof_write_folded(FILE *f, const VM *vm, const ProfState *ps) {
    for (int i = 0; i < ps->entry_count; i++) {
        const ProfEntry *e = &ps->entries[i];
        /* frames[] is leaf..root; folded format wants root..leaf */
        for (int d = e->depth - 1; d >= 0; d--) {
            const char *name = prof_fn_name(vm, e->frames[d]);
            if (!name)
                fprintf(f, "fn#%d", e->frames[d]);
            else
                fprintf(f, "%s", name);
            if (d > 0)
                fputc(';', f);
        }
        fprintf(f, " %llu\n", (unsigned long long)(e->weight_ns / 1000));
    }
}

static void prof_print_summary(const VM *vm, const ProfState *ps) {
    if (ps->total_ns == 0) {
        fprintf(stderr, "profiler: no samples collected\n");
        return;
    }
    double total_ms = (double)ps->total_ns / 1e6;
    uint64_t wall_ns = prof_now_ns() - ps->wall_start_ns;
    fprintf(stderr,
            "profiler: %llu samples, %.1f ms TA execution time "
            "(%.1f ms wall; sampling covers vm_run only, not .tabc load)\n",
            (unsigned long long)ps->total_samples, total_ms, (double)wall_ns / 1e6);

    /* aggregate per-fn_id SELF time (leaf frames only) across all stacks —
     * standard profiler semantics: never exceeds 100%. Inclusive view is
     * in the flame graph itself. */
    uint64_t *fn_total = calloc((size_t)vm->fn_count, sizeof(uint64_t));
    int *fn_idx = malloc((size_t)vm->fn_count * sizeof(int));
    if (!fn_total || !fn_idx) {
        free(fn_total);
        free(fn_idx);
        return;
    }
    for (int i = 0; i < vm->fn_count; i++)
        fn_idx[i] = i;
    for (int i = 0; i < ps->entry_count; i++) {
        const ProfEntry *e = &ps->entries[i];
        int fid = e->frames[0]; /* leaf = innermost (frames[] is leaf..root) */
        if (fid >= 0 && fid < vm->fn_count)
            fn_total[fid] += e->weight_ns;
    }
    /* sort fn ids by total weight desc */
    for (int i = 0; i < vm->fn_count; i++) {
        for (int j = i + 1; j < vm->fn_count; j++) {
            if (fn_total[fn_idx[j]] > fn_total[fn_idx[i]]) {
                int t = fn_idx[i];
                fn_idx[i] = fn_idx[j];
                fn_idx[j] = t;
            }
        }
    }
    int shown = vm->fn_count < 10 ? vm->fn_count : 10;
    fprintf(stderr, "top-%d by self time:\n", shown);
    for (int i = 0; i < shown; i++) {
        int fid = fn_idx[i];
        if (fn_total[fid] == 0)
            break;
        const char *name = prof_fn_name(vm, fid);
        fprintf(stderr, "  %5.1f%%  %9.1fms  %s\n",
                100.0 * (double)fn_total[fid] / (double)ps->total_ns, (double)fn_total[fid] / 1e6,
                name ? name : "?");
    }
    free(fn_total);
    free(fn_idx);
}

void prof_finish(VM *vm) {
    if (!vm->prof)
        return;
    ProfState *ps = vm->prof;
    vm->prof_on = 0;
    pthread_mutex_lock(&ps->lock);

    qsort(ps->entries, (size_t)ps->entry_count, sizeof(ProfEntry), cmp_entry_desc);

    char *json_path = malloc(strlen(ps->out_base) + 6);
    char *folded_path = malloc(strlen(ps->out_base) + 9);
    if (json_path && folded_path) {
        sprintf(json_path, "%s.json", ps->out_base);
        sprintf(folded_path, "%s.folded", ps->out_base);
        FILE *fj = fopen(json_path, "w");
        if (fj) {
            prof_write_json(fj, vm, ps);
            fclose(fj);
        } else {
            fprintf(stderr, "profiler: cannot write %s\n", json_path);
        }
        FILE *ff = fopen(folded_path, "w");
        if (ff) {
            prof_write_folded(ff, vm, ps);
            fclose(ff);
        } else {
            fprintf(stderr, "profiler: cannot write %s\n", folded_path);
        }
    }
    free(json_path);
    free(folded_path);

    prof_print_summary(vm, ps);
    pthread_mutex_unlock(&ps->lock);

    for (int i = 0; i < ps->entry_count; i++)
        free(ps->entries[i].frames);
    free(ps->entries);
    free(ps->slots);
    free(ps->out_base);
    pthread_mutex_destroy(&ps->lock);
    free(ps);
    vm->prof = NULL;
}