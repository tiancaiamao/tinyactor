#ifndef TA_H
#define TA_H

#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * Value representation — NaN-boxing (64-bit)
 * ============================================================ */

typedef uint64_t Val;

/* Extract the 16-bit tag from a NaN-boxed value — see ta_inline.h */

/* NaN-boxing tags: bits [63:48] of the 64-bit value.
 * Normal doubles are stored as-is. Non-double types use the high
 * 16 bits as a tag; the low 48 bits carry payload.              */
#define TAG_INT 0xFF00
#define TAG_NIL 0xFF01
#define TAG_TRUE 0xFF02
#define TAG_FALSE 0xFF03
#define TAG_SYM 0xFF04
#define TAG_PAIR 0xFF05
#define TAG_PID 0xFF06
#define TAG_CLOS 0xFF07
#define TAG_STRING 0xFF08
#define TAG_BYTES 0xFF09
#define TAG_CLOS_ID 0xFF0A /* direct fn_id (no heap alloc, nfree=0) */

/* Heap object types (stored in HeapHeader.type) */
#define HEAP_PAIR 1
#define HEAP_CLOS 2
#define HEAP_STRING 3
#define HEAP_BYTES 4

/* HeapHeader.flags bit: set while a GC copy has relocated the object; the
 * slot right after the header then holds the new address (gc_copy_obj,
 * val_converge_copy). Only observed inside a collection — fromspace and
 * chunk objects are dead afterwards. */
#define FLAG_FORWARDED 0x01

/* ---- Actor memory block layout (tuning knobs) ----
 *
 * One contiguous block per actor:
 *
 *     low addr → [ TA_PROC_CHUNK0 | heap ↑ ... ↓ stack ] ← high addr
 *
 * TA_PROC_CHUNK0 reserves a slice at the bottom of every block for the
 * C-callback chunk arena (gc issue #160): while a C module runs, its
 * allocations bump-allocate here instead of the managed heap, so the
 * managed heap stays frozen and nothing needs rooting. Overflow past the
 * slice falls back to malloc'd chunks. The region is otherwise untouched,
 * so a small default is free — tune by observing how often the overflow
 * path fires. */
#ifndef TA_PROC_CHUNK0
#define TA_PROC_CHUNK0 256
#endif
/* Usable heap+stack (excluding the chunk slice) an actor grows to when its
 * first heap object is allocated. Actors that never heap-allocate stay at
 * the 512-byte idling buffer. */
#ifndef TA_PROC_HEAP0
#define TA_PROC_HEAP0 2048
#endif

#define MAX_PROCS (1024 * 1024)

/* Proc.recv_deadline_ms sentinel: the deadline fired while the proc was
 * blocked in recv_after (BUILTIN_RECV_AFTER) — the timeout won, so the
 * builtin must return nil and leave the mailbox untouched (messages arriving
 * after expiry belong to the next receive). */
#define RECV_AFTER_EXPIRED (-2)

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
    int entry; /* bytecode offset */
    int nfree;
    Val free[]; /* variable-length: captured values */
} HeapClosure;

typedef struct {
    HeapHeader hdr;
    int len;
    char data[]; /* variable-length, NUL-terminated */
} HeapString;

typedef struct {
    HeapHeader hdr;
    int len;
    uint8_t data[]; /* variable-length */
} HeapBytes;

/* Message fragment (Task 2 will wire this into mailbox) */
typedef struct MsgFragment {
    struct MsgFragment *next;
    int size;
    Val root;
    uint8_t data[];
} MsgFragment;

/* ============================================================
 * Process & Scheduler
 * ============================================================ */

typedef struct VM VM; /* forward declaration for TaFunc */

/* Module function descriptor */
typedef struct {
    const char *name;
    Val (*fn)(VM *vm, Val *args, int nargs);
    int nargs;
} TaFunc;

typedef enum { PROC_RUNNING, PROC_WAIT_RECV, PROC_WAIT_IO, PROC_DEAD } ProcState;

typedef struct {
    int catch_pc;
    int sp;
    int fp;
} CatchFrame;

