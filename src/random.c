/*
 * random.c — pseudo-random number module for TinyActor VM
 *
 *   random.raw_srand(seed)   -> nil   (re)seed the process-level generator
 *   random.raw_rand_int(n)   -> Int in [0, n) | -1 (hard error: non-int
 *                               argument, or n <= 0)
 *   random.raw_rand_float()  -> Float in [0, 1)
 *
 * Registration follows the encoding-module pattern (see src/encoding.c):
 * the raw primitives are registered under their full dotted names WITHOUT
 * a vm_register_module("random") entry — a registry entry would make
 * `import random` a compile-time no-op and lib/random.ta — the public API
 * and the Result/Option lift layer — would never load. The raw_* prefix
 * likewise keeps the C names distinct from the public TA names, so a call
 * to random.rand_int goes through the lift in lib/random.ta instead of
 * hitting the C primitive directly via the codegen cfunc fast path.
 *
 * Random source: an in-module xorshift64* generator (Marsaglia's
 * xorshift, final multiplicative step from Vigna), NOT libc rand/rand_r:
 *   - the C standard does not fix rand()'s sequence, so "same seed ->
 *     same sequence" would hold on one libc only; xorshift64* is bit-
 *     identical on every platform and every run, which is what the
 *     deterministic-seed tests (and reproducible simulations) need;
 *   - rand_r() is an optional POSIX feature (absent on some libcs) and
 *     its low bits are weak, forcing masking for every draw;
 *   - arc4random* cannot be seeded, so deterministic tests would be
 *     impossible.
 *
 * State is process-global and guarded by a mutex: srand is documented as
 * process-level seeding, and the VM runs actors on several worker
 * threads — the mutex keeps "srand(42) then draw" one deterministic
 * sequence no matter which thread draws. Random generation is not on a
 * hot path, so the lock cost is irrelevant; simplicity wins.
 *
 * Seeding policy: before the first srand() the generator seeds lazily
 * from time(NULL) ^ (getpid() << 32) — NON-deterministic by design, so
 * two runs of the same program explore different draws. After
 * srand(seed) every draw is fully determined by the seed. Any seed value
 * (including 0) is usable: the raw seed goes through splitmix64, so the
 * xorshift state is never the degenerate all-zero.
 *
 * Uniformity: rand_int uses rejection sampling (Lemire's form: draws
 * below the 2^64 mod n threshold are retried), so every value in [0, n)
 * is exactly equally likely — no modulo bias. rand_float takes the top
 * 53 bits of a draw times 2^-53: every representable double in [0,1)
 * with 53-bit granularity is equally likely, and 1.0 is excluded by
 * construction (the 53-bit mantissa maxes out at 2^53 - 1).
 *
 * Range limit: TA ints are 48-bit two's complement, so n must lie in
 * [1, 2^47 - 1]; anything else is the hard error -1, lifted to Result by
 * lib/random.ta.
 */

#include "ta.h"
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t rng_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t rng_state;
static int rng_seeded;

/* splitmix64: turn an arbitrary seed into a well-mixed nonzero state. */
static uint64_t splitmix64(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* xorshift64* step; state must be nonzero (guaranteed by seeding). */
static uint64_t rng_next(void) {
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* Lazily install the default (time ^ pid) seed on first use. Caller
 * holds rng_lock. */
static void rng_seed_if_needed(void) {
    if (!rng_seeded) {
        uint64_t s = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32);
        rng_state = splitmix64(&s);
        rng_seeded = 1;
    }
}

static Val rand_srand(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_int(args[0]))
        return val_int(-1);
    uint64_t s = (uint64_t)val_get_int(args[0]);
    pthread_mutex_lock(&rng_lock);
    rng_state = splitmix64(&s);
    rng_seeded = 1;
    pthread_mutex_unlock(&rng_lock);
    return val_nil();
}

static Val rand_rand_int(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_int(args[0]))
        return val_int(-1);
    int64_t n = val_get_int(args[0]);
    if (n <= 0)
        return val_int(-1);
    uint64_t un = (uint64_t)n;
    pthread_mutex_lock(&rng_lock);
    rng_seed_if_needed();
    /* Rejection sampling: values below 2^64 mod n would be over-represented
     * by a plain modulo, so redraw them away. */
    uint64_t threshold = -un % un;
    uint64_t r = rng_next();
    while (r < threshold)
        r = rng_next();
    pthread_mutex_unlock(&rng_lock);
    return val_int((int64_t)(r % un));
}

static Val rand_rand_float(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    (void)args;
    pthread_mutex_lock(&rng_lock);
    rng_seed_if_needed();
    uint64_t r = rng_next();
    pthread_mutex_unlock(&rng_lock);
    return val_float((double)(r >> 11) * (1.0 / 9007199254740992.0));
}

TaFunc random_funcs[] = {
    {"raw_srand", rand_srand, 1},
    {"raw_rand_int", rand_rand_int, 1},
    {"raw_rand_float", rand_rand_float, 0},
    {NULL, NULL, 0},
};

/* Full dotted names only, no module-registry entry — see the header
 * comment (and the matching note in src/os.c / src/encoding.c). */
void vm_register_random_module(VM *vm) {
    for (int i = 0; random_funcs[i].name != NULL; i++) {
        char qualified[64];
        snprintf(qualified, sizeof(qualified), "random.%s", random_funcs[i].name);
        vm_register(vm, qualified, random_funcs[i].fn, random_funcs[i].nargs);
    }
}