// ============================================================
// sync.c — Kernel Semaphores & Futexes
// ============================================================
// Two user-facing synchronization primitives built on top of the
// scheduler's TASK_STATE_BLOCKED:
//
//   semaphore  — classic counting semaphore (System V style, id-based).
//   futex      — "fast user-space mutex": blocks on a user virtual address
//                only while *addr == expected (the standard PI-less
//                wait/retry loop apps use to build mutexes).
//
// Both park the calling task by flipping it to TASK_STATE_BLOCKED; the
// scheduler's READY scan skips such tasks, so a blocked task burns no CPU.
// The waking side (sem_post / futex_wake) flips it back to READY.
//
// All tables are static-sized; entries never move once allocated (waiters
// hold an index, not a pointer).
// ============================================================

#include "../include/sync.h"
#include "../include/task.h"
#include "../include/spinlock.h"
#include "../include/serial.h"
#include "../include/vmm.h"

#define MAX_SEMS   32
#define MAX_FUTEX  64
#define MAX_WAITERS 32  // tasks parked on one object
#define FUTEX_SWEEP_DIV_BASE 1  // v38.87: sweep expired timed-futex parkers every N timer ticks
// v38.91: runtime-tunable via /proc/sys/futex_sweep_div.
static int futex_sweep_div = FUTEX_SWEEP_DIV_BASE;

typedef struct {
    int in_use;
    int count;
    int waiters[MAX_WAITERS];
    int waiter_count;
} sem_t;

typedef struct {
    int in_use;
    uint32_t page_dir;   // futex address space (per-process keying)
    uint32_t addr;       // user virtual address being waited on
    int waiters[MAX_WAITERS];
    int waiter_count;
    int has_timeout;     // v38.87: bounded-park entries carry a deadline
    uint32_t expire_ms;  // absolute timer_ticks deadline (only if has_timeout)
} futex_t;

static sem_t   sems[MAX_SEMS];
static futex_t futexes[MAX_FUTEX];
static spinlock_t sync_lock = SPINLOCK_INIT;
extern volatile uint32_t timer_ticks;  // drivers/timer.c (ms since boot)

void sync_init(void) {
    for (int i = 0; i < MAX_SEMS; i++)   { sems[i].in_use = 0; sems[i].count = 0; sems[i].waiter_count = 0; }
    for (int i = 0; i < MAX_FUTEX; i++)  { futexes[i].in_use = 0; futexes[i].waiter_count = 0; }
    write_serial_string("[SYNC] init\n");
}

// Drop every waiter entry for `tid`. DEFERRED (v38.55): the obvious hook —
// calling this straight from task_cleanup — deadlocks on SMP. Signal-driven
// teardown (SIGKILL of a parked thread, etc.) runs WITH task_lock held, while
// sem_wait/futex_wait hold sync_lock across task_set_state (documented order
// sync_lock > task_lock): eager cleanup there is a classic AB-BA spin with
// interrupts off on both cores. Instead, teardown only SETS a pending bit
// (cli-scoped, no spinlocks) and the BSP main loop drains it with no other
// lock held.
static volatile uint32_t cleanup_pending_lo = 0;  // tids 1..31
static volatile uint32_t cleanup_pending_hi = 0;  // tids 32..63

void sync_task_cleanup_defer(int tid) {
    if (tid <= 0 || tid > 63) return;   // MAX_TASKS is 64 (task.c)
    uint32_t eflags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(eflags) : : "memory");
    if (tid < 32) cleanup_pending_lo |= (1u << tid);
    else          cleanup_pending_hi |= (1u << (tid - 32));
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
}