#define MAX_TOK_VECS 32

/* One malloc'd chunk of the gate-closed callback arena (see Proc.gc_ck_head).
 * The first chunk is not malloc'd — it is the TA_PROC_CHUNK0 slice at the
 * bottom of the proc's `mem` block, and its data pointer is always derived
 * from p->mem so it survives arena growth/swaps. */
typedef struct TaChunk {
    struct TaChunk *next;
    int used;
    int cap;
} TaChunk;

typedef struct Proc {
    int pid;
    atomic_int state;

    /* execution context */
    int pc;
    int sp; /* stack top offset (grows downward from mem end) */
    int fp; /* frame pointer */
    int reductions;
    int yield_requested; /* set by C functions via vm_yield(); checked by OP_CCALL_NAME */
    int die_requested;   /* set by C functions via vm_die(); checked by OP_CCALL_NAME */
    Val die_reason;      /* crash reason symbol (immediate value, GC-invisible) */

    uint8_t *code; /* shared bytecode (read-only) */
    int *fn_table; /* shared function table (read-only) */
    int fn_count;

    /* stack + heap: one contiguous block, growing toward each other
     * low addr → [chunk0][heap ↑] ... [stack ↓] ← high addr */
    uint8_t *mem;
    int mem_size;
    int heap_ptr; /* heap top offset (grows upward; starts at TA_PROC_CHUNK0) */

    /* mailbox — heap-fragment message queue (thread-safe).
     * Messages live in malloc'd MsgFragment nodes OUTSIDE the process
     * heap, so send/recv never touch the target heap (GC-race free).
     * gc.c's mailbox scan is a no-op: fragments are malloc'd, hence
     * outside fromspace, so gc_copy_val() returns early. */
    MsgFragment *mbox_frag_head; /* fragment list head */
    MsgFragment *mbox_frag_tail; /* fragment list tail */
    int mbox_count;              /* number of queued messages */
    pthread_mutex_t mbox_lock;

    /* selective-receive scan cursor: index of the next mailbox fragment
     * to try during an in-progress (receive ...). Reset to 0 by
     * recv_commit (matched) — preserved across a block so resumed
     * scans only inspect newly-arrived messages. */
    int peek_index;

    /* monitor watchers */
    int *watchers;
    Val *watcher_refs;
    int watcher_count, watcher_cap;

    /* token-vector registry (vm.make_tok_vec / vm.tok_type / vm.tok_val /
     * vm.free_tok_vec). Per-proc since #121: the old process-wide table in
     * api.c was not thread-safe and leaked on proc death. Ids are opaque
     * to TA and only meaningful within the owning proc. Elements are
     * TokVec* (defined in api.c), stored as void*. */
    void *tok_vecs[MAX_TOK_VECS];

    /* error handling (Phase 2) */
    CatchFrame catch_stack[8];
    int catch_sp;

    /* I/O wait */
    int wait_fd;
    short wait_events;                    /* POLLIN or POLLOUT */
    atomic_int_fast64_t wait_deadline_ms; /* monotonic-ms deadline (-1 = none); the I/O
                                             poller wakes the proc once it passes, so
                                             net_connect timeouts fire even when the
                                             socket never becomes ready */
    atomic_int_fast64_t recv_deadline_ms; /* recv_after(ms): monotonic-ms deadline.
                                             < -1 (RECV_AFTER_EXPIRED): deadline fired
                                             while blocked — opcode returns nil without
                                             touching the mailbox; -1: disarmed (ms
                                             operand still on stack); >= 0: armed —
                                             scheduler wakes the proc once it passes */

    /* GC runs in place from proc_heap_alloc once the heap has outgrown
     * gc_trigger — but only while gc_gate is 0. A region that holds live
     * heap references outside the TA stack (the collector's root set) closes
     * the gate with proc_gc_enter()/proc_gc_leave(); a collection moves the
     * objects. gc_pending records a request made while the gate was
     * closed; the next proc_gc_drain() (or gate-open allocation) honours it. */
    int gc_pending;
    int gc_trigger; /* heap_ptr above which an allocation requests collection */
    int gc_gate;    /* nesting depth of not-gc-safe regions; 0 = safe to collect */

    /* GC semispace (lazily allocated; sized to the collection's target
     * capacity). Released after each collection — the next one allocates a
     * fresh tospace — so an actor keeps a single resident buffer instead
     * of paying semispace's 2x forever. */
    uint8_t *gc_to;
    int gc_to_cap; /* malloc'd capacity of the gc_to buffer */
    int gc_to_size;

    /* Chunk arena (issue #160): where C callbacks allocate while the GC gate
     * is closed. The first chunk is the TA_PROC_CHUNK0 slice at the bottom of
     * `mem`; overflow chains malloc'd blocks (TaChunk). Objects here never
     * move and are invisible to the collector; at OP_CCALL exit the result
     * graph converges into the heap and the chain resets (inline chunk kept,
     * malloc'd chunks freed). gc_ck_total is the U watermark. */
    TaChunk gc_ck_head; /* inline first chunk (data = p->mem, cap fixed) */
    TaChunk *gc_ck_cur; /* chunk being bumped: &gc_ck_head or a malloc'd one */
    int gc_ck_total;
    int in_ccall;       /* inside a C module callback (OP_CCALL): heap allocs
                         * route to the chunk arena. A dedicated window flag,
                         * NOT the gc gate: the gate also closes inside the VM
                         * itself (val_deep_copy on recv/spawn, DOWN walks),
                         * whose results belong in the heap. */
    int gc_ck_converge; /* copy-out in progress: heap allocs bypass chunks, no GC */
    int gc_ck_promote;  /* a converge collection is running: chunk objects copy */

    /* GC stress knob (TA_GC_STRESS=N): allocation countdown until the next
     * collection request on this proc (sets gc_pending, honoured like any
     * other request once the gate is open). Only touched when the knob is on. */
    int gc_stress_cnt;

    /* retired-proc free-list linkage (proc_die → vm_free) */
    struct Proc *next_retired;
} Proc;

