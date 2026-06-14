#ifndef TA_H
#define TA_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>

/* ============================================================
 * Value representation — NaN-boxing (64-bit)
 * ============================================================ */

typedef uint64_t Val;

/* Extract the 16-bit tag from a NaN-boxed value. */
static inline uint16_t val_tag(Val v) {
    return (uint16_t)(v >> 48);
}

/* NaN-boxing tags: bits [63:48] of the 64-bit value.
 * Normal doubles are stored as-is. Non-double types use the high
 * 16 bits as a tag; the low 48 bits carry payload.              */
#define TAG_INT     0xFF00
#define TAG_NIL     0xFF01
#define TAG_TRUE    0xFF02
#define TAG_FALSE   0xFF03
#define TAG_SYM     0xFF04
#define TAG_PAIR    0xFF05
#define TAG_PID     0xFF06
#define TAG_CLOS    0xFF07
#define TAG_STRING  0xFF08
#define TAG_BYTES   0xFF09
#define TAG_CLOS_ID 0xFF0A  /* direct fn_id (no heap alloc, nfree=0) */

/* Heap object types (stored in HeapHeader.type) */
#define HEAP_PAIR    1
#define HEAP_CLOS    2
#define HEAP_STRING  3
#define HEAP_BYTES   4

#define MAX_PROCS 65536

/* ============================================================
 * Heap object structures
 * ============================================================ */

typedef struct {
    uint8_t type;
    uint8_t flags;
} HeapHeader;

typedef struct {
    HeapHeader hdr;
    Val car, cdr;
} HeapPair;

typedef struct {
    HeapHeader hdr;
    int entry;       /* bytecode offset */
    int nfree;
    Val free[];      /* variable-length: captured values */
} HeapClosure;

typedef struct {
    HeapHeader hdr;
    int len;
    char data[];     /* variable-length, NUL-terminated */
} HeapString;

typedef struct {
    HeapHeader hdr;
    int len;
    uint8_t data[];  /* variable-length */
} HeapBytes;

/* Message fragment (Task 2 will wire this into mailbox) */
typedef struct MsgFragment {
    struct MsgFragment *next;
    int   size;
    Val   root;
    uint8_t data[];
} MsgFragment;

/* ============================================================
 * Process & Scheduler
 * ============================================================ */

typedef struct VM VM;   /* forward declaration for TaFunc */

/* Module function descriptor */
typedef struct {
    const char *name;
    Val  (*fn)(VM *vm, Val *args, int nargs);
    int  nargs;
} TaFunc;

typedef enum { PROC_RUNNING, PROC_WAIT_RECV, PROC_WAIT_IO, PROC_DEAD } ProcState;

typedef struct {
    int catch_pc;
    int sp;
    int fp;
} CatchFrame;

typedef struct Proc {
    int       pid;
    ProcState state;

    /* execution context */
    int       pc;
    int       sp;           /* stack top offset (grows downward from mem end) */
    int       fp;           /* frame pointer */
    int       reductions;

    uint8_t  *code;         /* shared bytecode (read-only) */
    int      *fn_table;     /* shared function table (read-only) */
    int       fn_count;

    /* stack + heap: one contiguous block, growing toward each other
     * low addr → [heap ↑] ... [stack ↓] ← high addr */
    uint8_t  *mem;
    int       mem_size;
    int       heap_ptr;     /* heap top offset (grows upward) */

        /* mailbox (separately allocated, grows on demand) */
    Val      *mbox;
    int       mbox_head, mbox_tail, mbox_count, mbox_cap;
    pthread_mutex_t mbox_lock;

    /* monitor watchers */
    int      *watchers;
    Val      *watcher_refs;
    int       watcher_count, watcher_cap;

        /* error handling (Phase 2) */
    CatchFrame catch_stack[8];
    int        catch_sp;

        /* I/O wait */
    int        wait_fd;
    short      wait_events;  /* POLLIN or POLLOUT */

        /* GC roots (temporary roots for GC during multi-step allocations) */
    Val       gc_roots[16];
    int       gc_root_count;

    /* GC semispace */
    uint8_t  *gc_to;
    int       gc_to_size;
} Proc;

#define MAX_CFUNCS 128

/* Per-thread worker context */
typedef struct {
    VM   *vm;
    Proc *current_proc;
    int   thread_id;
} WorkerCtx;

/* Thread-local current process — set by worker_loop before executing a proc */
extern __thread Proc *tls_current_proc;

