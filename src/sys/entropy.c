// entropy.c — kernel entropy pool + ChaCha8-based CSPRNG (hardening step 2).
//
// Design notes:
//  * Entropy sources: TSC low bits at each addition, the addition counter, and
//    the caller-supplied counter (timer ticks, scancode, mouse deltas). ChaCha8
//    then irreversibly spreads these. This is a small embedded-style pool, not a
//    full Fortuna, but it beats both the previous fixed LCG and RNG output
//    derived directly from a public counter.
//  * Thread safety: a single short spinlock. IRQ callers (timer/kbd/mouse) run
//    with IF=0 already and use the plain lock; process-context callers (e.g.
//    read of /dev/random) use the irqsave variant. The lock is held only for
//    the 64-byte block mix, never across an I/O.
//  * The DRBG is backed by a 32-byte key/state. ChaCha state (16 words):
//    constants[4] | key[8] | counter[2] | nonce[2]; we initialize it from the
//    pool and use the first two output blocks as the next key/counter pair.

#include "../include/entropy.h"
#include "../include/utils.h"
#include "../include/msr.h"

#define CHACHA_ROUNDS 8
#define POOL_WORDS 8

static uint32_t pool[POOL_WORDS];
static uint32_t pool_mixed = 0;   // number of bytes mixed into the current pool
static uint32_t add_counter = 0;
static uint32_t pool_key[8];
static uint32_t pool_ctr[4];
static int seeded = 0;
static spinlock_t ent_lock = SPINLOCK_INIT;

static inline uint32_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return lo ^ hi;
}

static inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

#define QR(a, b, c, d) \
    a += b; d ^= a; d = rotl32(d, 16); \
    c += d; b ^= c; b = rotl32(b, 12); \
    a += b; d ^= a; d = rotl32(d, 8);  \
    c += d; b ^= c; b = rotl32(b, 7);

static void chacha_block(const uint32_t key[8], const uint32_t ctr[4],
                        uint32_t out[16]) {
    uint32_t x[16];
    // ChaCha constants "expand 32-byte k"
    x[0] = 0x61707865; x[1] = 0x3320646e; x[2] = 0x79622d32; x[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) x[4 + i] = key[i];
    x[12] = ctr[0]; x[13] = ctr[1]; x[14] = ctr[2]; x[15] = ctr[3];

    uint32_t w[16];
    for (int i = 0; i < 16; i++) w[i] = x[i];
    for (int r = 0; r < CHACHA_ROUNDS; r++) {
        QR(w[0], w[4], w[8], w[12]);
        QR(w[1], w[5], w[9], w[13]);
        QR(w[2], w[6], w[10], w[14]);
        QR(w[3], w[7], w[11], w[15]);
        QR(w[0], w[5], w[10], w[15]);
        QR(w[1], w[6], w[11], w[12]);
        QR(w[2], w[7], w[8], w[13]);
        QR(w[3], w[4], w[9], w[14]);
    }
    for (int i = 0; i < 16; i++) out[i] = w[i] + x[i];
}

static void reseed(void) {
    // Mix the pool into the DRBG key. The pool words themselves are accumulated
    // with the ticks; using the pool as chacha key spreads the entropy.
    uint32_t block[16];
    chacha_block(pool, pool_ctr, block);
    for (int i = 0; i < 8; i++) pool_key[i] = block[i];
    pool_ctr[0]++; if (pool_ctr[0] == 0) { pool_ctr[1]++; if (pool_ctr[1] == 0) { pool_ctr[2]++; if (pool_ctr[2] == 0) pool_ctr[3]++; } }
    // Feed extra asymmetry from two pool words that did not make the key.
    pool_key[0] ^= block[8];
    pool_key[7] ^= block[15];
    pool_mixed = 0;
    seeded = 1;
}

void entropy_init(void) {
    uint32_t ef = spin_lock_irqsave(&ent_lock);
    if (!seeded) {
        // Boot seed: stack of every possible cheap source available this early.
        // (TSC low bits carry the most real randomness; the rest are distinct
        // values so two identical boots on the same hardware still diverge.)
        pool[0] = read_tsc();
        pool[1] = read_tsc() ^ 0x9E3779B9;
        pool[2] = read_tsc() ^ add_counter;
        pool[3] = (uint32_t)(uintptr_t)&ent_lock;   // kernel VA of this struct
        pool[4] = read_tsc();
        pool[5] = 0x6A09E667;
        pool[6] = 0xBB67AE85;
        pool[7] = 0x3C6EF372;
        pool_ctr[0] = read_tsc(); pool_ctr[1] = 0; pool_ctr[2] = 0; pool_ctr[3] = 0;
        reseed();
    }
    spin_unlock_irqrestore(&ent_lock, ef);
}

void entropy_add(uint32_t counter) {
    uint32_t t = read_tsc();
    uint32_t ef = spin_lock_irqsave(&ent_lock);
    uint32_t idx = add_counter & (POOL_WORDS - 1);
    pool[idx] ^= t ^ (counter + 0x9E3779B9 + (add_counter << 15));
    // Rotate the pool so every word is touched across successive samples.
    uint32_t carry = pool[0];
    for (int i = 0; i < POOL_WORDS - 1; i++) pool[i] = pool[i + 1];
    pool[POOL_WORDS - 1] = carry;
    add_counter++;
    pool_mixed++;
    // Re-seed the DRBG whenever the pool has absorbed a full pool's worth of
    // samples (keeps /dev/random fresh between explicit reseeds).
    if ((pool_mixed & (POOL_WORDS - 1)) == 0) reseed();
    spin_unlock_irqrestore(&ent_lock, ef);
}

static void generate(uint32_t* out_words, int nwords) {
    uint32_t ef = spin_lock_irqsave(&ent_lock);
    if (!seeded) { spin_unlock_irqrestore(&ent_lock, ef); return; }
    uint32_t block[16];
    int produced = 0;
    while (produced < nwords) {
        chacha_block(pool_key, pool_ctr, block);
        pool_ctr[0] += 1;
        if (pool_ctr[0] == 0) {
            pool_ctr[1] += 1;
            if (pool_ctr[1] == 0) { pool_ctr[2] += 1; if (pool_ctr[2] == 0) pool_ctr[3] += 1; }
        }
        int take = 16;
        if (nwords - produced < take) take = nwords - produced;
        for (int i = 0; i < take; i++) out_words[produced + i] = block[i];
        produced += take;
        // Forward secrecy-ish: mix one output word back into the state.
        pool_key[0] ^= block[0];
    }
    spin_unlock_irqrestore(&ent_lock, ef);
}

int get_random_bytes(void* buf, uint32_t n) {
    if (n == 0) return 0;
    uint32_t words[16];
    uint8_t* out = (uint8_t*)buf;
    uint32_t done = 0;
    while (done < n) {
        uint32_t chunk = n - done;
        if (chunk > 64) chunk = 64;
        uint32_t nw = (chunk + 3) / 4;
        generate(words, (int)nw);
        if (!seeded) return -1;
        uint32_t bytes = chunk;
        if (bytes > nw * 4) bytes = nw * 4;
        for (uint32_t i = 0; i < bytes; i++) out[done + i] = (uint8_t)(words[i / 4] >> (8 * (i % 4)));
        done += bytes;
    }
    return 0;
}

uint32_t get_random_u32(void) {
    uint32_t w = 0;
    if (get_random_bytes(&w, 4) != 0) return 0;
    return w;
}