#define MAX_CFUNCS 128

/* Stringify a sanitizer tag macro (TA_MOD_TAG=tsan → "tsan") for use in
 * module paths: sanitizer builds load lib/http_tsan.dylib etc. */
#ifdef TA_MOD_TAG
#define TA_MOD_TAG_STR_(x) #x
#define TA_MOD_TAG_STR(x) TA_MOD_TAG_STR_(x)
#else
#define TA_MOD_TAG_STR(x) ""
#endif

/* Per-thread worker context */
typedef struct {
    VM *vm;
    Proc *current_proc;
    int thread_id;
} WorkerCtx;

/* Thread-local current process — set by worker_loop before executing a proc */
extern __thread Proc *tls_current_proc;

struct VM {
    Proc **procs;
    int procs_count, procs_cap;
    Proc *retired; /* dead procs deferred to vm_free (proc_die list) */

    int *runq; /* ready queue (pid array, circular buffer) */
    int rq_head, rq_tail, rq_cap;
    atomic_int rq_count;

    atomic_int next_pid;
    int next_ref; /* monitor ref counter */

    uint8_t *code; /* shared bytecode */
    int code_len, code_cap;
    int *fn_table;
    int fn_count, fn_table_cap;
    int top_fn_id;           /* fn_id of top-level thunk */
    int main_pid;            /* pid of main() process; -1 if none */
    atomic_int main_dead;    /* set when main() exits — triggers shutdown */
    atomic_int main_crashed; /* set when main() dies abnormally (reason !=
                                nil) — makes tavm exit non-zero */

    /* Per-fn_id name table (.tabc v2+; NULL entry = v1 module, fallback
     * "fn#<id>"). Appended in fn_id order across modules, read-only after
     * loading — used by prof.c / future debugger. */
    char **fn_names;
    int fn_names_count, fn_names_cap;

    /* symbol table — appended at load time AND interned on worker
     * threads at runtime (str.to_sym, DOWN payloads); guarded by
     * sym_lock */
    char **symbols;
    int sym_count, sym_cap;