struct VM {
    Proc   **procs;
    int      procs_count, procs_cap;

        int     *runq;          /* ready queue (pid array, circular buffer) */
    int      rq_head, rq_tail, rq_cap;
    atomic_int rq_count;

    atomic_int next_pid;
    int      next_ref;      /* monitor ref counter */

        uint8_t *code;          /* shared bytecode */
    int     *fn_table;
    int      fn_count;
    int      top_fn_id;     /* fn_id of top-level thunk */

    /* symbol table (shared, read-only after loading) */
    char   **symbols;
    int      sym_count, sym_cap;

    /* C function registry */
        struct {
        char *name;
        Val  (*fn)(VM *vm, Val *args, int nargs);
        int  nargs;
    } cfuncs[MAX_CFUNCS];
    int cfunc_count;

                    /* yield flag — set by C functions via vm_yield() */
    int      yield_requested;

    /* Module registry */
    TaFunc  **mod_funcs;     /* per-module function arrays */
    int      *mod_nfuncs;    /* per-module function counts */
    char    **mod_names;     /* module names */
        int      mod_count, mod_cap;

    /* Threading infrastructure */
    atomic_int      active_procs;
    pthread_mutex_t rq_lock;
    pthread_cond_t  rq_cond;
    int             nworkers;
    volatile int    stop;
    pthread_t      *workers;
};

/* ============================================================
 * Bytecode instruction set
 * ============================================================ */

typedef enum {
    /* stack */
    OP_PUSH_NIL,
    OP_PUSH_TRUE,
    OP_PUSH_FALSE,
        OP_PUSH_INT8,       /* i8 */
    OP_PUSH_INT,        /* i64 (8 bytes) */
    OP_PUSH_SYM,        /* idx (4 bytes) */
    OP_PUSH_STRING,     /* len(4), data(len) */

    /* local variables */
    OP_LOAD,            /* offset */
    OP_STORE,           /* offset */

    /* pair */
    OP_CONS,
    OP_CAR,
    OP_CDR,

    /* arithmetic */
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,

    /* comparison */
    OP_EQ,
    OP_LT,
    OP_LE,

    /* type tests */
    OP_IS_NIL,
    OP_IS_PAIR,
    OP_IS_INT,
    OP_IS_STRING,
    OP_IS_BYTES,
    OP_IS_PID,

    /* control flow */
    OP_JUMP,            /* addr */
    OP_JUMP_IF_FALSE,   /* addr */
    OP_POP,
    OP_DUP,

    /* functions */
    OP_CLOSURE,         /* fn_id, nfree, [offset...] */
    OP_CALL,            /* nargs */
    OP_TAIL_CALL,       /* nargs */
    OP_RET,

    /* actor */
    OP_SPAWN,           /* fn_id */
    OP_SPAWN_CLOS,
    OP_SEND,
    OP_RECV,
    OP_SELF,
    OP_MONITOR,

    /* built-in */
    OP_PRINT,
    OP_HALT,

    /* pattern matching */
    OP_MATCH_INT,       /* i64 */
    OP_MATCH_SYM,       /* idx */
    OP_MATCH_NIL,
    OP_MATCH_PAIR,      /* binds car & cdr on success */
    OP_MATCH_JUMP,      /* addr — jump on match failure */

        OP_STR_LEN,
    OP_STR_CONCAT,
    OP_STR_SLICE,
    OP_STR_EQ,
    OP_CCALL,           /* cfunc_idx(4 bytes), nargs(1 byte) */

    OP_COUNT
} OpCode;

/* ============================================================
 * C API — lifecycle
 * ============================================================ */

VM     *vm_new(void);
void    vm_free(VM *vm);

/* loading */
void    vm_register(VM *vm, const char *name,
                    Val (*fn)(VM *vm, Val *args, int nargs), int nargs);
void    vm_register_module(VM *vm, const char *name,
                           TaFunc *funcs, int nfuncs);
void    vm_register_net_module(VM *vm);
void    vm_register_http_module(VM *vm);
int     vm_load(VM *vm, const char *src);
int     vm_load_file(VM *vm, const char *path);

/* execution */
int     vm_spawn(VM *vm, int fn_id);
void    vm_run(VM *vm);
int     vm_step(VM *vm, Proc *proc);

/* yield API — lets C functions (e.g. net I/O) suspend the current proc
 * without polluting the value space or intruding into opcode logic. */
void    vm_watch_fd(VM *vm, int fd, short events);
void    vm_yield(VM *vm);