void sync_task_cleanup(int tid) {
    if (tid <= 0 || tid > 63) return;   // MAX_TASKS is 64 (task.c)
    // Recycle race (v38.89): the pending bit was set by a DEATH, but the
    // tid slot may have been reused since — a phase-1 worker exits, its tid
    // is recycled by a phase-2 clone, and the new owner parks, all before
    // the BSP drain runs. Purging unconditionally would evict the LIVE new
    // owner's waiter registration; no wake path can reach an unlisted
    // waiter, so it sleeps forever while every core idles (observed: a
    // consumer stranded on not_empty, all vCPUs HLT, no panic). Skipping
    // live tids is exact: waiter registration (sem/futex wait) and this
    // purge serialize on sync_lock, so a tid observed live here owns
    // whatever entries it holds. (Same task_is_alive precedent as wake_one
    // below, which already runs under this lock.)
    extern int task_is_alive(int);
    __asm__ volatile("cli");
    spin_lock(&sync_lock);
    if (!task_is_alive(tid)) {
    for (int i = 0; i < MAX_SEMS; i++) {
        if (!sems[i].in_use || sems[i].waiter_count == 0) continue;
        int w = 0;
        for (int k = 0; k < sems[i].waiter_count; k++) {
            if (sems[i].waiters[k] != tid) sems[i].waiters[w++] = sems[i].waiters[k];
        }
        sems[i].waiter_count = w;
    }
    for (int i = 0; i < MAX_FUTEX; i++) {
        if (!futexes[i].in_use || futexes[i].waiter_count == 0) continue;
        int w = 0;
        for (int k = 0; k < futexes[i].waiter_count; k++) {
            if (futexes[i].waiters[k] != tid) futexes[i].waiters[w++] = futexes[i].waiters[k];
        }
        futexes[i].waiter_count = w;
    }
    }  // end if (!task_is_alive): live tids keep their own registrations
    spin_unlock(&sync_lock);
    __asm__ volatile("sti");
}

// Drain pending cleanups. Called from the BSP main loop ONLY (never from
// teardown/scheduler context): takes sync_lock alone, so the documented
// ordering stays intact. Runs every main-loop iteration, so a dead task's
// stale entries live for well under a millisecond.
void sync_drain_pending(void) {
    if (cleanup_pending_lo == 0 && cleanup_pending_hi == 0) return;
    uint32_t lo, hi;
    uint32_t eflags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(eflags) : : "memory");
    lo = cleanup_pending_lo; hi = cleanup_pending_hi;
    cleanup_pending_lo = 0; cleanup_pending_hi = 0;
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
    while (lo) {
        int tid = __builtin_ctz(lo);   // BSF: no libgcc call on i386
        lo &= lo - 1;
        sync_task_cleanup(tid);
    }
    while (hi) {
        int tid = 32 + __builtin_ctz(hi);
        hi &= hi - 1;
        sync_task_cleanup(tid);
    }
}

// Wake the first LIVE waiter of `list`. Returns 1 if someone was woken.
// v38.55: dead tasks (ZOMBIE/FREE — killed while parked, or exited via the
// signal paths) are dropped without waking; before this, a wake spent on a
// corpse silently lost its token and wedged the remaining waiters.
static int wake_one(int* waiters, int* waiter_count) {
    extern int task_is_alive(int);
    while (*waiter_count > 0) {
        int tid = waiters[0];
        // An idle task in a waiter list is corruption (it can never legitimately
        // call futex_wait). task_set_state already refuses to wake it; dropping it
        // here keeps the bogus entry from wedging the queue forever.
        if (tid > 0) {
            extern int task_is_idle(int);
            if (task_is_idle(tid)) {
                for (int i = 1; i < *waiter_count; i++) waiters[i - 1] = waiters[i];
                (*waiter_count)--;
                continue;
            }
        }
        if (tid <= 0 || !task_is_alive(tid)) {
            // Dead waiter: drop and look at the next one.
            for (int i = 1; i < *waiter_count; i++) waiters[i - 1] = waiters[i];
            (*waiter_count)--;
            continue;
        }
        // shift queue (FIFO fairness)
        for (int i = 1; i < *waiter_count; i++) waiters[i - 1] = waiters[i];
        (*waiter_count)--;
        task_set_state(tid, TASK_STATE_READY);
        return 1;
    }
    return 0;
}

// ============================================================
// Semaphores
// ============================================================
int sem_create(int initial) {
    int id = -1;
    __asm__ volatile("cli");
    spin_lock(&sync_lock);
    for (int i = 0; i < MAX_SEMS; i++) {
        if (!sems[i].in_use) {
            sems[i].in_use = 1;
            sems[i].count = (initial < 0) ? 0 : initial;
            sems[i].waiter_count = 0;
            id = i;
            break;
        }
    }
    spin_unlock(&sync_lock);
    __asm__ volatile("sti");
    return id;
}