    /* C function registry */
    struct {
        char *name;
        Val (*fn)(VM *vm, Val *args, int nargs);
        int nargs;
    } cfuncs[MAX_CFUNCS];
    int cfunc_count;

    /* Module registry */
    TaFunc **mod_funcs; /* per-module function arrays */
    int *mod_nfuncs;    /* per-module function counts */
    char **mod_names;   /* module names */
    int mod_count, mod_cap;

    /* Threading infrastructure */
    atomic_int active_procs;
    atomic_int busy_workers; /* workers currently executing an actor */
    atomic_int recv_armed;   /* procs with an armed recv_after() deadline;
                                lets deadline scans skip entirely when 0 */
    pthread_mutex_t rq_lock;
    pthread_cond_t rq_cond;
    pthread_mutex_t procs_lock; /* protects vm->procs[] access */
    pthread_mutex_t sym_lock;   /* protects vm->symbols/sym_count/sym_cap
                                 * (interning happens on worker threads) */

    /* Buffers displaced by realloc while worker threads may still hold
     * previously published pointers into them (vm->code, fn_table,
     * fn_names, symbols — see vm_append_module / vm_intern_symbol).
     * Bytecode and tables are append-only, so stale pointers stay
     * semantically valid; the old allocations are kept alive until
     * vm_free instead of being freed by realloc. */
    void **retired_bufs;
    int retired_count, retired_cap;
    pthread_mutex_t retired_lock; /* retired_bufs is touched under
                                   * sym_lock (interns) AND procs_lock
                                   * (module append) — needs its own */
    int nworkers;
    atomic_int stop;

    /* I/O poller wake pipe (multi-thread mode only). An arm site writes a
     * byte so an io_poller already blocked in poll() with the lazy 100ms
     * cap re-scans and adopts a newly armed recv_after/wait deadline
     * immediately instead of waiting out the cap. Both ends are -1 in
     * single-thread mode, where the lone worker re-scans its own deadlines
     * before polling and no wake is needed. */
    int wake_pipe_r, wake_pipe_w;

    pthread_t *workers;
    Val eval_result; /* set by OP_HALT for --eval mode */

    /* Sampling profiler (src/prof.c) — prof_on set once before vm_run and
     * never changed during it, so the worker hot loop reads a plain int. */
    int prof_on;
    struct ProfState *prof;
};

/* ============================================================
 * Bytecode instruction set
 * ============================================================ */

/* ============================================================
 * Bytecode (.tabc) format version
 *
 * Written into the header by serialize_tabc (lib/bootstrap/codegen.ta) and
 * checked by the loader (src/api.c) — bump both in lockstep.
 *
 *   1 — fn_table only
 *   2 — fn_table + per-fn name table
 *   3 — actor primitives behind OP_BUILTIN (v2 still used opcodes 34-48
 *       for them, so a v2 image cannot be interpreted by a v3 VM)
 * ============================================================ */
#define TABC_VERSION 3