/* REPL */
Val     vm_eval(VM *vm, const char *src);

/* ============================================================
 * C API — value constructors
 * ============================================================ */

Val     val_int(int64_t i);
Val     val_nil(void);
Val     val_true(void);
Val     val_false(void);
Val     val_symbol(uint32_t idx);
int     vm_intern_symbol(VM *vm, const char *name);
Val     val_pid(uint32_t pid);

/* heap-allocated constructors (require process context) */
Val     val_pair(Proc *p, Val car, Val cdr);
Val     val_string(Proc *p, const char *data, int len);
Val     val_bytes(Proc *p, const uint8_t *data, int len);

/* ============================================================
 * C API — value predicates & accessors
 * ============================================================ */

int     val_is_int(Val v);
int64_t val_get_int(Val v);

int     val_is_nil(Val v);
int     val_is_true(Val v);     /* not nil and not false */

int     val_is_pair(Val v);
Val     val_get_car(Val v);
Val     val_get_cdr(Val v);

int     val_is_symbol(Val v);
uint32_t val_get_symbol(Val v);

int     val_is_pid(Val v);
uint32_t val_get_pid(Val v);

int     val_is_clos(Val v);
int     val_is_pid_type(Val v);

int     val_is_string(Val v);
HeapString *val_get_string(Val v);

int     val_is_bytes(Val v);
HeapBytes  *val_get_bytes(Val v);

/* ============================================================
 * Deep copy
 * ============================================================ */

Val     val_deep_copy(Proc *target, Val v);

/* ============================================================
 * Garbage collection
 * ============================================================ */

void    gc_collect(Proc *p);

static inline void gc_root_push(Proc *p, Val v) {
    if (p->gc_root_count < 16) p->gc_roots[p->gc_root_count++] = v;
}
static inline Val gc_root_pop(Proc *p) {
    return p->gc_roots[--p->gc_root_count];
}

/* ============================================================
 * Inline helpers — stack access
 * ============================================================ */

static inline Val *proc_stack(Proc *p) {
    return (Val *)(p->mem + p->mem_size);
}

static inline void proc_push(Proc *p, Val v) {
    p->sp--;
    *(Val *)(p->mem + p->mem_size + p->sp * sizeof(Val)) = v;
}

static inline Val proc_pop(Proc *p) {
    Val v = *(Val *)(p->mem + p->mem_size + p->sp * sizeof(Val));
    p->sp++;
    return v;
}

static inline Val proc_peek(Proc *p, int offset) {
    return *(Val *)(p->mem + p->mem_size + (p->sp + offset) * sizeof(Val));
}

/* ============================================================
 * Heap allocation helpers
 * ============================================================ */

/* Allocate `size` bytes on the process heap. Returns NULL if OOM. */
static inline int proc_grow(Proc *p); /* forward declaration */
static inline void *proc_heap_alloc(Proc *p, int size) {
    /* Align to 8 bytes */
    size = (size + 7) & ~7;
    if (p->heap_ptr + size > p->mem_size + p->sp * (int)sizeof(Val)) {
        /* heap-stack collision — trigger GC and retry */
        gc_collect(p);
        if (p->heap_ptr + size > p->mem_size + p->sp * (int)sizeof(Val)) {
            /* GC didn't free enough — try to grow */
            if (proc_grow(p) != 0) return NULL;
            if (p->heap_ptr + size > p->mem_size + p->sp * (int)sizeof(Val)) {
                return NULL; /* still OOM after grow */
            }
        }
    }
    void *ptr = p->mem + p->heap_ptr;
    p->heap_ptr += size;
    memset(ptr, 0, size);
    return ptr;
}

static inline int proc_grow(Proc *p) {
    int new_size = p->mem_size * 2;
    uint8_t *new_gc = realloc(p->gc_to, new_size);
    if (!new_gc) return -1;
    uint8_t *new_mem = realloc(p->mem, new_size);
    if (!new_mem) {
        p->gc_to = new_gc; /* keep grown gc_to */
        return -1;
    }
    p->mem = new_mem;
    p->gc_to = new_gc;
    p->mem_size = new_size;
    memset(p->gc_to, 0, new_size);
    return 0;
}

/* Convenience: get HeapPair* from a TAG_PAIR Val */
static inline HeapPair *val_as_pair(Val v) {
    return (HeapPair *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

/* Get HeapClosure* from a TAG_CLOS Val */
static inline HeapClosure *val_as_clos(Val v) {
    return (HeapClosure *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

#endif /* TA_H */

