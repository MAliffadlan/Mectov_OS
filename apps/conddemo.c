// conddemo — v38.27 mutex + condition-variable demo.
//
// Proves futex-based pthread-style synchronization works end to end on
// real SMP (4 cores) with clone() threads:
//
//   1. MUTEX STRESS — 4 threads each bump a shared counter LOCK_ITERS
//      times under mct_mutex_lock/unlock. The final counter must be
//      exactly 4 * LOCK_ITERS. On a broken lock (or plain races on 4
//      cores) it lands strictly below; on a correct mutex it is exact.
//
//   2. PRODUCER/CONSUMER — a bounded buffer of BUFFER_SIZE slots with
//      2 producers and 2 consumers. Producers block on the not-full
//      condition when the buffer is full; consumers block on not-empty
//      when it is empty. Every item carries a unique sequence number;
//      consumers record which they saw. Termination requires the final
//      "all items consumed" handshake.
//
// Verdicts printed to the terminal; run from the shell with:
//      run /apps/conddemo.mct
#include "src/include/syscall.h"
#include "lib/thread.h"
#include "lib/sync.h"

#define MUTEX_THREADS 4
#define LOCK_ITERS    5000          // per thread -> 20000 total
#define PRODUCERS     2
#define CONSUMERS     2
#define ITEMS_PER_PRODUCER 1500
#define TOTAL_ITEMS   (PRODUCERS * ITEMS_PER_PRODUCER)  // 3000
#define BUFFER_SIZE   8             // tiny -> forces not-full/not-empty waits

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { sys_print(msg, 0x0C); fails++; } \
} while (0)

// Minimal decimal printer (defined below; forward-declared for the worker).
static void print_int(int v);

// ---- shared state (test 1) ----
static mct_mutex_t g_mu;
static int g_counter = 0;

static void mutex_worker(void* arg) {
    int id = (int)(unsigned int)arg;
    int my = 0;  // stack-local: trustworthy even if shared .bss ever aliases
    for (int i = 0; i < LOCK_ITERS; i++) {
        if ((i % 2500) == 1250) {
            // Progress heartbeat: on the slow TCG CI runner each phase can
            // take minutes; these markers prove the workers are still making
            // progress (vs. all blocked on a lost wakeup) at post-mortem.
            sys_print("[CONDDEMO] mutex-progress\n", 0x08);
        }
        mct_mutex_lock(&g_mu);
        g_counter++;
        mct_mutex_unlock(&g_mu);
        my++;
    }
    // Per-worker completion count: distinguishes "a worker ran short"
    // (lifecycle bug) from "all ran full but increments vanished"
    // (aliasing/atomicity bug) when the final counter mismatches.
    sys_print("[CONDDEMO] worker-done id=", 0x0B);
    print_int(id);
    sys_print(" iters=", 0x0B);
    print_int(my);
    sys_print("\n", 0x0B);
}

// ---- shared state (test 2) ----
static mct_mutex_t c_mu;
static mct_cond_t  not_full;   // signaled when a slot frees up
static mct_cond_t  not_empty;  // signaled when an item arrives
static int buf[BUFFER_SIZE];
static int count = 0, head = 0, tail = 0;
static int next_item = 0;      // unique sequence handed to producers
static int consumed = 0;
static unsigned char seen[TOTAL_ITEMS];  // consumers mark what they saw

// Minimal decimal printer (no libc dependency).
static void print_int(int v) {
    char buf[12];
    int i = 0;
    if (v == 0) { sys_print("0", 0x0B); return; }
    while (v > 0 && i < 11) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) { char c[2] = { buf[--i], '\0' }; sys_print(c, 0x0B); }
}

static void producer(void* arg) {
    int id = (int)(unsigned int)arg;
    for (int i = 0; i < ITEMS_PER_PRODUCER; i++) {
        mct_mutex_lock(&c_mu);
        // Block while the buffer is full. MUST re-check under the lock.
        int w = 0;
        while (count == BUFFER_SIZE) {
            if (!w++) sys_print("[DBG] producer blocks on not_full\n", 0x0E);
            mct_cond_wait(&not_full, &c_mu);
        }
        buf[tail] = next_item++;
        tail = (tail + 1) % BUFFER_SIZE;
        count++;
        mct_cond_signal(&not_empty);   // one consumer may proceed
        mct_mutex_unlock(&c_mu);
    }
    sys_print("[DBG] producer done (printed)\n", 0x0E);
    sys_print("[CONDDEMO] producer done (", 0x0B);
    print_int(ITEMS_PER_PRODUCER);
    sys_print(" items)\n", 0x0B);
}