typedef enum {
    /* stack */
    OP_PUSH_NIL,
    OP_PUSH_TRUE,
    OP_PUSH_FALSE,
    OP_PUSH_INT8,   /* i8 */
    OP_PUSH_INT,    /* i64 (8 bytes) */
    OP_PUSH_SYM,    /* idx (4 bytes) */
    OP_PUSH_STRING, /* len(4), data(len) */

    /* local variables */
    OP_LOAD,  /* offset */
    OP_STORE, /* offset */

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
    OP_JUMP,          /* addr */
    OP_JUMP_IF_FALSE, /* addr */
    OP_POP,
    OP_DUP,

    /* functions */
    OP_CLOSURE,   /* fn_id, nfree, [offset...] */
    OP_CALL,      /* nargs */
    OP_TAIL_CALL, /* nargs */
    OP_RET,

    /* actor primitives — moved to OP_BUILTIN (see builtin_table below).
     * The nine numbers they used are kept as reserved slots so no surviving
     * opcode number moves.  Nothing emits them any more; if one ever reaches
     * the dispatch loop it falls through to the unknown-opcode report, the
     * same arm an out-of-range opcode takes. */
    OP_RESERVED_SPAWN,       /* 34, was OP_SPAWN */
    OP_RESERVED_SPAWN_MAIN,  /* 35, was OP_SPAWN_MAIN */
    OP_RESERVED_SPAWN_CLOS,  /* 36, was OP_SPAWN_CLOS */
    OP_RESERVED_SEND,        /* 37, was OP_SEND */
    OP_RESERVED_RECV,        /* 38, was OP_RECV */
    OP_RESERVED_RECV_PEEK,   /* 39, was OP_RECV_PEEK */
    OP_RESERVED_RECV_COMMIT, /* 40, was OP_RECV_COMMIT */
    OP_SELF,                 /* 41 — the one actor primitive that stays an opcode */
    OP_RESERVED_MONITOR,     /* 42, was OP_MONITOR */

    /* built-in */
    OP_HALT,

    /* Numbering is continuous from OP_ENTER to OP_BUILTIN. The values are
     * spelled out because they are mirrored by the hand-maintained op_*
     * constants in lib/bootstrap/codegen.ta and indexed positionally by
     * src/api.c's instr_len table — change the three in lockstep.
     *
     * There are no pattern-matching opcodes: the compiler lowers match /
     * receive patterns to generic tests (OP_EQ, OP_IS_NIL, OP_IS_PAIR) plus
     * OP_JUMP_IF_FALSE, and pattern-variable binding to OP_LOAD/OP_STORE —
     * the same instruction repertoire as user-written if/let. */
    OP_ENTER = 44,               /* nslots(4 bytes) — reserve stack space for locals */
    OP_CCALL_NAME = 45,          /* sym_idx(4 bytes), nargs(1 byte) — name-based CCALL */
    OP_NE = 46,                  /* != — string-aware inequality (mirror of OP_EQ) */
    OP_PUSH_FLOAT = 47,          /* len(4), decimal digits (len) — float literal; the
                                compiler carries the literal as a decimal string
                                (the bootstrap language has no floats) and the VM
                                parses it with strtod at runtime */
    OP_RESERVED_RECV_AFTER = 48, /* was OP_RECV_AFTER — now BUILTIN_RECV_AFTER */
    OP_BUILTIN = 49,             /* idx(1 byte) — cold-path actor primitive: the byte
                                  * after the opcode indexes builtin_table[] (src/builtin.c),
                                  * and that function reads any further operands from the
                                  * instruction stream itself.  See BStatus / builtin_table
                                  * below.  Added at the end of the enum so no surviving
                                  * opcode number moves. */

    OP_COUNT
} OpCode;

/* ============================================================
 * Actor primitives — builtin function-pointer table (src/builtin.c)
 *
 * The cold-path actor primitives (spawn / send / receive / monitor …) are
 * not their own opcodes any more: codegen emits OP_BUILTIN + a one-byte
 * index, and the VM dispatches through builtin_table[].  They run on the
 * "cold path" — a builtin owns p->sp and p->pc for the duration of the call
 * (the OP_BUILTIN handler in vm.c publishes sp before it and reloads it
 * after), so the hot-path loop-local stack pointer never has to be threaded
 * into them.  Every entry has the uniform signature
 *
 *     BStatus f(VM *vm, Proc *p)
 *
 * and reports whether the proc finished the instruction (B_OK) or blocked
 * and must re-run the whole OP_BUILTIN instruction once the scheduler wakes
 * it (B_SUSPEND).  The instructions that stay opcodes — cons / car / cdr /
 * the type tests / arithmetic / divzero — are cheaper than one indirect
 * call, which is the admission rule for this table.
 * ============================================================ */
typedef enum { B_OK, B_SUSPEND } BStatus;

typedef BStatus (*BuiltinFn)(VM *vm, Proc *p);

/* Static table indices (the one-byte OP_BUILTIN operand).  This order is the
 * contract between the OP_BUILTIN indices codegen.ta will emit and
 * builtin_table[] in src/builtin.c — change both together.  spawn_main is a
 * separate entry, not spawn plus a flag: it is the only spawn variant that
 * records vm->main_pid. */