int sem_wait(int id) {
    if (id < 0 || id >= MAX_SEMS) return -1;
    int tid = get_current_task();
    __asm__ volatile("cli");
    spin_lock(&sync_lock);
    if (!sems[id].in_use) {
        spin_unlock(&sync_lock);
        __asm__ volatile("sti");
        return -1;
    }
    if (sems[id].count > 0) {
        sems[id].count--;
        spin_unlock(&sync_lock);
        __asm__ volatile("sti");
        return 0;
    }
    // Block: park this task on the semaphore's queue. If the queue is full,
    // refuse to block — parking a task that no wake-up path can ever reach
    // would hang it forever.
    if (sems[id].waiter_count >= MAX_WAITERS) {
        spin_unlock(&sync_lock);
        __asm__ volatile("sti");
        return -2;
    }
    sems[id].waiters[sems[id].waiter_count++] = tid;
    task_set_state(tid, TASK_STATE_BLOCKED);
    spin_unlock(&sync_lock);
    __asm__ volatile("sti");

    // Wait to be woken (scheduler runs other tasks meanwhile).
    for (;;) {
        __asm__ volatile("pause");
        if (task_get_state(tid) != TASK_STATE_BLOCKED) break;
    }
    return 0;
}

int sem_post(int id) {
    if (id < 0 || id >= MAX_SEMS) return -1;
    __asm__ volatile("cli");
    spin_lock(&sync_lock);
    if (!sems[id].in_use) {
        spin_unlock(&sync_lock);
        __asm__ volatile("sti");
        return -1;
    }
    if (wake_one(sems[id].waiters, &sems[id].waiter_count)) {
        // a parked task takes the token directly
    } else {
        if (sems[id].count < 0x7FFFFFFF) sems[id].count++;
    }
    spin_unlock(&sync_lock);
    __asm__ volatile("sti");
    return 0;
}

int sem_destroy(int id) {
    if (id < 0 || id >= MAX_SEMS) return -1;
    __asm__ volatile("cli");
    spin_lock(&sync_lock);
    if (!sems[id].in_use) {
        spin_unlock(&sync_lock);
        __asm__ volatile("sti");
        return -1;
    }
    // Wake everyone parked (they will see the semaphore destroyed).
    for (int i = 0; i < sems[id].waiter_count; i++) {
        task_set_state(sems[id].waiters[i], TASK_STATE_READY);
    }
    sems[id].in_use = 0;
    sems[id].waiter_count = 0;
    spin_unlock(&sync_lock);
    __asm__ volatile("sti");
    return 0;
}

// ============================================================
// Futexes — keyed by (page_dir, addr)
// ============================================================
static futex_t* futex_find(uint32_t pd, uint32_t addr) {
    for (int i = 0; i < MAX_FUTEX; i++) {
        if (futexes[i].in_use && futexes[i].page_dir == pd && futexes[i].addr == addr)
            return &futexes[i];
    }
    return NULL;
}

static futex_t* futex_alloc(uint32_t pd, uint32_t addr) {
    futex_t* f = futex_find(pd, addr);
    if (f) return f;
    for (int i = 0; i < MAX_FUTEX; i++) {
        if (!futexes[i].in_use) {
            futexes[i].in_use = 1;
            futexes[i].page_dir = pd;
            futexes[i].addr = addr;
            futexes[i].waiter_count = 0;
            futexes[i].has_timeout = 0;
            return &futexes[i];
        }
    }
    return NULL;
}

int futex_wait(uint32_t addr, uint32_t expected) {
    // The address is a user virtual address in the current address space.
    // Validate it BEFORE dereferencing: an unmapped/evil pointer from Ring 3
    // would page-fault at CPL 0 (kernel panic) on the very first read.
    extern int validate_user_ptr(const void* ptr, uint32_t size);
    if (!validate_user_ptr((const void*)(uintptr_t)addr, 4)) return -2;
    volatile uint32_t* p = (volatile uint32_t*)(uintptr_t)addr;
    if (*p != expected) return -1;

    int tid = get_current_task();
    uint32_t pd = task_get_page_dir(tid);

    // Critical section: register the waiter + block it ATOMICALLY with the
    // re-check, under IF=0. task_set_state now PRESERVES IF (irqsave), so
    // interrupts stay disabled until sync_lock is released below — the timer
    // can never preempt us while holding sync_lock, which would leave the
    // lock owned by a task that is BLOCKED and can only be woken by code that
    // itself needs sync_lock (permanent deadlock).
    __asm__ volatile("cli");
    spin_lock(&sync_lock);
    // Re-check under the lock: a concurrent futex_wake between the read above
    // and now would otherwise be lost (classic missed-wakeup race).
    if (*p != expected) {
        spin_unlock(&sync_lock);
        __asm__ volatile("sti");
        return -1;
    }
    futex_t* f = futex_alloc(pd, addr);
    if (!f || f->waiter_count >= MAX_WAITERS) {
        spin_unlock(&sync_lock);
        __asm__ volatile("sti");
        return -2; // table full — caller may spin or fail
    }
    f->waiters[f->waiter_count++] = tid;
    task_set_state(tid, TASK_STATE_BLOCKED);
    spin_unlock(&sync_lock);
    __asm__ volatile("sti");

    // Wait to be woken (the scheduler runs other tasks meanwhile; the wake
    // path flips us to READY and this loop exits).
    for (;;) {
        __asm__ volatile("pause");
        if (task_get_state(tid) != TASK_STATE_BLOCKED) break;
    }
    return 0;
}