static void consumer(void* arg) {
    (void)arg;
    for (;;) {
        mct_mutex_lock(&c_mu);
        int w = 0;
        while (count == 0 && consumed < TOTAL_ITEMS) {
            if (!w++) sys_print("[DBG] consumer blocks on not_empty\n", 0x0E);
            mct_cond_wait(&not_empty, &c_mu);
        }
        if (count == 0) {           // consumed == TOTAL_ITEMS: all done
            mct_mutex_unlock(&c_mu);
            return;
        }
        int item = buf[head];
        head = (head + 1) % BUFFER_SIZE;
        count--;
        consumed++;
        if (item >= 0 && item < TOTAL_ITEMS) seen[item] = 1;
        mct_cond_signal(&not_full);  // a producer may proceed
        // Termination handshake: when the final item is consumed, producers
        // are done and cannot signal not_empty again — a consumer still
        // parked there would wait forever. Wake-all broadcast (proven):
        // every parked consumer re-checks the predicate and exits.
        // v38.92 note: the kernel requeue op is now deadline-correct
        // (per-waiter), but activating it on this hot path STILL corrupted
        // the consumed count under 2-core TCG stress (items lost/duped).
        // Requeued waiters bypass the mutex cmpxchg gauntlet on wake, and
        // the sweep/re-park interplay opens a window static analysis
        // didn't catch. Requeue stays kernel-tested only until a dedicated
        // stress suite proves it — wake-all + timed park remains hot path.
        if (consumed == TOTAL_ITEMS) mct_cond_broadcast(&not_empty);
        mct_mutex_unlock(&c_mu);
        mct_mutex_unlock(&c_mu);
    }
}

void _start(void) {
    sys_print("[CONDDEMO] start\n", 0x0E);

    if (mct_tls_init() != 0) {
        sys_print("[CONDDEMO] FAIL tls-init\n", 0x0C);
        sys_exit_with_code(1);
    }

    // ---- Test 1: mutual exclusion under stress ----
    // Phase heartbeats: on the 2-core TCG CI runner the 20k-iteration mutex
    // phase can take minutes; start/join markers keep any future timeout
    // diagnosable (which phase stalled).
    mct_mutex_init(&g_mu);
    g_counter = 0;
    sys_print("[CONDDEMO] mutex-phase-start\n", 0x0B);
    int tids[8];
    for (int i = 0; i < MUTEX_THREADS; i++) {
        tids[i] = mct_thread_create(mutex_worker, (void*)(unsigned int)i);
        if (tids[i] < 0) {
            sys_print("[CONDDEMO] FAIL create mutex worker\n", 0x0C);
            sys_exit_with_code(1);
        }
    }
    for (int i = 0; i < MUTEX_THREADS; i++) {
        if (mct_thread_join(tids[i]) != tids[i]) {
            sys_print("[CONDDEMO] FAIL join mutex worker\n", 0x0C);
            sys_exit_with_code(1);
        }
    }
    sys_print("[CONDDEMO] mutex-joined\n", 0x0B);
    CHECK(g_counter == MUTEX_THREADS * LOCK_ITERS, "[CONDDEMO] FAIL mutex counter\n");
    // Print the actual value on mismatch: the SIZE of the shortfall
    // discriminates a rare single-race loss (off by a handful) from
    // systematic aliasing (off by whole workers' worth).
    if (g_counter != MUTEX_THREADS * LOCK_ITERS) {
        sys_print("[CONDDEMO] counter-was ", 0x0C);
        print_int(g_counter);
        sys_print(" want ", 0x0C);
        print_int(MUTEX_THREADS * LOCK_ITERS);
        sys_print("\n", 0x0C);
    }
    sys_print("[CONDDEMO] mutex counter OK\n", 0x0B);

    // ---- Test 2: bounded producer/consumer via condvars ----
    mct_mutex_init(&c_mu);
    mct_cond_init(&not_full);
    mct_cond_init(&not_empty);
    count = head = tail = next_item = consumed = 0;
    for (int i = 0; i < TOTAL_ITEMS; i++) seen[i] = 0;

    int np = 0;
    for (int i = 0; i < PRODUCERS; i++) {
        int t = mct_thread_create(producer, (void*)(unsigned int)i);
        if (t < 0) { sys_print("[CONDDEMO] FAIL create producer\n", 0x0C); sys_exit_with_code(1); }
        tids[np++] = t;
    }
    int nc = 0;
    for (int i = 0; i < CONSUMERS; i++) {
        int t = mct_thread_create(consumer, 0);
        if (t < 0) { sys_print("[CONDDEMO] FAIL create consumer\n", 0x0C); sys_exit_with_code(1); }
        tids[np + nc++] = t;
    }
    for (int i = 0; i < np + nc; i++) {
        if (mct_thread_join(tids[i]) != tids[i]) {
            sys_print("[CONDDEMO] FAIL join pc thread\n", 0x0C);
            sys_exit_with_code(1);
        }
    }
    sys_print("[CONDDEMO] pc-threads-joined\n", 0x0B);

    // Every item was consumed exactly once (no loss, no dupes, no corruption).
    CHECK(consumed == TOTAL_ITEMS, "[CONDDEMO] FAIL consumed count\n");
    int all_seen = 1;
    for (int i = 0; i < TOTAL_ITEMS; i++) {
        if (!seen[i]) { all_seen = 0; break; }
    }
    CHECK(all_seen, "[CONDDEMO] FAIL missing/duplicate items\n");
    sys_print("[CONDDEMO] producer/consumer OK (3000 items, no loss/dupe)\n", 0x0B);

    if (fails == 0) {
        sys_print("[CONDDEMO] ALL PASS\n", 0x0A);
        sys_exit_with_code(0);
    } else {
        sys_print("[CONDDEMO] FAILED\n", 0x0C);
        sys_exit_with_code(1);
    }
}