typedef enum {
    BUILTIN_SPAWN = 0,
    BUILTIN_SPAWN_MAIN, /* spawn + record vm->main_pid (compiler-spawned main()) */
    BUILTIN_SPAWN_CLOS, /* spawn a closure value taken from the operand stack */
    BUILTIN_SEND,
    BUILTIN_RECV,
    BUILTIN_RECV_PEEK,   /* selective receive: peek next mbox msg or block */
    BUILTIN_RECV_COMMIT, /* selective receive: consume matched msg, reset scan */
    BUILTIN_RECV_AFTER,  /* wait for next msg up to a deadline; nil on timeout */
    BUILTIN_MONITOR,
    BUILTIN_COUNT
} BuiltinId;

extern const BuiltinFn builtin_table[BUILTIN_COUNT];

/* ============================================================
 * C API — lifecycle
 * ============================================================ */

VM *vm_new(void);
void vm_free(VM *vm);

/* loading */
void vm_register(VM *vm, const char *name, Val (*fn)(VM *vm, Val *args, int nargs), int nargs);
void vm_register_module(VM *vm, const char *name, TaFunc *funcs, int nfuncs);
int vm_find_cfunc(VM *vm, const char *name);
int vm_load_c_module(VM *vm, const char *path);
void vm_register_net_module(VM *vm);
/* Monotonic clock in milliseconds (CLOCK_MONOTONIC). Shared by src/net.c
 * (connect deadlines) and src/scheduler.c (I/O poller deadline wakes). */
int64_t net_now_ms(void);
void vm_register_http_module(VM *vm);
int vm_load(VM *vm, const char *src);
int vm_load_file(VM *vm, const char *path);

/* execution */
int vm_spawn(VM *vm, int fn_id);
void vm_run(VM *vm);
/* Execute proc for at most `reductions` instructions (the scheduling quantum).
 * Returns 0 when the budget is exhausted (proc still PROC_RUNNING), -1 when
 * the proc suspended or died — the caller tells those apart via p->state. */
int vm_run_proc(VM *vm, Proc *proc, int reductions);

/* stack walking & fn-name resolution — shared by the sampling profiler
 * (prof.c) and the crash report (proc_die in scheduler.c). vm_walk_stack
 * fills out[] leaf..root (current fn first) and returns the depth; see
 * OP_CALL in vm.c for the frame layout. vm_fn_name returns the fn_id's
 * name from the .tabc v2 name table, or NULL if unknown (v1 module). */
int vm_walk_stack(const VM *vm, const Proc *p, int *out, int max_depth);
const char *vm_fn_name(const VM *vm, int fid);

/* yield API — lets C functions (e.g. net I/O) suspend the current proc
 * without polluting the value space or intruding into opcode logic. */
void vm_watch_fd(VM *vm, int fd, short events);
void vm_yield(VM *vm);
void vm_die(VM *vm, const char *reason);
/* Wake the I/O poller so it re-scans deadlines/fds while blocked in poll()
 * (multi-thread mode only; a no-op in single-thread mode). */
void vm_wake_poller(VM *vm);

/* scheduler API — process lifecycle, mailbox, run queue (scheduler.c) */
void runq_enqueue(VM *vm, int pid);
int runq_trydequeue(VM *vm);
Proc *proc_new(VM *vm);
void proc_die(VM *vm, Proc *p, Val reason);
void mbox_deliver(VM *vm, Proc *target, Val msg);
Val mbox_pop(Proc *p);

/* profiler API (prof.c) — reduction-boundary sampling, --profile flag */
void prof_init(VM *vm, const char *out_path); /* out_path base, NULL = "profile" */
void prof_finish(VM *vm);                     /* dump + free; idempotent */
void prof_collect(VM *vm, Proc *p, uint64_t dt_ns);
uint64_t prof_now_ns(void);

/* debug */
void print_val(VM *vm, Val v);

/* REPL */
Val vm_eval(VM *vm, const char *src);