int futex_wake(uint32_t addr, int max_waiters) {
    if (max_waiters <= 0) return 0;
    uint32_t pd = task_get_page_dir(get_current_task());
    int woken = 0;
    __asm__ volatile("cli");
    spin_lock(&sync_lock);        futex_t* f = futex_find(pd, addr);
    if (f) {
        while (woken < max_waiters && f->waiter_count > 0) {
            if (wake_one(f->waiters, &f->waiter_count)) woken++;
        }
        // If nobody is waiting anymore, reclaim the slot (small table).
        if (f->waiter_count == 0) f->in_use = 0;
    }
    spin_unlock(&sync_lock);
    __asm__ volatile("sti");
    return woken;
}

// ------------------------------------------------------------
// v38.91 — Linux FUTEX_WAIT_REQUEUE semantics for condvar broadcast:
// wake up to `max_wake` waiters on `addr`, then MOVE the remaining waiters
// to `mutex_addr`'s futex queue so the next mutex_unlock wake releases them
// one at a time (no thundering herd: N waiters no longer all wake, all
// fail the mutex cmpxchg, and all re-park). Returns the number of waiters
// handled (woken + moved), -1 if *addr != expected, -2 on table/bad-arg
// failure. Caller protocol (condvar broadcast): bump the seq word FIRST,
// then requeue with the pre-bump value as `expected`.
// ------------------------------------------------------------
int futex_requeue(uint32_t addr, uint32_t expected, uint32_t mutex_addr, int max_wake) {
    if (max_wake < 0) max_wake = 0;
    extern int validate_user_ptr(const void* ptr, uint32_t size);
    if (!validate_user_ptr((const void*)(uintptr_t)addr, 4) ||
        !validate_user_ptr((const void*)(uintptr_t)mutex_addr, 4)) return -2;
    volatile uint32_t* p = (volatile uint32_t*)(uintptr_t)addr;
    int tid = get_current_task();
    uint32_t pd = task_get_page_dir(tid);
    int woken = 0, moved = 0;

    __asm__ volatile("cli");
    spin_lock(&sync_lock);
    if (*p != expected) {
        spin_unlock(&sync_lock);
        __asm__ volatile("sti");
        return -1;
    }
    futex_t* src = futex_find(pd, addr);
    if (!src) {
        spin_unlock(&sync_lock);
        __asm__ volatile("sti");
        return 0;                      // nobody waiting: nothing to do
    }
    futex_t* dst = (mutex_addr == addr) ? src : futex_alloc(pd, mutex_addr);
    if (!dst) {
        spin_unlock(&sync_lock);
        __asm__ volatile("sti");
        return -2;
    }
    // Wake the head of the cond queue first (FIFO fairness preserved).
    while (woken < max_wake && src->waiter_count > 0) {
        if (!wake_one(src->waiters, &src->waiter_count)) break;
        woken++;
    }
    // Move whoever is left to the mutex queue, preserving FIFO order.
    while (src->waiter_count > 0 && dst->waiter_count < MAX_WAITERS) {
        dst->waiters[dst->waiter_count++] = src->waiters[0];
        for (int i = 1; i < src->waiter_count; i++) src->waiters[i - 1] = src->waiters[i];
        src->waiter_count--;
        moved++;
    }
    // Waiters that did not fit stay on the cond queue; their 50 ms timed
    // park still releases them (they re-check the predicate), so overflow
    // can never wedge — it just falls back to the pre-requeue behavior.
    if (src->waiter_count == 0 && src != dst) src->in_use = 0;
    spin_unlock(&sync_lock);
    __asm__ volatile("sti");
    return woken + moved;
}

