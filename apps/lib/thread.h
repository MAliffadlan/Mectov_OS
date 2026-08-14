#ifndef MCT_THREAD_H
#define MCT_THREAD_H

#include "../../src/include/syscall.h"

// ============================================================
// Mectov OS Thread + TLS runtime (v38.24)
// ============================================================
// Real per-thread %gs: the kernel gives every thread a private FS/GS GDT
// descriptor whose base is the thread's control block (TCB). `%gs:0` reads
// the TCB self pointer — the classic i386 thread pointer — so this runtime
// can find its own state from anywhere in the thread.
//
// TCB layout (the kernel only knows the base; fields are ours):
//   +0x00 self  — must equal the TCB address (%gs:0 reads this)
//   +0x04 dtv   — reserved for a future dynamic-TLS vector (0 today)
//   +0x08 fn    — thread function (the clone trampoline calls it)
//   +0x0C arg   — thread argument
//   +0x10 ...   — 64 bytes of per-thread scratch (MCT_TLS_SCRATCH_SIZE)
//
// Main thread:  call mct_tls_init() once at app start.
// New threads:  mct_thread_create(fn, arg) — allocates a TCB, fills it, and
//               clone()s; the child's %gs already points at its TCB when the
//               trampoline runs, so fn() can use mct_tls_get() immediately.
// Join:         mct_thread_join(tid) — waitpid semantics on the child TID.

#define MCT_TLS_SCRATCH_OFFSET 16
#define MCT_TLS_SCRATCH_SIZE   64

typedef struct {
    void* self;                     // +0x00 == &tcb
    void* dtv;                      // +0x04 reserved
    void (*fn)(void*);              // +0x08 thread function
    void* arg;                      // +0x0C thread argument
} mct_tcb_t;

// The runtime allocates a little room beyond the header so apps can keep
// per-thread state right inside the TCB (a poor-man's TLS variable).
#define MCT_TCB_ALLOC (sizeof(mct_tcb_t) + MCT_TLS_SCRATCH_SIZE)

// Current thread's TCB (%gs:0 — the i386 thread pointer).
static inline mct_tcb_t* mct_tls_get(void) {
    mct_tcb_t* tp;
    __asm__ volatile("movl %%gs:0, %0" : "=r"(tp));
    return tp;
}

// Per-thread scratch pointer (unique copy per thread, at TCB+16).
static inline void* mct_tls_scratch(void) {
    return (char*)mct_tls_get() + MCT_TLS_SCRATCH_OFFSET;
}

// Main-thread init: allocate a TCB, point it at itself, install %gs.
// Returns 0 on success, -1 on failure. Call once, after malloc works.
static inline int mct_tls_init(void) {
    mct_tcb_t* tcb = (mct_tcb_t*)sys_malloc(MCT_TCB_ALLOC);
    if (!tcb) return -1;
    tcb->self = tcb;
    tcb->dtv = 0;
    tcb->fn = 0;
    tcb->arg = 0;
    return sys_tls_set((int)tcb);
}

// Child entry trampoline: the clone syscall starts the new thread here, with
// its %gs already pointing at the TCB. Pulls fn/arg out of the TCB and calls
// fn(arg); exits the thread when it returns (the parent joins via waitpid).
static void mct_thread_entry(void) {
    mct_tcb_t* tcb = mct_tls_get();
    void (*fn)(void*) = tcb->fn;
    void* arg = tcb->arg;
    fn(arg);
    syscall(SYS_EXIT, 0, 0, 0);
    for (;;) ;
}

// Create a thread running fn(arg), sharing this task's address space.
// Allocates the TCB, fills it, and clone()s with the default per-slot user
// stack. Returns the child TID to the caller, or -1 on failure.
static inline int mct_thread_create(void (*fn)(void*), void* arg) {
    mct_tcb_t* tcb = (mct_tcb_t*)sys_malloc(MCT_TCB_ALLOC);
    if (!tcb) return -1;
    tcb->self = tcb;
    tcb->dtv = 0;
    tcb->fn = fn;
    tcb->arg = arg;
    return sys_clone((int)(void*)&mct_thread_entry, 0, (int)tcb);
}

// Join a thread: blocks until the child exits (waitpid semantics).
// Returns the child TID on success, -1 on error.
static inline int mct_thread_join(int tid) {
    int st = 0;
    return sys_waitpid(tid, &st, 0);
}

#endif