/* ============================================================
 * C API — value constructors
 * ============================================================ */

/* val_int is static inline in ta_inline.h (the arithmetic opcodes build an
 * int result on every execution — it must not be an out-of-line call). */
Val val_float(double d);
Val val_nil(void);
Val val_true(void);
Val val_false(void);
Val val_symbol(uint32_t idx);
int vm_intern_symbol(VM *vm, const char *name);

/* api.c — free every token vector owned by p (proc_die cleanup; also
 * called from vm_free for procs that never went through proc_die). */
void vm_free_proc_tokvecs(Proc *p);
Val val_pid(uint32_t pid);

/* heap-allocated constructors (require process context) */
Val val_pair(Proc *p, Val car, Val cdr);
Val val_string(Proc *p, const char *data, int len);
Val val_bytes(Proc *p, const uint8_t *data, int len);

/* ============================================================
 * C API — value predicates & accessors
 * ============================================================ */

/* val_get_int / val_get_float / val_to_double / val_from_double are static
 * inline in ta_inline.h, alongside val_is_int / val_is_float (all of them sit
 * on the arithmetic/comparison hot paths — they must not be out-of-line
 * calls; as out-of-line calls each OP_ADD cost 3-4 real `bl`s).
 *
 * Contract of the float conversions — a float value is a normal
 * (non-NaN-boxed) double, i.e. a value whose top byte is NOT 0xFF (the tag
 * region); see ta_inline.h for the -NaN/-Inf collision note. val_to_double
 * widens int → double for mixed arithmetic; val_from_double never narrows
 * back to int (any op involving a float stays float). */

int val_is_nil(Val v);
int val_is_true(Val v); /* not nil and not false */

int val_is_pair(Val v);
Val val_get_car(Val v);
Val val_get_cdr(Val v);

int val_is_symbol(Val v);
uint32_t val_get_symbol(Val v);

int val_is_clos(Val v);
int val_is_pid(Val v);
uint32_t val_get_pid(Val v);

int val_is_pid(Val v);

int val_is_string(Val v);
HeapString *val_get_string(Val v);

int val_is_bytes(Val v);
HeapBytes *val_get_bytes(Val v);

/* ============================================================
 * Deep copy
 * ============================================================ */

Val val_deep_copy(Proc *target, Val v);
/* Deep copy for callback convergence: chunk objects rebuild in the heap,
 * arena (heap) objects pass through untouched, immediates as-is. */
Val val_converge_copy(Proc *target, Val root);
/* Upper bound of the heap bytes val_deep_copy(target, v) will allocate. */
int val_calc_heap_size(Val v);

/* ============================================================
 * Utility — dynamic array growth macro
 * ============================================================ */
#define DA_GROW(ptr, count, cap)                                                                   \
    do {                                                                                           \
        if ((count) >= (cap)) {                                                                    \
            (cap) = (cap) ? (cap) * 2 : 16;                                                        \
            (ptr) = realloc((ptr), (cap) * sizeof(*(ptr)));                                        \
        }                                                                                          \
    } while (0)

/* ============================================================
 * Garbage collection
 *
 * Collectors run in place from proc_heap_alloc, gated by Proc.gc_gate: a
 * collection only happens while no code holds a live heap reference outside
 * the TA stack, so no value ever has to be rooted. The arena is a fixed
 * reservation that never moves (a collection swaps its two semispaces).
 * See docs/design-decisions.md D11.
 * ============================================================ */

int gc_collect(Proc *p, int extra_room);
/* Convergence slow path: a collection that also promotes reachable chunk
 * objects (issue #160). Roots are the TA stack plus *extra_root (updated in
 * place). Returns 0; running out of cap is fatal here — a callback result
 * that cannot fit is a genuine out-of-memory. */
void gc_collect_converge(Proc *p, Val *extra_root);

/* All static inline helpers (proc_push/pop/peek, proc_heap_alloc,
 * proc_arena_grow, val_as_pair/clos, etc.) live here: */
#include "ta_inline.h"

#endif /* TA_H */