// ------------------------------------------------------------
// v38.87 — Timed futex wait: bounded parking (Linux FUTEX_WAIT_TIMEOUT
// semantics). Blocks while *p == expected, but the sweep below wakes the
// parker once the deadline passes even if nobody ever called futex_wake.
// This bounds any lost-wakeup window to the sweep period: the condvar
// protocol always writes the word (seq++) BEFORE calling wake, so a parker
// released by the sweep re-checks a word that already moved and retries the
// predicate — it can never stay stuck on a satisfied condition.
// Returns 0 (parked, then released — woken OR swept; caller re-checks the
// predicate either way), -1 (value changed, never slept), -2 (bad ptr /
// table full).
// ------------------------------------------------------------
int futex_wait_timeout(uint32_t addr, uint32_t expected, uint32_t timeout_ms) {
    extern int validate_user_ptr(const void* ptr, uint32_t size);
    if (!validate_user_ptr((const void*)(uintptr_t)addr, 4)) return -2;
    volatile uint32_t* p = (volatile uint32_t*)(uintptr_t)addr;
    if (*p != expected) return -1;

    int tid = get_current_task();
    uint32_t pd = task_get_page_dir(tid);

    // Same critical-section discipline as futex_wait: register the waiter and
    // block it atomically with the re-check, under IF=0 (task_set_state
    // preserves IF, so interrupts stay off until sync_lock is released).
    __asm__ volatile("cli");
    spin_lock(&sync_lock);
    if (*p != expected) {
        spin_unlock(&sync_lock);
        __asm__ volatile("sti");
        return -1;
    }
    futex_t* f = futex_alloc(pd, addr);
    if (!f || f->waiter_count >= MAX_WAITERS) {
        spin_unlock(&sync_lock);
        __asm__ volatile("sti");
        return -2;
    }
    f->waiters[f->waiter_count++] = tid;
    if (timeout_ms == 0) timeout_ms = 1;      // 0 => expire on the next sweep
    f->has_timeout = 1;
    f->expire_ms = timer_ticks + timeout_ms;  // absolute deadline
    task_set_state(tid, TASK_STATE_BLOCKED);
    spin_unlock(&sync_lock);
    __asm__ volatile("sti");

    for (;;) {
        __asm__ volatile("pause");
        if (task_get_state(tid) != TASK_STATE_BLOCKED) break;
    }
    return 0;
}

// v38.91: /proc/sys tunable (see vfs.c registry).
int proc_sys_futex_sweep_div(void) { return futex_sweep_div; }
void proc_sys_futex_sweep_div_set(int v) {
    if (v >= 1 && v <= 50) futex_sweep_div = v;
}

// v38.87 — Timer-tick sweep: wake parkers whose deadline expired. Runs from
// irq_handler BEFORE schedule(): the sync_lock -> task_set_state(task_lock)
// ordering matches futex_wait — calling this from inside schedule() (which
// already holds task_lock) would be an ABBA deadlock. A busy sync_lock just
// skips this round; the next tick (10 ms later) retries.
void futex_sweep(void) {
    static int sweep_div = 0;
    if (futex_sweep_div < 1) futex_sweep_div = 1;
    if (++sweep_div < futex_sweep_div) return;
    sweep_div = 0;
    if (sync_lock.locked) return;
    uint32_t now = timer_ticks;

    __asm__ volatile("cli");
    spin_lock(&sync_lock);
    int woken_any = 0;
    for (int i = 0; i < MAX_FUTEX; i++) {
        if (!futexes[i].in_use || !futexes[i].has_timeout) continue;
        if ((int)(now - futexes[i].expire_ms) < 0) continue;  // not yet due
        futexes[i].has_timeout = 0;
        while (futexes[i].waiter_count > 0) {
            if (!wake_one(futexes[i].waiters, &futexes[i].waiter_count)) break;
            woken_any = 1;
        }
        if (futexes[i].waiter_count == 0) futexes[i].in_use = 0;
    }
    spin_unlock(&sync_lock);
    // NOTE: no `sti` here — the sweep runs inside irq_handler (interrupt
    // gate, IF=0). Re-enabling interrupts mid-handler nests timer IRQs on
    // the same kernel stack and double-schedules: corrupts frames, GPF.
    // (futex_wait's sti is correct only because it runs in syscall context.)
    if (woken_any) {
        extern int take_resched(void);
        take_resched();  // run the scheduler on this CPU right away
    }
}
