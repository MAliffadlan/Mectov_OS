// threaddemo — v38.24 TLS + clone() demo.
//
// Proves real per-thread storage end to end:
//   1. the main thread installs its own TLS (mct_tls_init)
//   2. spawns NUM_THREADS workers via sys_clone — each gets its OWN TCB and
//      a private FS/GS descriptor, so %gs:0 differs per thread
//   3. every worker keeps a private counter in its own TCB scratch area and
//      bumps it ITERS times. With working TLS the counters never collide —
//      each lands on exactly ITERS. Without TLS they would share one cell
//      and the values would race/corrupt.
//   4. the main thread joins all workers (waitpid) and verifies every
//      counter, including its own.
//
// Run from the terminal:  run /apps/threaddemo.mct
#include "src/include/syscall.h"
#include "lib/thread.h"

#define NUM_THREADS 4
#define ITERS 20000

static int results[NUM_THREADS];   // worker i's final counter (distinct index)
static int fails = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { sys_print(msg, 0x0C); fails++; } \
} while (0)

static void worker(void* arg) {
    int id = (int)(unsigned int)arg;
    // This thread's private counter lives in ITS TCB (unique per thread).
    int* c = (int*)mct_tls_scratch();
    *c = 0;
    for (int i = 0; i < ITERS; i++) (*c)++;
    results[id] = *c;
}

void _start(void) {
    sys_print("[THREADDEMO] start\n", 0x0E);

    if (mct_tls_init() != 0) {
        sys_print("[THREADDEMO] FAIL tls-init\n", 0x0C);
        sys_exit_with_code(1);
    }

    // The main thread has its own TLS counter too.
    int* mc = (int*)mct_tls_scratch();
    *mc = 0;
    for (int i = 0; i < ITERS; i++) (*mc)++;

    int tids[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        tids[i] = mct_thread_create(worker, (void*)(unsigned int)i);
        if (tids[i] < 0) {
            sys_print("[THREADDEMO] FAIL create\n", 0x0C);
            sys_exit_with_code(1);
        }
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        if (mct_thread_join(tids[i]) != tids[i]) {
            sys_print("[THREADDEMO] FAIL join\n", 0x0C);
            sys_exit_with_code(1);
        }
    }

    CHECK(*mc == ITERS, "[THREADDEMO] FAIL main counter\n");
    for (int i = 0; i < NUM_THREADS; i++) {
        if (results[i] != ITERS) {
            sys_print("[THREADDEMO] FAIL counter isolation\n", 0x0C);
            fails++;
        }
    }

    if (fails == 0) {
        sys_print("[THREADDEMO] ALL PASS\n", 0x0A);
        sys_exit_with_code(0);
    } else {
        sys_print("[THREADDEMO] FAILED\n", 0x0C);
        sys_exit_with_code(1);
    }
}
