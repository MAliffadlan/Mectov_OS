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
} futex_t;

static sem_t   sems[MAX_SEMS];
static futex_t futexes[MAX_FUTEX];
static spinlock_t sync_lock = SPINLOCK_INIT;

void sync_init(void) {
    for (int i = 0; i < MAX_SEMS; i++)   { sems[i].in_use = 0; sems[i].count = 0; sems[i].waiter_count = 0; }
    for (int i = 0; i < MAX_FUTEX; i++)  { futexes[i].in_use = 0; futexes[i].waiter_count = 0; }
    write_serial_string("[SYNC] init\n");
}

// Wake the first waiter of `list`. Returns 1 if someone was woken.
static int wake_one(int* waiters, int* waiter_count) {
    if (*waiter_count <= 0) return 0;
    int tid = waiters[0];
    // shift queue (FIFO fairness)
    for (int i = 1; i < *waiter_count; i++) waiters[i - 1] = waiters[i];
    (*waiter_count)--;
    task_set_state(tid, TASK_STATE_READY);
    return 1;
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
    spin_lock(&sync_lock);
    futex_t* f = futex_find(pd, addr);
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
