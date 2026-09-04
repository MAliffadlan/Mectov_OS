#include "../include/task.h"
#include "../include/mem.h"
#include "../include/serial.h"
#include "../include/io.h"
#include "../include/spinlock.h"
#include "../include/apic.h"
#include "../include/acpi.h"   // smp_cpu_count / smp_lapic_ids for per-CPU idle tasks
#include "../include/vmm.h"   // vmm_setup_user_stack(), USER_STACK_* layout
#include "../include/idt.h"   // registers_t
#include "../include/timer.h" // get_ticks()
#include "../include/fd.h"    // global_fds[] for fork fd sharing
#include "../include/utils.h" // memcpy/memset
#include "../include/loader.h" // loader_image_t for exec()
#include "../include/vfs.h"   // fs_nodes[] + offset reads for file-backed mmap
#include "../include/gdt.h"   // tls_gs_sel()/gdt_tls_update() (v38.24 TLS)
#include "../include/fpu.h"   // eager fxsave/fxrstor context switching (v38.41)

static spinlock_t task_lock = SPINLOCK_INIT;
// TLS_MAX_TASKS in gdt.h must cover every task slot: keep in sync with
// MAX_TASKS below (both 64). The GDT carries one FS+GS pair per slot.

#define MAX_TASKS 64
#define KERNEL_STACK_SIZE TASK_KSTACK_SIZE  // defined in task.h (shared with /proc)
// Kernel stacks live in their own page-aligned arena: one 20KB slot per task
// — a 4KB guard page (unmapped in every page directory) below a 16KB stack.
// An overflow faults on the guard page and the #PF handler (idt.c) panics
// with a clear message instead of silently corrupting the stack or the page
// tables. Slots are exactly 5 pages, so with the array 4KB-aligned every
// slot — and therefore every guard page — is page-aligned. User address
// spaces inherit the unmapped guards when they clone the kernel page tables
// (vmm_create_address_space copies PTEs verbatim from the boot directory).
#define KERNEL_STACK_GUARD 4096
#define KERNEL_STACK_SLOT  (KERNEL_STACK_SIZE + KERNEL_STACK_GUARD)
static uint8_t kstacks[MAX_TASKS][KERNEL_STACK_SLOT] __attribute__((aligned(4096)));

// Top of task tid's kernel stack (one past the last byte, i.e. the initial
// ESP for iret / the TSS.esp0 value).
static inline uint32_t kstack_top(int tid) {
    return (uint32_t)(uintptr_t)&kstacks[tid][KERNEL_STACK_GUARD] + KERNEL_STACK_SIZE;
}

void task_install_stack_guards(uint32_t page_dir);  // defined below init_tasking
// Address of task tid's guard page (first 4KB of its slot).
static inline uint32_t kstack_guard(int tid) {
    return (uint32_t)(uintptr_t)&kstacks[tid][0];
}
// USER_STACK_SIZE now lives in vmm.h — the Ring 3 stack is mapped into the
// task's own address space, not carved out of this struct.

// Task states come from task.h (TASK_STATE_FREE/RUNNING/READY/SLEEP/BLOCKED/ZOMBIE)

// Zombie tasks older than this (ms) are reaped even if their parent never
// calls waitpid() — a safety net for fire-and-forget launchers like the
// terminal's `run`, which would otherwise leak slots forever.
#define ZOMBIE_REAP_MS 15000

// Park loop for tasks killed by a default-action signal mid-return. The
// scheduler switches away on the next tick and the zombie slot is reaped.
static void task_dead_park(void) {
    for (;;) __asm__ volatile("hlt");
}

typedef struct {
    uint32_t esp;          // Saved stack pointer (points to register frame)
    uint32_t stack_watermark; // Peak kernel-stack bytes used (scheduler samples
                              // esp at each preemption; 100% of TASK_KSTACK_SIZE
                              // means the guard page is one push away).
    // NB: the kernel stack itself is NOT inline here — it lives in the
    // page-aligned kstacks[] arena above, with a guard page below it.
    int      state;        // 0=free, 1=running, 2=ready, 3=sleep, 4=blocked, 5=zombie
    uint8_t  ring;         // 0 = kernel task, 3 = user task
    // === NEW FIELDS (add-on, safe defaults) ===
    int      priority;     // 0=background, 1=interactive, 2=realtime
    int      sleep_ticks;  // remaining ticks until wake (0 = not sleeping)
    int      wait_ticks;   // consecutive ticks waiting in READY state
    int      rq_cpu;       // per-CPU runqueue this task is queued on (-1 = not queued)
    int      is_idle;      // 1 = pinned per-CPU idle task (never migrated)
    uint32_t page_dir;     // per-process page directory (0 = global identity)
    int      fd_table[16]; // local file descriptors mapped to global FDs
    char     launch_arg[128]; // command-line argument passed at launch
    int      trusted_shell; // 1 = verified shell host (terminal/explorer/taskmgr).
                            // Gates SYS_EXEC_CMD/SYS_KILL_TASK. Set ONLY by the
                            // kernel from the resolved image identity + parent
                            // trust (task_grant_trusted_shell) — the launch_arg
                            // string itself is user-controlled and forgeable,
                            // so it must never be a privilege input again.
    int      current_dir;  // per-task working directory
    uint32_t heap_ptr;     // current heap break (e.g. 0x08000000)
    // === NEW: process model ===
    int      parent;       // tid of the parent task (0 = kernel task)
    int      exit_code;    // exit status (valid while ZOMBIE)
    int      pgrp;         // process group id (0 = none); terminal signals target this
    int      session;      // session id (0 = none); controlling-terminal membership
    int      waiting;      // while blocked in waitpid: target pid (>0) or -1 (any)
    uint32_t pending_signals;   // bitmap of pending signals (bit sig)
    void*    signal_handlers[SIG_MAX]; // NULL=default, SIG_IGN_SENTINEL=ignore, else user fn
    uint32_t sig_masks[SIG_MAX];    // sa_mask: extra signals blocked while handler runs
    uint32_t sig_flags[SIG_MAX];    // per-signal sigaction flags (SA_RESTART, SA_NODEFER)
    uint32_t blocked_signals;   // bitmap of signals held pending (sigprocmask)
    uint32_t sig_restart_ticks; // remaining sleep ticks when a SA_RESTART handler interrupted SYS_SLEEP
    uint32_t sig_frame_esp;     // user-stack address of the sigframe awaiting SYS_SIGRETURN
    uint32_t zombie_since;      // tick when the task became a zombie (reap timeout)
    uint32_t shm_bits;          // bitmap of shm segments this task has attached
    mmap_region_t mmap_regions[MMAP_MAX_REGIONS]; // reserved mmap() ranges (0 = free)
    // === NEW: Unix user id (v38.23) ===
    int uid;                    // 0 = root, 1000 = user (permission checks)
    int gid;                    // group id (0 today, carried for the future)
    // === NEW: Thread-local storage (v38.24) ===
    uint32_t tls_base;          // user VA of this thread's TCB; 0 = no TLS.
                                // Its FS/GS GDT descriptors (slot = 2*tid)
                                // are kept in sync via gdt_tls_update().
    // === NEW: Resource limits (v38.28) ===
    rlimit_t rlimits[RLIM_NLIMITS];  // soft/hard per RLIMIT_* resource
    // === NEW: FPU/SSE context (v38.41) ===
    // 16-byte-aligned (fxsave #GP's on misaligned operands); schedule()
    // fxsave's the outgoing task's live state into it and fxrstor's the
    // incoming task's on every context switch.
    uint8_t  fxsave[512] __attribute__((aligned(16)));
} task_t;

static task_t tasks[MAX_TASKS];
static void mmap_free_dirty_bitmaps(int tid);  // defined in the mmap section
static int current_task[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
// Set at the end of init_tasking(). Before that, APs boot with
// current_task[cid] == -1 (they wake in smp_init(), program their LAPIC
// timer and sti before the scheduler exists) — that state is legitimate and
// must not trip the iret-ESP sanity check.
static volatile int scheduler_ready = 0;

// Default resource limits for a fresh task slot (set by init_tasking and by
// every task constructor that reuses a slot). Values are generous for the
// single-user desktop but real: 64 processes, 256 MB of address space, and
// the full 16-fd table per task. Root (uid 0) bypasses every limit.
static void rlimit_set_defaults(int tid) {
    tasks[tid].rlimits[RLIMIT_NPROC].cur  = 64;
    tasks[tid].rlimits[RLIMIT_NPROC].max  = 64;
    tasks[tid].rlimits[RLIMIT_AS].cur     = 256u * 1024u * 1024u;
    tasks[tid].rlimits[RLIMIT_AS].max     = 256u * 1024u * 1024u;
    tasks[tid].rlimits[RLIMIT_NOFILE].cur = 16;
    tasks[tid].rlimits[RLIMIT_NOFILE].max = 16;
}

// RLIMIT_NPROC enforcement: count live tasks with the same uid as `tid` and
// reject creation when the caller's soft NPROC limit is reached (POSIX: the
// limit applies to the real user id, so every process of that user counts,
// not just direct children). Root (uid 0) bypasses. Must be called with
// task_lock held. Returns 0 when the limit allows another task, -1 when not.
static int rlimit_nproc_allows(int tid) {
    if (tasks[tid].uid == ROOT_UID) return 0;  // root is never limited
    uint32_t limit = tasks[tid].rlimits[RLIMIT_NPROC].cur;
    if (limit == 0) return -1;
    int count = 0;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_STATE_FREE) continue;
        if (tasks[i].uid == tasks[tid].uid) count++;
        if (count >= (int)limit) return -1;
    }
    return 0;
}

// RLIMIT_AS enforcement: the task's committed address space is its heap
// (0x08000000 .. heap_ptr) plus every reserved mmap region. Returns bytes.
static uint32_t rlimit_as_used(int tid) {
    uint32_t used = 0;
    if (tasks[tid].heap_ptr > 0x08000000u) {
        used += tasks[tid].heap_ptr - 0x08000000u;
    }
    for (int j = 0; j < MMAP_MAX_REGIONS; j++) {
        used += tasks[tid].mmap_regions[j].size;
    }
    return used;
}

static inline int get_cid() { extern uint32_t smp_lapic_addr; return smp_lapic_addr ? (apic_get_id() & 15) : 0; }

// Exported CPU-id accessor for lock ownership keys in other subsystems
// (vfs.c, wm.c). Same semantics as the internal get_cid().
int task_get_cid(void) { return get_cid(); }
static int num_tasks = 0;
static int boot_current_dir = 0;
// Foreground process group of the controlling terminal (set by the shell via
// tcsetpgrp). 0 = no controlling terminal yet. Read by the SIGTTIN/SIGTTOU
// checks and by Ctrl+C/Ctrl+Z in kernel.c.
static int kernel_fg_pgrp = 0;

// ============================================================
// Ring 3 scanout ownership (SYS_FB_MAP / display-server foundation)
// ============================================================
// fb_owner_tid is the task currently holding the VBE framebuffer mapping.
// Written under fb_map_lock (acquire/release); read WITHOUT the lock from
// the kernel main loop and SYS_GET_KEY — an aligned int read is atomic and a
// one-tick-stale answer is benign (same policy as mouse_x/y, see
// docs/kernel-locking.md "Known remaining gaps"). -1 = desktop owns pixels.
static int fb_owner_tid = -1;
static spinlock_t fb_map_lock = SPINLOCK_INIT;

// Current scanout holder's tid, or -1. Used by SYS_GET_KEY to route
// keystrokes to the active compositor.
int fb_owner_current(void) { return fb_owner_tid; }

// 1 when a LIVE task holds the scanout. The kernel main loop suppresses its
// own desktop rendering while this is true — the Ring 3 compositor owns
// every pixel — and restores the desktop the first tick after it goes false.
int fb_scanout_active(void) {
    int t = fb_owner_tid;
    return (t >= 0 && t < MAX_TASKS && tasks[t].state != TASK_STATE_FREE &&
            tasks[t].state != TASK_STATE_ZOMBIE);
}

// Drop scanout ownership if `tid` held it. Called from task_cleanup (exit /
// kill) and the exec reset path: the dying/replaced address space loses the
// PAGE_DEV mappings with its page tables, so only the ownership bookkeeping
// needs clearing here. The next main-loop tick notices the handback and
// repaints the desktop.
void fb_cleanup_task(int tid) {
    if (fb_owner_tid == tid) {
        fb_owner_tid = -1;
        write_serial_string("[FBMAP] owner tid ");
        write_serial_hex((uint32_t)tid);
        write_serial_string(" gone - scanout returns to the desktop\n");
    }
}


// ============================================================
// Per-CPU runqueues (Fase 3: SMP scheduler)
// ============================================================
// Each CPU owns a runqueue of runnable tasks (READY or RUNNING). A task is
// queued iff tasks[tid].rq_cpu >= 0. RUNNING members are only ever the
// current task of that CPU; pickers/stealers only take READY members, so a
// task can never execute on two CPUs at once. Every rq_* helper below
// requires task_lock to be held.
#define MAX_CPUS 16

struct runqueue {
    int tids[MAX_TASKS];
    int count;
};

static struct runqueue rq[MAX_CPUS];

// Per-CPU load sampling (for the SysInfo app's live core bars). Every
// schedule() tick counts whether the CPU ran a real task (not task 0 / the
// pinned idle) and every 50 ticks (50 ms) publishes the percentage into
// cpu_load_pct[cid], which the SYS_GET_SYSINFO syscall reads. All updates
// happen under task_lock (schedule already holds it); reads are plain
// 32-bit loads, fine for a monitor.
static int rq_cpu_count(void);   // defined below (phantom-CPU-aware core count)

#define CPU_LOAD_WINDOW 50   // ticks per window (1 kHz -> 50 ms)
static volatile uint32_t cpu_load_pct[MAX_CPUS];
static uint32_t cpu_win_busy[MAX_CPUS];
static uint32_t cpu_win_ticks[MAX_CPUS];

uint32_t task_cpu_load(int cid) {
    if (cid < 0 || cid >= MAX_CPUS) return 0;
    return cpu_load_pct[cid];
}

int task_cpu_count(void) { return rq_cpu_count(); }

// Queue tid on cpu's runqueue (no-op if already queued).
static void rq_enqueue(int cpu, int tid) {
    if (tid < 0 || tid >= MAX_TASKS || cpu < 0 || cpu >= MAX_CPUS) return;
    if (tasks[tid].rq_cpu >= 0) return;
    // Idle tasks are pinned to the core matching their tid and NEVER leave it.
    // If one ends up enqueued elsewhere, a wake/state-corruption bug ran a
    // worker's frame under an idle task's identity (current_task[cid] says
    // idle while real code executes) — refusing the foreign enqueue stops the
    // chain before another core can iret the idle task's stale frame.
    if (tasks[tid].is_idle && cpu != tid) return;
    if (rq[cpu].count >= MAX_TASKS) cpu = 0;  // overflow fallback (cannot happen)
    rq[cpu].tids[rq[cpu].count++] = tid;
    tasks[tid].rq_cpu = cpu;
}

// Unqueue tid from whatever runqueue it currently sits on.
static void rq_remove(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return;
    int cpu = tasks[tid].rq_cpu;
    if (cpu < 0 || cpu >= MAX_CPUS) return;
    struct runqueue* q = &rq[cpu];
    for (int i = 0; i < q->count; i++) {
        if (q->tids[i] == tid) {
            q->tids[i] = q->tids[q->count - 1];
            q->count--;
            break;
        }
    }
    tasks[tid].rq_cpu = -1;
}

// Number of CPUs that actually exist (lapic ids are contiguous 0..N-1 in
// QEMU). rq_least_loaded MUST only consider real cores: scanning the whole
// 16-slot table would pick a phantom CPU (4..15) whose queue never ticks and
// park every new task on a core that does not exist.
static int rq_cpu_count(void) {
    extern uint32_t smp_cpu_count;
    int n = (int)smp_cpu_count;
    if (n < 1 || n > MAX_CPUS) n = 1;
    return n;
}

// CPU with the fewest queued tasks (tie -> lowest id). New tasks land on the
// least-loaded core so the four CPUs share the work.
static int rq_least_loaded(void) {
    int best = 0;
    int n = rq_cpu_count();
    for (int c = 1; c < n; c++) {
        if (rq[c].count < rq[best].count) best = c;
    }
    return best;
}

// Wake-path enqueue: park the task on the least-loaded CPU. The woken task
// is no longer running only once the scheduler on its home core has actually
// parked it — see task_is_current_elsewhere(): a task that blocks itself
// mid-syscall (futex/sem wait) stays current on its core until the next
// timer tick preempts it, so rq_pick/rq_steal must not resume it before
// then (a stale saved frame would be iret'd -> stack corruption).
static void rq_enqueue_wake(int tid) {
    // An idle task is never woken: it parks in its own hlt loop on its own
    // core and is always already in its own runqueue. Waking one (e.g. from
    // a corrupted futex waiter list) would enqueue it on a foreign core and
    // let that core iret its stale frame under the wrong identity.
    if (tid <= 0 || tid >= MAX_TASKS) return;
    if (tasks[tid].is_idle) return;
    rq_enqueue(rq_least_loaded(), tid);
}

// Called from irq_common_stub before iret. `esp` is the frame we are about to
// pop; it must lie within the current task's kernel stack. If not, a saved
// frame was corrupted (or the task table got clobbered) — dump the full state
// instead of silently iret'ing into garbage.
static void esp_bad(const char* tag, uint32_t cid, uint32_t cur, uint32_t esp, uint32_t top) {
    // One atomic line: 4 cores share the serial port and multi-call prints
    // interleave into garbage. Buffer must exceed the max content.
    char b[96];
    int n = 0;
    const char* s = tag;
    while (*s && n < 90) b[n++] = *s++;
    b[n++] = ' '; b[n++] = '0'; b[n++] = 'x';
    for (int i = 7; i >= 0 && n < 94; i--) { int d = (cid >> (i * 4)) & 0xF; b[n++] = d < 10 ? '0' + d : 'A' + d - 10; }
    b[n++] = ' '; b[n++] = '0'; b[n++] = 'x';
    for (int i = 7; i >= 0 && n < 94; i--) { int d = (cur >> (i * 4)) & 0xF; b[n++] = d < 10 ? '0' + d : 'A' + d - 10; }
    b[n++] = ' '; b[n++] = '0'; b[n++] = 'x';
    for (int i = 7; i >= 0 && n < 94; i--) { int d = (esp >> (i * 4)) & 0xF; b[n++] = d < 10 ? '0' + d : 'A' + d - 10; }
    b[n++] = ' '; b[n++] = '0'; b[n++] = 'x';
    for (int i = 7; i >= 0 && n < 94; i--) { int d = (top >> (i * 4)) & 0xF; b[n++] = d < 10 ? '0' + d : 'A' + d - 10; }
    b[n++] = '\n';
    extern void write_serial_buffer(const char* buf, int size);
    write_serial_buffer(b, n);
    extern void panic_finish(void);
    panic_finish();
}

void irq_esp_sanity_check(uint32_t esp) {
    int cid = get_cid();
    int cur = current_task[cid];
    // Pre-scheduler boot window: APs legitimately tick with cur == -1 while
    // parked in their own hlt loop (see scheduler_ready comment). Skip.
    if (!scheduler_ready) return;
    if (cur < 0 || cur >= MAX_TASKS) {
        esp_bad("[ESP] BAD current_task", (uint32_t)cid, (uint32_t)cur, esp, 0);
    }
    // Task 0 is the BSP's kernel main loop: it runs on the BOOT stack (the
    // scheduler fills tasks[0].esp with whatever frame preempted kernel main),
    // so its esp legitimately lives far outside kstacks[0]. Only kstack-based
    // tasks (AP idle, ring-3) must iret from inside their own kstack.
    if (cur == 0) return;
    uint32_t top = kstack_top(cur);
    if (esp > top || esp + 4 < kstack_guard(cur)) {
        esp_bad("[ESP] BAD frame", (uint32_t)cid, (uint32_t)cur, esp, top);
        extern void panic_finish(void);
        panic_finish();
    }
}

// Is tid one of the per-CPU idle tasks? Idle tasks are pinned to the core
// matching their tid and can never legitimately enter a waiter list or a
// foreign runqueue; callers that spot one treat it as corruption.
int task_is_idle(int tid) {
    if (tid <= 0 || tid >= MAX_TASKS) return 0;
    return tasks[tid].is_idle;
}

// True if `tid` is currently executing on some CPU (including cid itself).
// A self-blocking task keeps its core until the timer preempts it; resuming
// it from a peer core before that happens would run it twice from a stale
// kernel-stack frame.
static int task_is_current_elsewhere(int cid, int tid) {
    for (int c = 0; c < MAX_CPUS; c++) {
        if (c != cid && current_task[c] == tid) return 1;
    }
    return 0;
}

// True if `tid` is still the running task on ANY core (its own included). A
// self-blocked task (futex/sem/waitpid park) stays current on its owner core
// until that core's timer preempts it. While it is, its kernel-stack frame is
// LIVE — enqueueing it elsewhere would let a peer core iret a stale frame.
static int task_current_anywhere(int tid) {
    for (int c = 0; c < MAX_CPUS; c++) {
        if (current_task[c] == tid) return 1;
    }
    return 0;
}

// Pick the best READY task from cpu cid's own runqueue (priority + aging).
// Task 0 (the kernel main loop) may only run on the BSP; idle tasks only on
// the core they were pinned to. Never picks a task still current on a peer
// CPU (see task_is_current_elsewhere).
static int rq_pick(int cid) {
    struct runqueue* q = &rq[cid];
    int best = -1, best_score = -1;
    for (int i = 0; i < q->count; i++) {
        int tid = q->tids[i];
        if (tasks[tid].state != TASK_STATE_READY) continue;
        if (tid == 0 && cid != 0) continue;
        // Idle tasks are pinned to the core matching their tid. A foreign
        // core must never pick one (its saved frame only makes sense on its
        // home core — iret'ing it elsewhere runs the hlt loop under a stale
        // identity and corrupts the per-CPU task table).
        if (tasks[tid].is_idle && cid != tid) continue;
        if (task_is_current_elsewhere(cid, tid)) continue;
        tasks[tid].wait_ticks++;
        int score = tasks[tid].priority * 10 + tasks[tid].wait_ticks;
        if (score > best_score) { best_score = score; best = tid; }
    }
    if (best >= 0) tasks[best].wait_ticks = 0;
    return best;
}

// Steal a READY task from a peer CPU's runqueue (migration). Never steals
// task 0 or per-CPU idle tasks.
static int rq_steal(int cid) {
    int best = -1, best_score = -1, best_src = -1;
    // Only real cores: phantom CPUs (4..15) have empty queues, but scanning
    // them is wasted work — and rq_cpu_count() is the same source of truth
    // rq_least_loaded() uses, so both agree on what "a CPU" is.
    int n = rq_cpu_count();
    for (int c = 0; c < n; c++) {
        if (c == cid) continue;
        struct runqueue* q = &rq[c];
        for (int i = 0; i < q->count; i++) {
            int tid = q->tids[i];
            if (tid == 0 || tasks[tid].is_idle) continue;
            if (tasks[tid].state != TASK_STATE_READY) continue;
            if (task_is_current_elsewhere(cid, tid)) continue;
            int score = tasks[tid].priority * 10 + tasks[tid].wait_ticks;
            if (score > best_score) { best_score = score; best = tid; best_src = c; }
        }
    }
    if (best < 0) return -1;
    struct runqueue* src = &rq[best_src];
    for (int i = 0; i < src->count; i++) {
        if (src->tids[i] == best) {
            src->tids[i] = src->tids[src->count - 1];
            src->count--;
            break;
        }
    }
    tasks[best].rq_cpu = cid;
    rq[cid].tids[rq[cid].count++] = best;
    return best;
}

// Ring 0 entry for a per-CPU idle task: halt until the next timer IRQ wakes
// the scheduler. Mirrors task_dead_park but as a schedulable idle.
static void ap_idle(void) {
    for (;;) __asm__ volatile("hlt");
}

// Create a Ring 0 idle task pinned to `cpu` (its lapic id). Created for every
// Application Processor during init_tasking() so a core with an empty
// runqueue can park instead of stealing the BSP's kernel main loop. The BSP's
// idle is task 0 itself.
static int create_idle_task(int cpu) {
    __asm__ volatile("cli");
    spin_lock(&task_lock);
    int tid = -1;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_STATE_FREE) { tid = i; break; }
    }
    if (tid < 0) {
        spin_unlock(&task_lock);
        __asm__ volatile("sti");
        return -1;
    }
    tasks[tid].ring = 0;
    tasks[tid].priority = PRIORITY_BACKGROUND;
    tasks[tid].sleep_ticks = 0;
    tasks[tid].wait_ticks = 0;
    tasks[tid].page_dir = tasks[0].page_dir;
    tasks[tid].parent = 0;
    tasks[tid].exit_code = 0;
    tasks[tid].pgrp = 0;
    tasks[tid].session = 0;
    tasks[tid].waiting = 0;
    tasks[tid].pending_signals = 0;
    tasks[tid].blocked_signals = 0;
    tasks[tid].sig_restart_ticks = 0;
    tasks[tid].sig_frame_esp = 0;
    tasks[tid].zombie_since = 0;
    tasks[tid].shm_bits = 0;
    for (int j = 0; j < 16; j++) tasks[tid].fd_table[j] = -1;
    for (int j = 0; j < MMAP_MAX_REGIONS; j++) tasks[tid].mmap_regions[j].base = 0;
    for (int j = 0; j < SIG_MAX; j++) { tasks[tid].signal_handlers[j] = NULL; tasks[tid].sig_masks[j] = 0; tasks[tid].sig_flags[j] = 0; }
    tasks[tid].is_idle = 1;
    tasks[tid].rq_cpu = -1;
    rlimit_set_defaults(tid);
    fpu_task_init(tasks[tid].fxsave);

    // Ring 0 interrupt frame -> ap_idle() (same layout as create_task).
    uint32_t* stack = (uint32_t*)kstack_top(tid);
    *(--stack) = 0x202;      // EFLAGS (IF=1)
    *(--stack) = 0x08;       // CS (kernel code)
    *(--stack) = (uint32_t)(uintptr_t)&ap_idle;
    *(--stack) = 0;          // err_code
    *(--stack) = 0;          // int_no
    *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; *(--stack) = 0;
    *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; *(--stack) = 0;
    *(--stack) = 0x10;       // DS (kernel data)
    *(--stack) = 0x10;       // FS (kernel data — no TLS)
    *(--stack) = 0x10;       // GS (kernel data — no TLS)
    tasks[tid].esp = (uint32_t)stack;
    tasks[tid].stack_watermark = 0;

    num_tasks++;
    tasks[tid].state = TASK_STATE_READY;
    rq_enqueue(cpu, tid);   // pinned to its own core

    spin_unlock(&task_lock);
    __asm__ volatile("sti");
    return tid;
}

void init_tasking() {
    int cid = get_cid();
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_STATE_FREE;
        tasks[i].ring = 0;
        tasks[i].priority = PRIORITY_INTERACTIVE;
        tasks[i].sleep_ticks = 0;
        tasks[i].wait_ticks = 0;
        tasks[i].rq_cpu = -1;
        tasks[i].is_idle = 0;
        tasks[i].page_dir = 0;
        tasks[i].heap_ptr = 0x08000000;
        tasks[i].launch_arg[0] = '\0';
        tasks[i].trusted_shell = 0;
        tasks[i].current_dir = 0;
        tasks[i].parent = 0;
        tasks[i].exit_code = 0;
        tasks[i].pgrp = 0;
        tasks[i].session = 0;
        tasks[i].waiting = 0;
        tasks[i].pending_signals = 0;
        tasks[i].blocked_signals = 0;
        tasks[i].sig_frame_esp = 0;
        tasks[i].zombie_since = 0;
        for (int j = 0; j < SIG_MAX; j++) { tasks[i].signal_handlers[j] = NULL; tasks[i].sig_masks[j] = 0; tasks[i].sig_flags[j] = 0; }
        for (int j = 0; j < 16; j++) tasks[i].fd_table[j] = -1;
        for (int j = 0; j < MMAP_MAX_REGIONS; j++) tasks[i].mmap_regions[j].base = 0;
        rlimit_set_defaults(i);
        fpu_task_init(tasks[i].fxsave);
    }
    tasks[0].state = TASK_STATE_RUNNING;
    tasks[0].ring = 0;
    tasks[0].priority = PRIORITY_INTERACTIVE;
    tasks[0].wait_ticks = 0;
    tasks[0].esp = 0; // Will be filled by scheduler on first preemption
    tasks[0].parent = 0;
    tasks[0].shm_bits = 0;
    rlimit_set_defaults(0);
    
    // Save boot CR3 to task 0
    uint32_t boot_cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(boot_cr3));
    tasks[0].page_dir = boot_cr3;
    
    current_task[cid] = 0;
    num_tasks = 1;
    task_set_launch_arg(0, "idle");

    // Task 0 is the BSP's idle (kernel main loop): pin it to runqueue 0.
    tasks[0].rq_cpu = 0;
    tasks[0].is_idle = 0;
    rq[0].tids[0] = 0;
    rq[0].count = 1;

    // Create a pinned idle task for every Application Processor so the
    // per-CPU scheduler has somewhere to park when a core has nothing to run.
    // APs are already awake (smp_init() runs before init_tasking()) and pick
    // their idle task up on the next timer IRQ.
    for (uint32_t i = 0; i < smp_cpu_count; i++) {
        uint8_t lapic_id = smp_lapic_ids[i];
        if (lapic_id == (smp_bsp_lapic_id & 15)) continue;
        create_idle_task(lapic_id & 15);
    }

    // Unmap every task's kernel-stack guard page in the boot page directory.
    // This must run after paging_init() and before any user address space is
    // created: vmm_create_address_space() clones the boot directory's PTEs
    // verbatim, so every later (and forked) address space inherits the holes.
    task_install_stack_guards(tasks[0].page_dir);
    write_serial_string("[K] stack guards installed\n");

    // From here on, every core must have a current task whenever it iret's.
    // The iret-ESP sanity check (irq_esp_sanity_check) is armed from now on.
    scheduler_ready = 1;
}

uint32_t tasks_get_boot_cr3(void) {
    return tasks[0].page_dir;
}

// ============================================================
// Kernel stack guard pages
// ============================================================
// Each task's 16KB kernel stack sits above a 4KB guard page that is unmapped
// in every page directory. task_is_stack_guard() lets the #PF handler
// (idt.c) tell a stack overflow from any other fault and panic with a clear
// message instead of corrupting memory silently.
int task_is_stack_guard(uint32_t addr) {
    uint32_t base = (uint32_t)(uintptr_t)&kstacks[0][0];
    if (addr < base) return 0;
    uint32_t off = addr - base;
    if (off >= (uint32_t)MAX_TASKS * KERNEL_STACK_SLOT) return 0;
    return (off % KERNEL_STACK_SLOT) < KERNEL_STACK_GUARD;
}

uint32_t task_stack_top(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return 0;
    return kstack_top(tid);
}

// Clear the guard-page PTE of every task in the given page directory. Called
// once for the boot directory at init; cloned directories already inherit the
// cleared entries (see init_tasking). invlpg keeps the current TLB coherent
// if this runs on the active directory.
void task_install_stack_guards(uint32_t page_dir) {
    pte_t* pdpt = (pte_t*)(uintptr_t)page_dir;
    for (int tid = 0; tid < MAX_TASKS; tid++) {
        uint32_t va = kstack_guard(tid);
        uint32_t pi = pdpt_index(va);
        uint32_t di = pd_index(va);
        uint32_t ti = pt_index(va);
        if (!(pdpt[pi] & PAGE_PRESENT)) continue;
        pte_t* pd = (pte_t*)(uintptr_t)(uint32_t)(pdpt[pi] & PTE_ADDR_MASK);
        if (!(pd[di] & PAGE_PRESENT)) continue;
        pte_t* pt = (pte_t*)(uintptr_t)(uint32_t)(pd[di] & PTE_ADDR_MASK);
        if (pt[ti] & PAGE_PRESENT) {
            pt[ti] = 0;
            __asm__ __volatile__("invlpg (%0)" : : "r"(va));
        }
    }
}

// Create a Ring 0 (kernel) task
int create_task(void (*entry)()) {
    int cid = get_cid();
    // CRITICAL: Disable interrupts BEFORE taking the lock to prevent scheduler deadlocks!
    __asm__ volatile("cli");
    spin_lock(&task_lock);
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == 0) {
            
            tasks[i].ring = 0;
            tasks[i].priority = PRIORITY_INTERACTIVE;
            tasks[i].wait_ticks = 0;
            tasks[i].current_dir = (current_task[cid] >= 0) ? tasks[current_task[cid]].current_dir : 0;
            tasks[i].parent = 0;
            tasks[i].exit_code = 0;
            tasks[i].pgrp = 0;
            tasks[i].session = 0;
            tasks[i].waiting = 0;
            tasks[i].pending_signals = 0;
            tasks[i].blocked_signals = 0;
            tasks[i].sig_frame_esp = 0;
            tasks[i].zombie_since = 0;
            tasks[i].shm_bits = 0;
            for (int j = 0; j < SIG_MAX; j++) { tasks[i].signal_handlers[j] = NULL; tasks[i].sig_masks[j] = 0; tasks[i].sig_flags[j] = 0; }
            task_set_launch_arg(i, "sys_kernel");
            for (int j = 0; j < 16; j++) tasks[i].fd_table[j] = -1;
            fpu_task_init(tasks[i].fxsave);
            uint32_t* stack = (uint32_t*)kstack_top(i);
            
            // Ring 0 interrupt frame
            *(--stack) = 0x202;      // EFLAGS (IF=1)
            *(--stack) = 0x08;       // CS (kernel code)
            *(--stack) = (uint32_t)(uintptr_t)entry; // EIP
            *(--stack) = 0;          // err_code
            *(--stack) = 0;          // int_no
            *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; // eax,ecx,edx,ebx
            *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; // esp,ebp,esi,edi
            *(--stack) = 0x10;       // DS (kernel data)
            *(--stack) = 0x10;       // FS (kernel data — no TLS)
            *(--stack) = 0x10;       // GS (kernel data — no TLS)
            
            tasks[i].esp = (uint32_t)stack;
            tasks[i].stack_watermark = 0;
            tasks[i].rq_cpu = -1;
            tasks[i].is_idle = 0;
            tasks[i].tls_base = 0;
            rlimit_set_defaults(i);
            num_tasks++;
            
            // Set state LAST — only now is it safe for the scheduler
            tasks[i].state = 2;
            rq_enqueue(rq_least_loaded(), i);
            
            spin_unlock(&task_lock);
            __asm__ volatile("sti");
            return i;
        }
    }
    spin_unlock(&task_lock);
    __asm__ volatile("sti");
    return -1;
}

// Create a Ring 3 (user) task.
//
// UNSUPPORTED — always fails. This used to hand a Ring 3 task the kernel's own
// page directory and a stack inside task_t, which only worked because the
// identity map was user-accessible. Now that it is not, a Ring 3 task needs a
// private address space with its code mapped user-readable and a stack from
// vmm_setup_user_stack(). Use load_mct_app() (which builds exactly that) or
// thread_create() with a page directory from vmm_create_address_space().
//
// It has no callers in the tree; kept so the declaration in task.h stays valid.
int create_user_task(void (*entry)()) {
    (void)entry;
    write_serial_string("[TASK] create_user_task: unsupported, use load_mct_app()\n");
    return -1;
}

extern void tss_set_kernel_stack(uint32_t stack);

// Shared cleanup for task termination (exit or kill)
static void task_cleanup(int tid) {
    if (tid <= 0 || tid >= MAX_TASKS) return;

    // 0. Drop this task from every sem/futex waiter list (v38.55). Dead
    //    waiters used to linger; after slot recycle a wake flipped the
    //    UNRELATED new owner of the tid. Runs first, before anything can
    //    observe the dying task's state transitions.
    extern void sync_task_cleanup_defer(int tid);
    // v38.55: DEFERRED — see sync.c. Signal teardown paths reach us with
    // task_lock held; an eager sync_lock take here inverts the documented
    // sync_lock > task_lock order and deadlocks two cores with IF=0. The
    // BSP main loop drains the pending bit within one iteration.
    sync_task_cleanup_defer(tid);

    // 1. Clean up windows owned by this task
    extern void wm_cleanup_task(int tid);
    wm_cleanup_task(tid);

    // 1a. If this task held the Ring 3 scanout (SYS_FB_MAP), drop ownership
    //     so the kernel main loop restores the desktop next tick.
    fb_cleanup_task(tid);

    // 1b. If this was the terminal's foreground app, release the terminal
    //     (so a later Ctrl+C is a no-op and the terminal is free again).
    extern int term_app_running;
    extern int term_app_task_id;
    if (term_app_running && term_app_task_id == tid) {
        term_app_running = 0;
        term_app_task_id = -1;
        // Drop queued keys for the dead app (single-consumer keyboard v38.9)
        extern void term_app_key_clear(void);
        term_app_key_clear();
        // The foreground group died: give the terminal back to the shell's
        // group (the task that spawned the app — the app itself has its own
        // pgrp == its own tid, so reading the app's pgrp would hand the
        // terminal to a corpse), so Ctrl+C/Z keep working and new `run`
        // commands are not treated as background.
        int app_parent = tasks[tid].parent;
        int shell_pgrp = (app_parent > 0 && app_parent < MAX_TASKS) ? tasks[app_parent].pgrp : 0;
        if (kernel_fg_pgrp == tasks[tid].pgrp || kernel_fg_pgrp == tid) {
            if (shell_pgrp > 0) kernel_fg_pgrp = shell_pgrp;
            else kernel_fg_pgrp = 0;
            write_serial_string("[JOBS] foreground TID ");
            write_serial_hex(tid);
            write_serial_string(" exited — fg pgrp reset to ");
            write_serial_hex(kernel_fg_pgrp);
            write_serial_string("\n");
        }
        write_serial_string("[JOBS] foreground app TID ");
        write_serial_hex(tid);
        write_serial_string(" exited — terminal released\n");
    }

    // 2. Release all fds this task holds (global fd slots + pipes). Without
    //    this, exit/kill leaked slots and left pipe readers blocked forever.
    extern void task_close_all_fds(int tid);
    task_close_all_fds(tid);
    
    // 3. Release shm segments this task still had attached (task may have
    //    died without calling shmdt). This must run BEFORE the address space
    //    is freed so segment accounting sees the mapping still in place.
    if (tasks[tid].shm_bits != 0) {
        extern void shm_task_exit(int tid);
        shm_task_exit(tid);
    }

    // 3b. Drop file-backed mmap dirty bitmaps (no writeback on death: a dead
    //     task's changes are lost, matching anonymous discard semantics).
    mmap_free_dirty_bitmaps(tid);

    // 4. Free address space (if it's not the kernel's) — but only when this is
    //    the LAST task still using it. Sibling threads of one process share the
    //    same page_dir; freeing it when one of them exits would yank the memory
    //    out from under the others.
    if (tasks[tid].page_dir != 0 && tasks[tid].page_dir != tasks[0].page_dir) {
        int sharers = 0;
        for (int k = 1; k < MAX_TASKS; k++) {
            if (k != tid && tasks[k].state != TASK_STATE_FREE &&
                tasks[k].page_dir == tasks[tid].page_dir) sharers++;
        }
        if (sharers == 0) {
            // SMP safety: the dying task may still be mid-flight on ANOTHER
            // core with this page directory loaded in CR3 (it runs for up to
            // one timer tick after being killed). Freeing the tables under it
            // would corrupt that core's page walks. If any CPU's CR3 is this
            // directory, skip the free — a bounded leak, but never a crash.
            // (The current CPU's own CR3 is handled by switching to task 0's.)
            uint32_t active_cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(active_cr3));
            int busy_elsewhere = 0;
            for (int c = 0; c < MAX_CPUS; c++) {
                if (current_task[c] == tid) busy_elsewhere = 1;
            }
            if (!busy_elsewhere &&
                (active_cr3 & 0xFFFFF000) == (tasks[tid].page_dir & 0xFFFFF000)) {
                extern void vmm_switch_page_dir(uint32_t);
                vmm_switch_page_dir(tasks[0].page_dir);
            }
            if (!busy_elsewhere) {
                extern void vmm_free_address_space(uint32_t);
                vmm_free_address_space(tasks[tid].page_dir);
            }
        }
        tasks[tid].page_dir = 0;
    }
}

// Per-CPU scheduler entry — called from the timer IRQ on every core.
//
// Lock discipline: schedule() runs with IF=0 (interrupt gate). It may take
// task_lock because every other holder disables interrupts before acquiring
// it: a timer IRQ can therefore never fire while this CPU holds the lock (no
// self-deadlock), and a cross-CPU holder is never preemptible either.
//
// NOTE: net_poll() is deliberately NOT called here. The BSP main loop
// (kernel.c) drains the NIC once per loop iteration; polling it from the
// 1000Hz timer IRQ as well made RX/TX state mutate from two contexts at
// once (double-processing, descriptor desync).
uint32_t schedule(uint32_t esp) {
    int cid = get_cid();
    int cur = current_task[cid];

    spin_lock(&task_lock);

    // 1. Sleep upkeep — BSP only. One global decrement per tick keeps sleep
    //    durations wall-clock correct now that all four CPUs tick at 1kHz.
    if (cid == 0) {
        for (int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i].state == TASK_STATE_SLEEP) {
                if (tasks[i].sleep_ticks > 0) {
                    tasks[i].sleep_ticks--;
                }
                if (tasks[i].sleep_ticks <= 0) {
                    tasks[i].state = TASK_STATE_READY;
                    rq_enqueue_wake(i);
                }
            }
        }
    }

    // Fast path: single-task system (kernel only), nothing to pick.
    if (num_tasks <= 1 && cur >= 0) {
        spin_unlock(&task_lock);
        return esp;
    }

    // 2. Save the preempted frame; RUNNING -> READY (stays on our runqueue).
    //    The esp we just got is the deepest point of this tick (timer
    //    interrupts land anywhere in the call stack), so fold it into the
    //    per-task stack watermark — /proc/tasks shows how close to the
    //    guard page each task has ever come.
    // HARD INVARIANT: an idle task can only ever be current on its own core.
    // If the scheduler is about to save a frame for an idle task on a foreign
    // core, a previous commit already iret'd a stale frame under the wrong
    // identity — halt instead of compounding the corruption.
    if (cur > 0 && cur < MAX_TASKS && tasks[cur].is_idle && cid != cur) {
        extern void panic_finish(void);
        extern void write_serial_string(const char*);
        extern void write_serial_hex(uint32_t);
        write_serial_string("[SCHED] IDLE ON FOREIGN CORE cid=");
        write_serial_hex(cid);
        write_serial_string(" tid=");
        write_serial_hex(cur);
        write_serial_string("\n");
        panic_finish();
    }

    if (cur >= 0) {
        tasks[cur].esp = esp;
        if (esp >= kstack_guard(cur) + KERNEL_STACK_GUARD && esp <= kstack_top(cur)) {
            uint32_t used = kstack_top(cur) - esp;
            if (used > tasks[cur].stack_watermark) tasks[cur].stack_watermark = used;
        }
        if (tasks[cur].state == TASK_STATE_RUNNING) {
            tasks[cur].state = TASK_STATE_READY;
        }
        // A task woken while still running (futex/sem wake flipped it READY
        // but its owner core hadn't preempted it yet) is off-queue. If we now
        // switch AWAY from it, it would be stranded (READY, in no runqueue) —
        // put it back on OUR queue so it stays pickable from a fresh frame.
        if (tasks[cur].state == TASK_STATE_READY && tasks[cur].rq_cpu < 0) {
            rq_enqueue(cid, cur);
        }
    }

    // 3. Pick from our own runqueue; steal from a peer if empty; keep the
    //    current task if it is still runnable; otherwise idle out (on the BSP
    //    that is the kernel main loop, on APs their own pinned idle task).
    int next = rq_pick(cid);
    if (next < 0) next = rq_steal(cid);
    if (next < 0) {
        if (cur >= 0 &&
            (tasks[cur].state == TASK_STATE_READY ||
             tasks[cur].state == TASK_STATE_RUNNING)) {
            next = cur;
        } else {
            // Nothing runnable to switch to: we iret right back into the
            // CURRENT frame. If cur is a task that blocked itself mid-syscall
            // (futex/sem/waitpid park loop), that frame is ITS — it keeps
            // executing, so it must stay current on this CPU. Clearing
            // current_task[cid] here would lose the task's identity: later
            // get_current_task() returns garbage (parking a bogus tid/pd into
            // the futex table) and the busy-elsewhere guard in rq_pick/steal
            // no longer protects it -> another CPU iret's its stale frame =
            // double-run, kernel-stack corruption. (cur == -1 already means
            // this CPU was parked in its own idle loop.)
            spin_unlock(&task_lock);
            return esp;
        }
    }

    // 3b. Per-CPU load sample: a tick counts as busy only when the picked
    //     task is real work — never task 0 (kernel main loop / BSP idle) or a
    //     pinned per-CPU idle task.
    if (cid < MAX_CPUS) {
        if (next != 0 && !tasks[next].is_idle) cpu_win_busy[cid]++;
        if (++cpu_win_ticks[cid] >= CPU_LOAD_WINDOW) {
            cpu_load_pct[cid] = (cpu_win_busy[cid] * 100) / cpu_win_ticks[cid];
            cpu_win_busy[cid] = 0;
            cpu_win_ticks[cid] = 0;
        }
    }

    // 4. Commit the switch. The picked task must never be an idle task on a
    //    foreign core (the rq_pick/steal guards above make it impossible, but
    //    a corrupted runqueue could still hand one back — refuse to commit).
    if (next > 0 && next < MAX_TASKS && tasks[next].is_idle && cid != next) {
        spin_unlock(&task_lock);
        return esp;
    }
    tasks[next].state = TASK_STATE_RUNNING;
    current_task[cid] = next;

    // CRITICAL: TSS.esp0 must point at the TOP of the next task's kernel
    // stack. A Ring 3 interrupt on THIS CPU pushes its frame there; the TSS is
    // per-CPU, so a task migrating between cores still gets its own stack top.
    tss_set_kernel_stack(kstack_top(next));

    // Switch page directory if different.
    extern void vmm_switch_page_dir(uint32_t);
    if (tasks[next].page_dir != 0) {
        vmm_switch_page_dir(tasks[next].page_dir);
    } else {
        vmm_switch_page_dir(tasks[0].page_dir);
    }

    // 5. Deliver pending signals to the task about to return to Ring 3. Its
    //    CR3 is already active, so the handler frame can be written straight
    //    into its user stack. If a default-action signal terminates the task,
    //    re-pick another ready task instead of iret'ing a dead frame.
    if (tasks[next].ring == 3) {
        while (tasks[next].state == TASK_STATE_RUNNING &&
               tasks[next].pending_signals != 0) {
            if (!task_deliver_signals((void*)tasks[next].esp)) break;
            if (tasks[next].state == TASK_STATE_ZOMBIE) {
                // The signal killed it: choose another task (terminate_task
                // already removed it from the runqueue).
                next = rq_pick(cid);
                if (next < 0) next = rq_steal(cid);
                if (next < 0) {
                    current_task[cid] = -1;
                    spin_unlock(&task_lock);
                    return esp;
                }
                tasks[next].state = TASK_STATE_RUNNING;
                current_task[cid] = next;
                tss_set_kernel_stack(kstack_top(next));
                if (tasks[next].page_dir != 0) {
                    vmm_switch_page_dir(tasks[next].page_dir);
                } else {
                    vmm_switch_page_dir(tasks[0].page_dir);
                }
            }
        }
    }

    // Eager FPU switch (v38.41): swap the full x87+MMX+SSE image between
    // the outgoing and incoming tasks. IF=0 under task_lock here, so no
    // other context can observe the FPU mid-swap. `cur` still names the
    // task this CPU entered with (current_task[cid] was overwritten
    // above); if the final pick — possibly after a signal-kill repick —
    // landed back on it, the hardware already holds its live state and
    // the swap is skipped. fxsave does not disturb the hardware state, so
    // the early-return paths above never need a matching restore.
    if (next != cur) {
        if (cur >= 0 && cur < MAX_TASKS) fpu_save(tasks[cur].fxsave);
        fpu_restore(tasks[next].fxsave);
    }

    uint32_t ret = tasks[next].esp;
    spin_unlock(&task_lock);
    return ret;
}

// Exported lock helpers for code paths outside task.c that touch the task
// table (signal delivery on the syscall-return path in syscall.c). Callers
// MUST disable interrupts before task_lock_acquire() (same cli-first
// discipline as every internal user) or a timer IRQ can self-deadlock inside
// schedule()'s own spin_lock(&task_lock).
void task_lock_acquire(void) { spin_lock(&task_lock); }
void task_lock_release(void) { spin_unlock(&task_lock); }

// Terminate a task (exit or default-action signal). Cleans up its resources,
// reparents its children to the kernel task, turns it into a ZOMBIE when a
// live parent exists (so waitpid can reap it and the exit code survives), and
// wakes the parent if it is blocked in waitpid().
//
// Called with interrupts disabled by all callers (exit path holds cli;
// task_signal holds its own cli/sti pair).
// Requires task_lock held by the caller (schedule delivery path, task_signal,
// task_exit_with_code — all lock before calling). Removes the task from its
// runqueue so the per-CPU scheduler never picks a dying task.
static void terminate_task(int tid, int code) {
    if (tid <= 0 || tid >= MAX_TASKS) return;
    if (tasks[tid].state == TASK_STATE_FREE || tasks[tid].state == TASK_STATE_ZOMBIE) return;

    rq_remove(tid);
    task_cleanup(tid);

    // Reparent this task's children to the kernel task. Zombie children get
    // reaped immediately; live ones keep running and are reaped when they
    // exit (parent == 0 never waits).
    for (int k = 1; k < MAX_TASKS; k++) {
        if (k != tid && tasks[k].state != TASK_STATE_FREE && tasks[k].parent == tid) {
            if (tasks[k].state == TASK_STATE_ZOMBIE) {
                tasks[k].state = TASK_STATE_FREE;
                num_tasks--;
                gdt_tls_clear(k);   // slot freed: drop its TLS descriptors
            } else {
                tasks[k].parent = 0;
            }
        }
    }

    int p = tasks[tid].parent;
    if (p >= 1 && p < MAX_TASKS && p != tid && tasks[p].state != TASK_STATE_FREE &&
        tasks[p].state != TASK_STATE_ZOMBIE) {
        tasks[tid].state = TASK_STATE_ZOMBIE;
        tasks[tid].exit_code = code & 0xFF;
        tasks[tid].zombie_since = get_ticks();
        // Wake the parent if it is parked in waitpid()
        if (tasks[p].waiting != 0) {
            tasks[p].waiting = 0;
            tasks[p].state = TASK_STATE_READY;
            rq_enqueue_wake(p);
        }
        // SIGCHLD is pending for the parent (default action: ignored)
        tasks[p].pending_signals |= (1u << SIGCHLD);
    } else {
        // No one will ever reap it: release the slot immediately.
        tasks[tid].state = TASK_STATE_FREE;
        num_tasks--;
        gdt_tls_clear(tid);   // slot freed: drop its TLS descriptors
    }
}

// Exit the current task with an exit status (SYS_EXIT / crash / bg child).
// The task becomes a ZOMBIE until its parent waitpids it; the address space
// and fds are released immediately so memory returns to the system promptly.
void task_exit_with_code(int code) {
    int cid = get_cid();
    int tid = current_task[cid];
    if (tid <= 0) return; // never kill the kernel task

    __asm__ volatile("cli");
    spin_lock(&task_lock);
    terminate_task(tid, code);
    spin_unlock(&task_lock);
    __asm__ volatile("sti");

    // Never returns. The scheduler will not reschedule a ZOMBIE.
    for (;;) __asm__ volatile("hlt");
}

// Terminate the current task — called from SYS_EXIT syscall
void task_exit(void) {
    task_exit_with_code(0);
}

int task_get_ppid(void) {
    int cid = get_cid();
    if (current_task[cid] < 0 || current_task[cid] >= MAX_TASKS) return 0;
    return tasks[current_task[cid]].parent;
}

int get_current_task(void) {
    int cid = get_cid();
    return current_task[cid];
}

int get_current_dir(void) {
    int cid = get_cid();
    if (current_task[cid] >= 0 && current_task[cid] < MAX_TASKS) {
        return tasks[current_task[cid]].current_dir;
    }
    return boot_current_dir;
}

void set_current_dir(int dir) {
    int cid = get_cid();
    if (current_task[cid] >= 0 && current_task[cid] < MAX_TASKS) {
        tasks[current_task[cid]].current_dir = dir;
    } else {
        boot_current_dir = dir;
    }
}

// === NEW: Thread creation with priority + page_dir ===
// Create a task that shares the caller's address space, with an optional
// explicit child stack (0 = the default per-slot 64KB user stack) and an
// optional TLS base (0 = no TLS). When tls_base is nonzero the task's FS/GS
// GDT descriptors (all CPUs) are pointed at it and its saved frame carries
// the task's private TLS selectors, so Ring 3 %gs accesses resolve to the
// thread control block. The caller must have interrupts disabled.
int thread_create_ex(void (*entry)(), int priority, uint32_t page_dir,
                     uint32_t child_stack, uint32_t tls_base) {
    int cid = get_cid();
    // Clamp priority like task_set_priority does: an out-of-range value from
    // Ring 3 (e.g. INT_MAX) would overflow the scheduler's priority*10 + aging
    // score and let the thread monopolize the CPU.
    if (priority < PRIORITY_BACKGROUND || priority > PRIORITY_REALTIME) priority = PRIORITY_INTERACTIVE;

    __asm__ volatile("cli");
    spin_lock(&task_lock);

    // RLIMIT_NPROC counts threads too (POSIX: RLIMIT_NPROC bounds the number
    // of processes OR threads of a user). A thread shares its creator's uid
    // and address space, so the same per-uid count applies.
    int parent_tid = (current_task[cid] >= 0) ? current_task[cid] : -1;
    if (parent_tid > 0 && rlimit_nproc_allows(parent_tid) != 0) {
        spin_unlock(&task_lock);
        __asm__ volatile("sti");
        return -1;
    }

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_STATE_FREE) {
            
            uint32_t user_esp;
            if (child_stack != 0) {
                // Caller-supplied stack (clone semantics): the VA is already
                // mapped in the shared address space; just start there.
                user_esp = child_stack;
            } else {
                // Each task slot owns a private 64KB stack region below
                // USER_STACK_TOP in this address space: USER_STACK_TOP -
                // (i+1)*USER_STACK_SIZE. The stack is mapped while holding
                // task_lock (slot i is only claimed after a successful map),
                // so two tasks can never pick the same VA — the old fixed
                // USER_STACK_BOTTOM..USER_STACK_TOP mapping let sibling
                // threads overwrite each other's stacks.
                // Lock order task_lock -> vmm_lock (via frame_alloc) is safe:
                // no path ever acquires them in reverse.
                uint32_t stack_top = USER_STACK_TOP - ((uint32_t)(i + 1) * USER_STACK_SIZE);
                user_esp = vmm_setup_user_stack(page_dir, stack_top);
                if (user_esp == 0) {
                    write_serial_string("[TASK] thread_create_ex: could not map user stack\n");
                    spin_unlock(&task_lock);
                    return -1;
                }
            }

            tasks[i].ring = 3;  // Threads are user tasks by default
            tasks[i].heap_ptr = 0x08000000;
            tasks[i].current_dir = (current_task[cid] >= 0) ? tasks[current_task[cid]].current_dir : 0;
            tasks[i].priority = priority;
            tasks[i].page_dir = page_dir;
            tasks[i].sleep_ticks = 0;
            tasks[i].wait_ticks = 0;
            tasks[i].parent = (current_task[cid] >= 0) ? current_task[cid] : 0;
            tasks[i].exit_code = 0;
            tasks[i].pgrp = (current_task[cid] >= 0) ? tasks[current_task[cid]].pgrp : 0;
            tasks[i].session = (current_task[cid] >= 0) ? tasks[current_task[cid]].session : 0;
            // Inherit the caller's Unix identity (uid/gid). load_mct_app() then
            // raises Ring 3 apps to USER_UID; fork/exec/thread-create inherit
            // from their parent either way (v38.23 ownership model).
            tasks[i].uid = (current_task[cid] >= 0) ? tasks[current_task[cid]].uid : ROOT_UID;
            tasks[i].gid = (current_task[cid] >= 0) ? tasks[current_task[cid]].gid : 0;
            // A thread inherits its creator's resource limits (POSIX: rlimits
            // are per-process and shared by all threads of that process).
            if (current_task[cid] >= 0 && current_task[cid] < MAX_TASKS) {
                memcpy(tasks[i].rlimits, tasks[current_task[cid]].rlimits, sizeof(tasks[i].rlimits));
            } else {
                rlimit_set_defaults(i);
            }
            tasks[i].waiting = 0;
            tasks[i].pending_signals = 0;
            tasks[i].blocked_signals = 0;
            tasks[i].sig_restart_ticks = 0;
            tasks[i].sig_frame_esp = 0;
            tasks[i].zombie_since = 0;
            tasks[i].shm_bits = 0;
            mmap_free_dirty_bitmaps(i);  // reused slot: drop any stale bitmaps
            fpu_task_init(tasks[i].fxsave);  // fresh task: clean FPU image
            for (int j = 0; j < MMAP_MAX_REGIONS; j++) tasks[i].mmap_regions[j].base = 0;
            for (int j = 0; j < SIG_MAX; j++) tasks[i].signal_handlers[j] = NULL;
            // Inherit the caller's fd table (POSIX spawn semantics). The shell's
            // pipeline spawns a child that dup2()s a pipe/file onto fd 0/1 and
            // then loads an app: without this copy the new task would have an
            // empty fd table and the pipe/file would never be used.
            for (int j = 0; j < 16; j++) tasks[i].fd_table[j] = -1;
            if (current_task[cid] >= 0 && current_task[cid] < MAX_TASKS &&
                tasks[current_task[cid]].state != TASK_STATE_FREE) {
                extern global_fd_t global_fds[];
                for (int j = 0; j < 16; j++) {
                    int gfd = tasks[current_task[cid]].fd_table[j];
                    if (gfd >= 0 && gfd < MAX_GLOBAL_FDS && global_fds[gfd].in_use) {
                        tasks[i].fd_table[j] = gfd;
                        global_fds[gfd].ref_count++;
                    }
                }
            }
            
            tasks[i].tls_base = tls_base;
            if (tls_base != 0) gdt_tls_update(i, tls_base);

            uint32_t* stack = (uint32_t*)kstack_top(i);

            // Ring 3 interrupt frame. A thread with TLS starts with its own
            // FS/GS selectors; without one, plain user data (0x23).
            uint16_t tsel = (tls_base != 0) ? tls_gs_sel(i) : 0x23;
            *(--stack) = 0x23;       // SS
            *(--stack) = user_esp;   // ESP
            *(--stack) = 0x202;      // EFLAGS
            *(--stack) = 0x1B;       // CS
            *(--stack) = (uint32_t)(uintptr_t)entry; // EIP
            *(--stack) = 0;          // err_code
            *(--stack) = 0;          // int_no
            *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; *(--stack) = 0;
            *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; *(--stack) = 0;
            *(--stack) = 0x23;       // DS
            *(--stack) = tsel;       // FS
            *(--stack) = tsel;       // GS
            
            tasks[i].esp = (uint32_t)stack;
            tasks[i].stack_watermark = 0;
            tasks[i].rq_cpu = -1;
            tasks[i].is_idle = 0;
            num_tasks++;
            tasks[i].state = TASK_STATE_READY;
            rq_enqueue(rq_least_loaded(), i);
            
            // NOTE: Interrupts stay DISABLED. Caller must call sti after
            // finishing any post-create setup
            spin_unlock(&task_lock);
            return i;
        }
    }
    spin_unlock(&task_lock);
    return -1;
}

// Backward-compatible wrapper: no explicit stack, no TLS.
int thread_create(void (*entry)(), int priority, uint32_t page_dir) {
    return thread_create_ex(entry, priority, page_dir, 0, 0);
}

// Sleep the current task for N timer ticks
void task_sleep(int ticks) {
    int cid = get_cid();
    if (current_task[cid] < 0 || ticks <= 0) return;
    
    __asm__ volatile("cli");
    spin_lock(&task_lock);
    tasks[current_task[cid]].sleep_ticks = ticks;
    tasks[current_task[cid]].state = TASK_STATE_SLEEP;
    rq_remove(current_task[cid]);
    spin_unlock(&task_lock);
    __asm__ volatile("sti");
    
    // Park the task. The timer IRQ runs schedule(), which skips SLEEP tasks
    // (decrementing sleep_ticks) until they flip back to READY; the task then
    // resumes here and exits the loop. The old 100k-pause busy-wait was both a
    // CPU burn and imprecise: it could return while still marked SLEEP, after
    // which the scheduler froze the task at an arbitrary instruction.
    while (tasks[current_task[cid]].state == TASK_STATE_SLEEP) {
        __asm__ volatile("hlt");
    }
}

// Wake up a sleeping task
void task_wake(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return;
    __asm__ volatile("cli");
    spin_lock(&task_lock);
    if (tasks[tid].state == TASK_STATE_SLEEP) {
        tasks[tid].sleep_ticks = 0;
        tasks[tid].state = TASK_STATE_READY;
        rq_enqueue_wake(tid);
    }
    spin_unlock(&task_lock);
    __asm__ volatile("sti");
}

// Blocked-state accessors for sync.c (semaphores/futexes).
// A BLOCKED task is invisible to the scheduler's READY scan, so it stops
// consuming CPU until sync.c flips it back to READY.
int task_get_state(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return TASK_STATE_FREE;
    return tasks[tid].state;
}

void task_set_state(int tid, int state) {
    if (tid < 0 || tid >= MAX_TASKS) return;
    // Idle tasks never block, sleep or get woken — they only ever sit in
    // their own runqueue and hlt loop. Refusing transitions keeps a stray
    // wake (e.g. a corrupted futex waiter list containing an idle tid) from
    // migrating one onto a foreign runqueue.
    if (tasks[tid].is_idle && state != TASK_STATE_RUNNING && state != TASK_STATE_READY) {
        return;
    }
    // Preserve the caller's IF state (irqsave/restore) instead of forcing
    // cli...sti. Callers that hold ANOTHER lock while blocking/waking a task
    // (futex/semaphore wait, under sync_lock) must keep interrupts disabled
    // until they release that lock — an unconditional `sti` here would open a
    // preemption window where the timer parks the caller mid-critical-section,
    // leaving e.g. sync_lock held by a task that can never be woken (the wake
    // itself needs that lock) -> permanent deadlock.
    uint32_t eflags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(eflags) : : "memory");
    spin_lock(&task_lock);
    int old = tasks[tid].state;
    if (old != state) {
        tasks[tid].state = state;
        if (state == TASK_STATE_READY) {
            tasks[tid].sleep_ticks = 0;
            // If the woken task is STILL RUNNING on its owner core (it
            // blocked itself mid-syscall and the timer hasn't preempted it
            // yet), its kernel-stack frame is live: enqueueing it on some
            // (possibly foreign) runqueue would let a peer core iret a stale
            // frame — the double-run that corrupts the task identity. Just
            // flip the state; the owner core resumes it (keep-current path)
            // or re-enqueues it when it finally preempts the park loop.
            if (!task_current_anywhere(tid)) {
                rq_enqueue_wake(tid);
            }
        } else if (state != TASK_STATE_RUNNING) {
            rq_remove(tid);
        }
    }
    spin_unlock(&task_lock);
    __asm__ __volatile__("pushl %0; popfl" : : "r"(eflags) : "memory", "cc");
}

// Get/set priority
int task_set_priority(int tid, int priority) {
    if (tid < 0 || tid >= MAX_TASKS) return -1;
    if (tasks[tid].state == TASK_STATE_FREE) return -1;
    if (priority < PRIORITY_BACKGROUND || priority > PRIORITY_REALTIME) return -1;
    tasks[tid].priority = priority;
    return 0;
}

int task_get_priority(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return -1;
    if (tasks[tid].state == TASK_STATE_FREE) return -1;
    return tasks[tid].priority;
}

// Get/set page directory
uint32_t task_get_page_dir(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return 0;
    return tasks[tid].page_dir;
}

// Does this task run in the kernel's global address space rather than a private
// one? Beware: task 0 stores the boot CR3, NOT 0, so a bare `page_dir == 0`
// test does not recognise the kernel task. Anything deciding "is this caller
// Ring 3 with its own address space?" must go through here.
int task_in_kernel_space(int tid) {
    uint32_t pd = task_get_page_dir(tid);
    if (pd == 0) return 1;
    return (pd & 0xFFFFF000) == (tasks[0].page_dir & 0xFFFFF000);
}

void task_set_page_dir(int tid, uint32_t page_dir) {
    if (tid < 0 || tid >= MAX_TASKS) return;
    tasks[tid].page_dir = page_dir;
}

// Check if a task is alive (a ZOMBIE is not alive)
int task_is_alive(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return 0;
    return (tasks[tid].state != TASK_STATE_FREE && tasks[tid].state != TASK_STATE_ZOMBIE);
}

int task_get_fd(int tid, int local_fd) {
    if (tid < 0 || tid >= MAX_TASKS) return -1;
    if (local_fd < 0 || local_fd >= 16) return -1;
    return tasks[tid].fd_table[local_fd];
}

void task_set_fd(int tid, int local_fd, int global_fd) {
    if (tid < 0 || tid >= MAX_TASKS) return;
    if (local_fd < 0 || local_fd >= 16) return;
    tasks[tid].fd_table[local_fd] = global_fd;
}

// Kill a specific task by ID (called from kernel or syscall). Now routes
// through the signal framework: SIGKILL is uncatchable and terminates the
// task (the parent is woken / the exit code is recorded for waitpid).
int task_kill(int tid) {
    return task_signal(tid, SIGKILL);
}

// Send a signal to a task and every descendant still alive (POSIX process-
// group semantics for Ctrl+C: interrupting the foreground job must reach the
// whole job tree, not just the job leader). Used by the terminal's Ctrl+C.
// Returns the number of tasks signalled, or -1 on a bad root.
int task_signal_group(int root_tid, int sig) {
    if (root_tid <= 0 || root_tid >= MAX_TASKS) return -1;

    // Snapshot the group membership under task_lock: a concurrent fork/exit
    // could otherwise reparent tasks (or free a slot) mid-walk and deliver to
    // the wrong set. Delivery itself happens AFTER the lock is released,
    // because task_signal() re-takes task_lock (cli-first) — nesting would
    // self-deadlock. The snapshot is just a tid list; task_signal() re-checks
    // liveness, so a task that dies meanwhile is skipped safely.
    int members[MAX_TASKS];
    int count = 0;

    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&task_lock);

    if (tasks[root_tid].state == TASK_STATE_FREE) {
        spin_unlock(&task_lock);
        __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
        return -1;
    }

    // One pass over the task table: any task whose parent chain leads back to
    // root_tid is in the group. (Root itself is appended last so its children
    // get the signal even if terminating root first would orphan them.)
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_STATE_FREE || i == root_tid) continue;
        int p = tasks[i].parent;
        int hops = 0;
        // Walk the parent chain toward the root. Guard with a hop cap so a
        // corrupt/cyclic parent chain cannot loop forever.
        while (p > 0 && p < MAX_TASKS && hops < MAX_TASKS && p != i) {
            if (p == root_tid) {
                members[count++] = i;
                break;
            }
            p = tasks[p].parent;
            hops++;
        }
    }
    // Root last: it is the group leader; children above were signalled first.
    members[count++] = root_tid;

    spin_unlock(&task_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));

    int sent = 0;
    for (int i = 0; i < count; i++) {
        if (task_signal(members[i], sig) >= 0) sent++;
    }
    return sent;
}

// ---- Process groups & sessions (Fase 2) ----

int task_get_uid(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return ROOT_UID;
    return tasks[tid].uid;
}
void task_set_uid(int tid, int uid) {
    if (tid < 0 || tid >= MAX_TASKS) return;
    tasks[tid].uid = uid;
}
int task_get_gid(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return 0;
    return tasks[tid].gid;
}
void task_set_gid(int tid, int gid) {
    if (tid < 0 || tid >= MAX_TASKS) return;
    tasks[tid].gid = gid;
}

// ---- Resource limits (v38.28) ----

// SYS_GETRLIMIT: copy the current task's soft+hard value for a resource.
int task_getrlimit(int res, rlimit_t* out) {
    if (res < 0 || res >= RLIM_NLIMITS) return -1;
    if (!out) return -1;
    int cid = get_cid();
    int tid = (current_task[cid] >= 0 && current_task[cid] < MAX_TASKS) ? current_task[cid] : 0;
    *out = tasks[tid].rlimits[res];
    return 0;
}

// SYS_SETRLIMIT (POSIX semantics): a non-root caller may lower cur and raise
// cur only up to max, but may never raise max. Root may set anything. cur
// must not exceed max. Values are clamped to sane bounds to keep a hostile
// caller from setting an absurd hard limit that later trips an overflow.
int task_setrlimit(int res, const rlimit_t* in) {
    if (res < 0 || res >= RLIM_NLIMITS) return -1;
    if (!in) return -1;
    int cid = get_cid();
    int tid = (current_task[cid] >= 0 && current_task[cid] < MAX_TASKS) ? current_task[cid] : 0;

    rlimit_t want = *in;
    // Clamp to hard physical ceilings so a corrupt/absurd value can never
    // cause an arithmetic overflow in the enforcement checks below.
    if (want.max > 0x7FFFFFFFu) want.max = 0x7FFFFFFFu;
    // POSIX EINVAL: cur may never exceed max. Refuse instead of clamping, so
    // a caller cannot silently "raise" a limit by asking for an impossible
    // pair (e.g. `ulimit -n 99` with hard=16 must fail, not clamp to 16).
    if (want.cur > want.max) return -1;

    rlimit_t cur = tasks[tid].rlimits[res];
    int is_root = (tasks[tid].uid == ROOT_UID);
    if (!is_root) {
        // Non-root: never raise the hard limit.
        if (want.max > cur.max) return -1;
        if (want.cur > want.max) return -1;
    }

    tasks[tid].rlimits[res] = want;
    return 0;
}

// RLIMIT_AS enforcement (v38.28): returns 1 when the CURRENT task may grow
// its address space by `additional` bytes without exceeding its soft AS
// limit (root bypasses). Counts committed heap (0x08000000..heap_ptr) plus
// all reserved mmap regions — the same accounting task_mmap_reserve uses.
// Called from SYS_MALLOC and the mmap reservations; no task_lock needed (the
// task table reads are plain and each caller holds its own lock or none).
int task_rlimit_as_allows(uint32_t additional) {
    int cid = get_cid();
    int tid = (current_task[cid] >= 0 && current_task[cid] < MAX_TASKS) ? current_task[cid] : -1;
    if (tid < 0) return 1;
    if (tasks[tid].uid == ROOT_UID) return 1;
    uint32_t limit = tasks[tid].rlimits[RLIMIT_AS].cur;
    if (limit == 0) return 0;
    uint64_t used = rlimit_as_used(tid);
    used += additional;
    return used <= limit;
}

// RLIMIT_NOFILE enforcement at fd allocation: returns 1 when the task may
// allocate another fd, 0 when its soft NOFILE limit is reached. Root
// bypasses. Used by task_map_fd()/do_sys_dup2_tid() before mapping a new
// local fd. Runs without task_lock (plain read of the current task's table;
// fd_lock is already held by every caller).
int task_rlimit_nofile_ok(void) {
    int cid = get_cid();
    int tid = (current_task[cid] >= 0 && current_task[cid] < MAX_TASKS) ? current_task[cid] : -1;
    if (tid < 0) return 1;
    if (tasks[tid].uid == ROOT_UID) return 1;
    uint32_t limit = tasks[tid].rlimits[RLIMIT_NOFILE].cur;
    if (limit >= MAX_FDS_PER_TASK) return 1;
    int open = 0;
    for (int j = 0; j < 16; j++) {
        if (tasks[tid].fd_table[j] >= 0) open++;
    }
    return open < (int)limit;
}

// ---- Thread-local storage (v38.24) ----

// Set the CURRENT task's TLS base. The GDT descriptors are repointed (or
// cleared for base 0) on every CPU; the caller (SYS_TLS_SET handler) also
// rewrites the pending return frame's GS selector so the task runs with %gs
// pointing at its TCB the moment the syscall iret's. The task's %gs register
// at this instant is irrelevant — the frame is what the epilogue loads.
uint32_t task_get_tls_base(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return 0;
    return tasks[tid].tls_base;
}

int task_set_tls(uint32_t base) {
    int cid = get_cid();
    int tid = current_task[cid];
    if (tid <= 0 || tid >= MAX_TASKS) return -1;
    // Must not race the scheduler's per-switch frame loads.
    __asm__ volatile("cli");
    spin_lock(&task_lock);
    tasks[tid].tls_base = base;
    if (base) gdt_tls_update(tid, base); else gdt_tls_clear(tid);
    spin_unlock(&task_lock);
    __asm__ volatile("sti");
    return 0;
}

int task_get_pgrp(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return 0;
    return tasks[tid].pgrp;
}

int task_set_pgrp(int tid, int pgrp) {
    if (tid <= 0 || tid >= MAX_TASKS) return -1;
    if (tasks[tid].state == TASK_STATE_FREE) return -1;
    tasks[tid].pgrp = pgrp;
    return 0;
}

int task_get_session(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return 0;
    return tasks[tid].session;
}

void task_set_session(int tid, int session) {
    if (tid <= 0 || tid >= MAX_TASKS) return;
    tasks[tid].session = session;
}

int task_get_parent(int tid) {
    if (tid <= 0 || tid >= MAX_TASKS) return 0;
    return tasks[tid].parent;
}

int task_get_fg_pgrp(void) {
    return kernel_fg_pgrp;
}

void task_set_fg_pgrp(int pgrp) {
    kernel_fg_pgrp = pgrp;
}

// Signal every task in a process group (POSIX semantics for Ctrl+C/Ctrl+Z:
// the terminal delivers to the foreground group, not to a task subtree).
int task_signal_pgrp(int pgrp, int sig) {
    if (pgrp <= 0 || pgrp >= MAX_TASKS) return -1;
    int sent = 0;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_STATE_FREE) continue;
        if (tasks[i].pgrp != pgrp) continue;
        task_signal(i, sig);
        sent++;
    }
    return sent;
}

// A task is in the background when there is a controlling terminal (fg pgrp
// set) and its own group differs. Tasks without a group (pgrp == 0) or
// without a controlling terminal are never background — this keeps apps
// launched from the desktop (no tty) unaffected.
int task_is_background(int tid) {
    if (tid <= 0 || tid >= MAX_TASKS) return 0;
    if (kernel_fg_pgrp <= 0) return 0;
    int p = tasks[tid].pgrp;
    if (p <= 0) return 0;
    return p != kernel_fg_pgrp;
}

// ============================================================
// fork()
// ============================================================

static int fork_common(void (*kern_entry)(void), const char* child_arg) {
    int cid = get_cid();
    if (current_task[cid] < 0) return -1;
    int parent = current_task[cid];
    if (parent == 0) return -1;                     // kernel idle task cannot fork
    if (task_in_kernel_space(parent)) return -1;    // fork needs a private address space

    __asm__ volatile("cli");
    spin_lock(&task_lock);

    // RLIMIT_NPROC: refuse to create another process when the caller's soft
    // limit is reached (EAGAIN semantics — the caller may retry after
    // reaping or raising the limit).
    if (rlimit_nproc_allows(parent) != 0) {
        spin_unlock(&task_lock);
        __asm__ volatile("sti");
        return -1;
    }

    // COW clone of the parent's address space. User writable pages are marked
    // read-only + PAGE_COW in BOTH directories, so writes fault and duplicate.
    uint32_t new_pd = vmm_clone_address_space(tasks[parent].page_dir);
    if (new_pd == 0) {
        spin_unlock(&task_lock);
        __asm__ volatile("sti");
        return -1;
    }

    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_STATE_FREE) continue;

        // Byte-copy the parent's kernel stack, then patch the child's saved
        // syscall frame. The parent is inside the fork syscall with IF=0
        // (interrupt gate), so the stack cannot contain nested IRQ frames.
        memcpy(&kstacks[i][KERNEL_STACK_GUARD], &kstacks[parent][KERNEL_STACK_GUARD], KERNEL_STACK_SIZE);
        // POSIX: the child inherits the parent's FPU state as of the fork.
        // The parent is current on this CPU with IF=0, so flush its live
        // hardware state into its own image first, then copy it wholesale.
        fpu_save(tasks[parent].fxsave);
        memcpy(tasks[i].fxsave, tasks[parent].fxsave, sizeof(tasks[i].fxsave));
        // CRITICAL: locate the frame at the TOP of the kernel stack, not at
        // tasks[parent].esp. The fork syscall (int 0x80) always pushes its
        // registers_t frame at TSS.esp0 (= kernel stack top), and the parent
        // is INSIDE that syscall right now — so the frame is deterministically
        // at kstack_top(parent) - sizeof(registers_t).
        //
        // tasks[parent].esp is the LAST preemption frame, which is only
        // guaranteed to sit at the stack top when the parent was interrupted
        // in USER mode. If a timer fires while the parent is in kernel mode
        // with IF=1 (e.g. right after gui_unlock()'s sti inside a GUI syscall),
        // schedule() saves a DEEP stack address; copying from there yields a
        // garbage frame whose ds field makes the child's first context-switch
        // epilogue (mov %eax,%ds) take a #GP. KVM's speed makes that window
        // reachable; TCG's slowness masked it.
        // The frame sits at the TOP of the stack (kstack_top is one past the
        // last byte; the memcpy above preserved the parent's top-of-stack
        // frame at the same offset in the child's stack).
        tasks[i].esp = kstack_top(i) - sizeof(registers_t);
        tasks[i].stack_watermark = 0;

        registers_t* fr = (registers_t*)tasks[i].esp;

        // CRITICAL: the saved `esp` field holds the absolute kernel-stack
        // address of THIS frame's int_no slot (the epilogue does popad ->
        // add esp,8 -> iret and reads eip/cs/eflags/useresp/ss through it).
        // The byte copy above carried over the PARENT's address; without this
        // repoint, the child's first iret would pop its return context from
        // the parent's stack — stale the moment the parent pushes any new
        // syscall frame (e.g. a draw_text right after fork()). It only ever
        // worked because fork's slow serial log let the timer preempt the
        // parent while its fork frame was still intact.
        fr->esp = (uint32_t)&fr->int_no;

        // TLS (v38.24): the child inherits the parent's TLS base (POSIX) but
        // gets its OWN FS/GS descriptors keyed to its tid — the byte-copied
        // frame still carries the PARENT's selectors, which would alias the
        // parent's GDT slots and break when the parent exits.
        tasks[i].tls_base = tasks[parent].tls_base;
        if (kern_entry) {
            // Shell background child: never return to user mode — iret straight
            // into a kernel entry that runs the command and exits.
            fr->eip = (uint32_t)(uintptr_t)kern_entry;
            fr->cs  = 0x08;   // kernel code segment
            fr->ds  = 0x10;   // kernel data segment
            fr->fs  = 0x10;   // kernel data segment
            fr->gs  = 0x10;
        } else {
            fr->eax = 0;      // child sees fork() == 0
            if (tasks[i].tls_base) {
                gdt_tls_update(i, tasks[i].tls_base);
                fr->fs = tls_gs_sel(i);
                fr->gs = tls_gs_sel(i);
            } else {
                fr->fs = 0x23;
                fr->gs = 0x23;
            }
        }

        tasks[i].ring = 3;
        tasks[i].priority = tasks[parent].priority;
        tasks[i].sleep_ticks = 0;
        tasks[i].wait_ticks = 0;
        tasks[i].rq_cpu = -1;       // fresh runqueue membership (not inherited)
        tasks[i].is_idle = 0;
        tasks[i].page_dir = new_pd;
        tasks[i].parent = parent;
        tasks[i].exit_code = 0;
        tasks[i].pgrp = tasks[parent].pgrp;          // POSIX: group is inherited
        tasks[i].session = tasks[parent].session;
        tasks[i].waiting = 0;
        tasks[i].pending_signals = 0;
        tasks[i].blocked_signals = tasks[parent].blocked_signals;  // mask inherits
        tasks[i].sig_restart_ticks = 0;
        tasks[i].sig_frame_esp = 0;
        tasks[i].zombie_since = 0;
        tasks[i].shm_bits = tasks[parent].shm_bits;  // child inherits attachments
        memcpy(tasks[i].signal_handlers, tasks[parent].signal_handlers,
               sizeof(tasks[i].signal_handlers));
        memcpy(tasks[i].sig_masks, tasks[parent].sig_masks, sizeof(tasks[i].sig_masks));
        memcpy(tasks[i].sig_flags, tasks[parent].sig_flags, sizeof(tasks[i].sig_flags));
        memcpy(tasks[i].fd_table, tasks[parent].fd_table, sizeof(tasks[i].fd_table));
        memcpy(tasks[i].launch_arg, tasks[parent].launch_arg, sizeof(tasks[i].launch_arg));
        // Trust inherits with the image (fork of a shell host is another
        // instance of the same host program), matching launch_arg semantics.
        tasks[i].trusted_shell = tasks[parent].trusted_shell;
        tasks[i].current_dir = tasks[parent].current_dir;
        tasks[i].heap_ptr = tasks[parent].heap_ptr;
        // Resource limits are inherited by the child (POSIX fork semantics).
        memcpy(tasks[i].rlimits, tasks[parent].rlimits, sizeof(tasks[i].rlimits));
        // Child inherits mmap regions (the COW clone keeps the same VA layout;
        // faulted pages become COW, unfaulted ones stay demand-paged).
        memcpy(tasks[i].mmap_regions, tasks[parent].mmap_regions,
               sizeof(tasks[i].mmap_regions));
        // File-backed regions keep SHARED frames across fork (their PTEs carry
        // PAGE_SHARED, so the clone never COWs them) but each task owns its
        // own dirty bitmap — a fresh copy of the parent's, so either task can
        // msync/munmap and free its own without yanking the other's.
        for (int j = 0; j < MMAP_MAX_REGIONS; j++) {
            if (tasks[i].mmap_regions[j].base != 0 &&
                tasks[i].mmap_regions[j].map_flags == MMAP_FILE_SHARED) {
                uint32_t npages = tasks[i].mmap_regions[j].size / 4096;
                uint8_t* d = kmalloc((npages + 7) / 8);
                if (d) {
                    memcpy(d, tasks[i].mmap_regions[j].dirty, (npages + 7) / 8);
                    tasks[i].mmap_regions[j].dirty = d;
                } else {
                    // OOM: drop the mapping from the child rather than share
                    // the parent's bitmap (double-free on munmap). The frames
                    // stay mapped and are freed at exit via refcounts.
                    memset(&tasks[i].mmap_regions[j], 0, sizeof(mmap_region_t));
                }
            }
        }

        // Share the parent's open fds: bump each global refcount so a close in
        // one process does not yank the descriptor from the other.
        extern global_fd_t global_fds[];
        for (int j = 0; j < 16; j++) {
            int gfd = tasks[i].fd_table[j];
            if (gfd >= 0 && gfd < MAX_GLOBAL_FDS && global_fds[gfd].in_use) {
                global_fds[gfd].ref_count++;
            }
        }

        if (child_arg && child_arg[0]) {
            int n = 0;
            for (; child_arg[n] && n < 127; n++) tasks[i].launch_arg[n] = child_arg[n];
            tasks[i].launch_arg[n] = '\0';
        }

        num_tasks++;
        tasks[i].state = TASK_STATE_READY;
        rq_enqueue(rq_least_loaded(), i);

        spin_unlock(&task_lock);
        __asm__ volatile("sti");

        // Single locked buffer write: the COW #PF handler on another core
        // falls back to raw writes when serial_lock is busy, which interleaves
        // bytes into any multi-call log line. A single write_serial_string
        // with a formatted buffer prints the whole fork marker atomically.
        {
            char bf[80];
            char* bp = bf;
            char* be = bf + sizeof(bf) - 1;
            const char* _s = "[TASK] fork: child tid=";
            while (*_s && bp < be) *bp++ = *_s++;
            *bp++ = '0'; *bp++ = 'x';
            for (int _j = 28; _j >= 0 && bp + 1 < be; _j -= 4) {
                int nib = (i >> _j) & 0xF;
                *bp++ = (nib < 10) ? '0' + nib : 'A' + nib - 10;
            }
            _s = " pd=";
            while (*_s && bp < be) *bp++ = *_s++;
            *bp++ = '0'; *bp++ = 'x';
            for (int _j = 28; _j >= 0 && bp + 1 < be; _j -= 4) {
                int nib = (new_pd >> _j) & 0xF;
                *bp++ = (nib < 10) ? '0' + nib : 'A' + nib - 10;
            }
            *bp++ = '\n'; *bp = '\0';
            write_serial_string(bf);
        }
        return i;
    }

    spin_unlock(&task_lock);
    __asm__ volatile("sti");
    vmm_free_address_space(new_pd);
    return -1;
}

int task_fork(void) {
    return fork_common(NULL, NULL);
}

// ============================================================
// exec() — replace the current task's image in place
// ============================================================

// exec does NOT return to the caller on success — the syscall epilogue iret's
// straight into the new program's entry point. So the failure path is the only
// one that matters here: it must leave the old image completely untouched.
int task_exec(const char* path, const char* arg, void* frame) {
    int cid = get_cid();
    if (current_task[cid] < 0) return -1;
    int tid = current_task[cid];
    if (tid == 0) return -1;                     // kernel idle task cannot exec
    if (task_in_kernel_space(tid)) return -1;    // exec needs a private address space

    // Build the new image (new address space + code copied in). The caller
    // already copied path/arg into kernel memory, so the VFS read below works
    // regardless of which address space is active.
    extern int loader_build_image(const char* filename, const char* arg, loader_image_t* img);
    loader_image_t img;
    int rc = loader_build_image(path, arg, &img);
    if (rc < 0) return rc;
    if (img.page_dir == 0) return -6;

    // Map a fresh Ring 3 stack for this task's slot in the NEW address space.
    uint32_t stack_top = USER_STACK_TOP - ((uint32_t)(tid + 1) * USER_STACK_SIZE);
    uint32_t user_esp = vmm_setup_user_stack(img.page_dir, stack_top);
    if (user_esp == 0) {
        vmm_free_address_space(img.page_dir);
        return -6;
    }

    __asm__ volatile("cli");

    // Drop the old image's windows (the app code that owned them is gone).
    extern void wm_cleanup_task(int tid);
    wm_cleanup_task(tid);

    // Switch CR3 to the new address space BEFORE freeing the old one. The
    // kernel identity map lives in every address space, so we keep running.
    vmm_switch_page_dir(img.page_dir);

    // Free the old address space only if no sibling task shares it (threads of
    // one process share page_dir; freeing it under a sibling would yank its
    // memory away).
    uint32_t old_pd = tasks[tid].page_dir;
    if (old_pd != 0 && old_pd != tasks[0].page_dir) {
        int sharers = 0;
        for (int k = 1; k < MAX_TASKS; k++) {
            if (k != tid && tasks[k].state != TASK_STATE_FREE &&
                tasks[k].page_dir == old_pd) sharers++;
        }
        if (sharers == 0) {
            extern void vmm_free_address_space(uint32_t);
            vmm_free_address_space(old_pd);
        }
    }

    // ---- Rewire the task state to the new image ----
    tasks[tid].page_dir = img.page_dir;
    tasks[tid].heap_ptr = img.heap_start;
    // A fresh address space has no mmap regions. Drop the old image's
    // file-backed dirty bitmaps first (exec discards mappings without
    // writeback, POSIX-style).
    mmap_free_dirty_bitmaps(tid);
    // exec replaces the image: the old PAGE_DEV scanout mapping (if any) is
    // discarded with the region table — release ownership too, so the next
    // image does not silently keep pixels it never asked for.
    fb_cleanup_task(tid);
    for (int j = 0; j < MMAP_MAX_REGIONS; j++) tasks[tid].mmap_regions[j].base = 0;

    // Reset signal state per POSIX: caught handlers revert to default, ignored
    // ones stay ignored; nothing is pending; no sigframe in flight; the signal
    // mask is cleared.
    for (int s = 1; s < SIG_MAX; s++) {
        if (tasks[tid].signal_handlers[s] != SIG_IGN_SENTINEL) {
            tasks[tid].signal_handlers[s] = NULL;
        }
        tasks[tid].sig_masks[s] = 0;
        tasks[tid].sig_flags[s] = 0;
    }
    tasks[tid].pending_signals = 0;
    // The signal mask survives exec (POSIX): only caught handlers reset to
    // default. This matters once process groups land (SIGTTIN/SIGTTOU rely on
    // the inherited mask across exec).
    tasks[tid].sig_restart_ticks = 0;
    tasks[tid].sig_frame_esp = 0;

    if (arg && arg[0]) {
        int n = 0;
        for (; arg[n] && n < 127; n++) tasks[tid].launch_arg[n] = arg[n];
        tasks[tid].launch_arg[n] = '\0';
    } else {
        task_set_launch_arg(tid, path);
    }

    // Recompute shell-host trust for the NEW image (v38.53): the parent in
    // the rule is this task's own pre-exec flag, so exec'ing a host binary
    // from an untrusted app does not confer trust.
    tasks[tid].trusted_shell = task_grant_trusted_shell(tid, path);

    // ---- Patch the LIVE syscall return frame ----
    // The task is inside SYS_EXEC (interrupt gate, IF=0), so its registers_t
    // frame is deterministically at the top of the kernel stack — exactly the
    // frame the syscall epilogue will iret from. Rewriting eip/cs/useresp/ss
    // makes that iret land in the new program instead of returning to the old.
    registers_t* fr = (registers_t*)frame;
    fr->eip     = img.entry;
    fr->cs      = 0x1B;
    fr->ss      = 0x23;
    fr->ds      = 0x23;
    fr->eflags  = 0x202;   // IF=1
    fr->useresp = user_esp;
    fr->eax     = 0;       // exec() reports success (never actually returned)
    // TLS (v38.24): exec replaces the address space, so the old TCB VA is
    // gone. Reset the task's TLS — the new program calls SYS_TLS_SET itself
    // if it wants one — and drop the frame's stale TLS selectors.
    if (tasks[tid].tls_base) { gdt_tls_clear(tid); tasks[tid].tls_base = 0; }
    fr->fs = 0x23;
    fr->gs = 0x23;

    // Fresh program image -> fresh FPU state (POSIX: exec resets the FPU).
    // This task keeps the CPU (the epilogue iret's straight into the new
    // program), so swap the hardware state now, not at the next switch.
    fpu_task_init(tasks[tid].fxsave);
    fpu_restore(tasks[tid].fxsave);

    __asm__ volatile("sti");

    write_serial_string("[TASK] exec: tid=");
    write_serial_hex(tid);
    write_serial_string(" entry=");
    write_serial_hex(img.entry);
    write_serial_string(" pd=");
    write_serial_hex(img.page_dir);
    write_serial_string("\n");

    // The frame is now the new program's entry; the epilogue iret's into it.
    return 0;
}

int task_fork_kernel(void (*entry)(void), const char* child_arg) {
    return fork_common(entry, child_arg);
}

// ============================================================
// fork + exec in one step (pipeline / redirection child)
// ============================================================

int task_fork_exec(int in_fd, int out_fd, const char* path, const char* arg) {
    int cid = get_cid();
    if (current_task[cid] <= 0) return -1;   // kernel task cannot spawn

    // Build the child's image FIRST (kernel-side copies, so safe regardless
    // of which address space is active).
    extern int loader_build_image(const char* filename, const char* arg,
                                  loader_image_t* img);
    loader_image_t img;
    int rc = loader_build_image(path, arg, &img);
    if (rc < 0) return rc;
    if (img.page_dir == 0) return -6;

    __asm__ volatile("cli");
    spin_lock(&task_lock);

    // RLIMIT_NPROC: the spawner (the shell) is a normal user task, so creating
    // the child counts against its limit like any other fork.
    if (current_task[cid] > 0 && rlimit_nproc_allows(current_task[cid]) != 0) {
        spin_unlock(&task_lock);
        __asm__ volatile("sti");
        extern void vmm_free_address_space(uint32_t);
        vmm_free_address_space(img.page_dir);
        return -1;
    }

    int child = -1;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_STATE_FREE) continue;
        child = i;
        break;
    }
    if (child < 0) {
        spin_unlock(&task_lock);
        __asm__ volatile("sti");
        extern void vmm_free_address_space(uint32_t);
        vmm_free_address_space(img.page_dir);
        return -2;
    }

    // Map the child's Ring 3 stack in the NEW address space (same slot
    // discipline as thread_create so stacks never collide).
    uint32_t stack_top = USER_STACK_TOP - ((uint32_t)(child + 1) * USER_STACK_SIZE);
    uint32_t user_esp = vmm_setup_user_stack(img.page_dir, stack_top);
    if (user_esp == 0) {
        spin_unlock(&task_lock);
        __asm__ volatile("sti");
        vmm_free_address_space(img.page_dir);
        return -6;
    }

    // Fill the task slot. The child is a brand-new process, NOT a copy of the
    // shell: fresh stack, fresh kernel frame, exec'd image.
    tasks[child].ring = 3;
    tasks[child].priority = PRIORITY_INTERACTIVE;
    tasks[child].page_dir = img.page_dir;
    tasks[child].sleep_ticks = 0;
    tasks[child].wait_ticks = 0;
    tasks[child].rq_cpu = -1;
    tasks[child].is_idle = 0;
    tasks[child].parent = current_task[cid];   // shell can waitpid it
    tasks[child].exit_code = 0;
    tasks[child].pgrp = tasks[current_task[cid]].pgrp;  // inherits the shell group
    tasks[child].session = tasks[current_task[cid]].session;
    tasks[child].waiting = 0;
    tasks[child].pending_signals = 0;
    tasks[child].blocked_signals = 0;
    tasks[child].sig_restart_ticks = 0;
    tasks[child].sig_frame_esp = 0;
    tasks[child].zombie_since = 0;
    tasks[child].shm_bits = 0;
    tasks[child].heap_ptr = img.heap_start;
    tasks[child].current_dir = tasks[current_task[cid]].current_dir;
    // The spawned process inherits the caller's resource limits (POSIX exec).
    memcpy(tasks[child].rlimits, tasks[current_task[cid]].rlimits, sizeof(tasks[child].rlimits));
    mmap_free_dirty_bitmaps(child);  // fresh spawn: never inherit stale bitmaps
    for (int j = 0; j < MMAP_MAX_REGIONS; j++) tasks[child].mmap_regions[j].base = 0;
    for (int j = 0; j < SIG_MAX; j++) tasks[child].signal_handlers[j] = NULL;

    // Inherit the caller's fd table, then rewire fd 0/1 for the pipe/file and
    // drop every other descriptor (POSIX spawn: clean stdin/stdout/stderr).
    extern global_fd_t global_fds[];
    for (int j = 0; j < 16; j++) tasks[child].fd_table[j] = -1;
    for (int j = 0; j < 16; j++) {
        int gfd = tasks[current_task[cid]].fd_table[j];
        if (gfd >= 0 && gfd < MAX_GLOBAL_FDS && global_fds[gfd].in_use) {
            tasks[child].fd_table[j] = gfd;
            global_fds[gfd].ref_count++;
        }
    }
    extern void task_rewire_fds(int tid, int in_fd, int out_fd);
    task_rewire_fds(child, in_fd, out_fd);

    if (arg && arg[0]) {
        int n = 0;
        for (; arg[n] && n < 127; n++) tasks[child].launch_arg[n] = arg[n];
        tasks[child].launch_arg[n] = '\0';
    } else {
        int n = 0;
        for (; path[n] && n < 127; n++) tasks[child].launch_arg[n] = path[n];
        tasks[child].launch_arg[n] = '\0';
    }

    // Fresh Ring 3 interrupt frame at the top of the child's kernel stack.
    uint32_t* stack = (uint32_t*)kstack_top(child);
    *(--stack) = 0x23;       // SS
    *(--stack) = user_esp;   // ESP
    *(--stack) = 0x202;      // EFLAGS
    *(--stack) = 0x1B;       // CS
    *(--stack) = (uint32_t)(uintptr_t)img.entry; // EIP
    *(--stack) = 0;          // err_code
    *(--stack) = 0;          // int_no
    *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; *(--stack) = 0;
    *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; *(--stack) = 0;
    *(--stack) = 0x23;       // DS
    *(--stack) = 0x23;       // FS (user data — no TLS)
    *(--stack) = 0x23;       // GS (user data — no TLS)
    tasks[child].esp = (uint32_t)stack;
    tasks[child].stack_watermark = 0;

    num_tasks++;
    tasks[child].state = TASK_STATE_READY;
    rq_enqueue(rq_least_loaded(), child);

    spin_unlock(&task_lock);
    __asm__ volatile("sti");

    write_serial_string("[TASK] fork_exec: child tid=");
    write_serial_hex(child);
    write_serial_string(" path=");
    write_serial_string(path);
    write_serial_string(" in=");
    write_serial_hex(in_fd);
    write_serial_string(" out=");
    write_serial_hex(out_fd);
    write_serial_string("\n");
    return child;
}

// ============================================================
// waitpid()
// ============================================================

int task_waitpid(int pid, int* status, int options) {
    int cid = get_cid();
    int self = current_task[cid];
    if (self <= 0) return -1;
    if (status == NULL) return -1;

    for (;;) {
        int found_child = 0;
        int zombie = -1;
        for (int i = 1; i < MAX_TASKS; i++) {
            if (tasks[i].state == TASK_STATE_FREE) continue;
            if (tasks[i].parent != self) continue;
            if (pid > 0 && i != pid) continue;
            found_child = 1;
            if (tasks[i].state == TASK_STATE_ZOMBIE) { zombie = i; break; }
        }

        if (zombie >= 0) {
            // Reap under task_lock (v38.55): the scan above is a snapshot.
            // Two sibling threads in waitpid() could both pick the same
            // zombie and double-reap (state=FREE twice, num_tasks underflow,
            // TLS descriptors cleared for a slot already recycled to a new
            // task). Re-validate under the lock and re-scan if we lost the
            // race. Lock order note: this path holds no other locks, and
            // terminate_task's wake flips our state under the same lock.
            __asm__ volatile("cli");
            spin_lock(&task_lock);
            if (tasks[zombie].state != TASK_STATE_ZOMBIE ||
                tasks[zombie].parent != self) {
                spin_unlock(&task_lock);
                __asm__ volatile("sti");
                continue;   // peer reaped it mid-scan — restart the search
            }
            *status = tasks[zombie].exit_code;
            tasks[zombie].state = TASK_STATE_FREE;
            num_tasks--;
            gdt_tls_clear(zombie);   // slot freed: drop its TLS descriptors
            spin_unlock(&task_lock);
            __asm__ volatile("sti");
            return zombie;
        }
        if (!found_child) return -1;          // ECHILD
        if (options & 1) return 0;            // WNOHANG: nothing to reap yet

        // Park the task until a child exits. The exit path flips us back to
        // READY (see terminate_task) — same hlt-park pattern as task_sleep().
        __asm__ volatile("cli");
        spin_lock(&task_lock);
        tasks[self].waiting = (pid > 0) ? pid : -1;
        tasks[self].state = TASK_STATE_BLOCKED;
        rq_remove(self);
        // Lost-wakeup guard: a child can exit between the scan above and this
        // park. Its exit path saw waiting == 0 and therefore did NOT wake us
        // (see terminate_task), leaving us about to sleep forever with a
        // reaped-able zombie. Re-check for a zombie under task_lock — the exit
        // path holds the same lock, so this re-check cannot race it. If one
        // exists, undo the park and let the next loop iteration reap it.
        int recheck_zombie = -1;
        for (int i = 1; i < MAX_TASKS; i++) {
            if (tasks[i].state == TASK_STATE_FREE) continue;
            if (tasks[i].parent != self) continue;
            if (pid > 0 && i != pid) continue;
            if (tasks[i].state == TASK_STATE_ZOMBIE) { recheck_zombie = i; break; }
        }
        if (recheck_zombie >= 0) {
            tasks[self].waiting = 0;
            tasks[self].state = TASK_STATE_READY;
            // We are still current on this core, so do NOT enqueue: if a timer
            // tick preempts us before the loop re-scans, schedule() re-enqueues
            // us via the keep-current path (READY + rq_cpu < 0).
        }
        spin_unlock(&task_lock);
        __asm__ volatile("sti");
        if (recheck_zombie >= 0) continue;   // reap the zombie on the next pass
        while (tasks[self].state == TASK_STATE_BLOCKED) __asm__ volatile("hlt");
        tasks[self].waiting = 0;
    }
}

// ============================================================
// Signals
// ============================================================

void task_set_signal_handler(int tid, int sig, void* h) {
    task_set_sigaction(tid, sig, h, 0, 0);
}

void* task_get_signal_handler(int tid, int sig) {
    void* h; uint32_t m, f;
    task_get_sigaction(tid, sig, &h, &m, &f);
    return h;
}

void task_set_sigaction(int tid, int sig, void* h, uint32_t mask, uint32_t flags) {
    if (tid < 0 || tid >= MAX_TASKS || sig <= 0 || sig >= SIG_MAX) return;
    tasks[tid].signal_handlers[sig] = h;
    tasks[tid].sig_masks[sig] = mask;
    tasks[tid].sig_flags[sig] = flags;
}

void task_get_sigaction(int tid, int sig, void** h, uint32_t* mask, uint32_t* flags) {
    if (h) *h = NULL;
    if (mask) *mask = 0;
    if (flags) *flags = 0;
    if (tid < 0 || tid >= MAX_TASKS || sig <= 0 || sig >= SIG_MAX) return;
    if (h) *h = tasks[tid].signal_handlers[sig];
    if (mask) *mask = tasks[tid].sig_masks[sig];
    if (flags) *flags = tasks[tid].sig_flags[sig];
}

uint32_t task_get_blocked(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return 0;
    return tasks[tid].blocked_signals;
}

void task_set_blocked(int tid, uint32_t bits) {
    if (tid < 0 || tid >= MAX_TASKS) return;
    tasks[tid].blocked_signals = bits;
}

uint32_t task_get_sig_frame_esp(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return 0;
    return tasks[tid].sig_frame_esp;
}

void task_set_sig_frame_esp(int tid, uint32_t esp) {
    if (tid < 0 || tid >= MAX_TASKS) return;
    tasks[tid].sig_frame_esp = esp;
}

int task_signal(int tid, int sig) {
    if (tid <= 0 || tid >= MAX_TASKS) return -1;
    if (tasks[tid].state == TASK_STATE_FREE) return -1;
    if (tasks[tid].state == TASK_STATE_ZOMBIE) return 0;  // already dead, waiting to be reaped
    if (sig <= 0 || sig >= SIG_MAX) return -1;

    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&task_lock);

    if (sig == SIGKILL) {
        // Uncatchable: terminate immediately.
        terminate_task(tid, 128 + SIGKILL);
        spin_unlock(&task_lock);
        __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
        return 0;
    }
    if (sig == SIGSTOP) {
        // Uncatchable, like SIGKILL: suspend the task until SIGCONT. The
        // scheduler only picks READY tasks, so a STOPPED task never runs.
        tasks[tid].state = TASK_STATE_STOPPED;
        tasks[tid].pending_signals &= ~((1u << SIGSTOP) | (1u << SIGTSTP));
        rq_remove(tid);
        spin_unlock(&task_lock);
        __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
        return 0;
    }
    if (sig == SIGCONT) {
        // Resume a suspended task. Also discards any pending stop requests.
        tasks[tid].pending_signals &= ~((1u << SIGSTOP) | (1u << SIGTSTP));
        if (tasks[tid].state == TASK_STATE_STOPPED) {
            tasks[tid].state = TASK_STATE_READY;
            rq_enqueue_wake(tid);
        }
        spin_unlock(&task_lock);
        __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
        return 0;
    }

    // SIGKILL/SIGSTOP/SIGCONT are never blocked (POSIX). For every other
    // signal, a blocked signal is marked pending but its action (default or
    // handler) is deferred until sigprocmask unblocks it — it must also NOT
    // wake a parked task, since the delivery would otherwise race ahead of the
    // unblock.
    uint32_t blocked = tasks[tid].blocked_signals & (1u << sig);
    if (blocked && sig != SIGKILL && sig != SIGSTOP && sig != SIGCONT) {
        tasks[tid].pending_signals |= (1u << sig);
        spin_unlock(&task_lock);
        __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
        return 0;
    }

    void* h = tasks[tid].signal_handlers[sig];
    if (h == SIG_IGN_SENTINEL) {
        spin_unlock(&task_lock);
        __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
        return 0;   // ignored
    }
    if (h == NULL) {
        // Default action: SIGCHLD is ignored; SIGTSTP/SIGTTIN/SIGTTOU suspend
        // the task (SIGTSTP is the catchable variant of SIGSTOP, delivered by
        // Ctrl+Z; SIGTTIN/SIGTTOU stop background jobs touching the terminal);
        // everything else terminates.
        if (sig == SIGCHLD) {
            // ignored
        } else if (sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) {
            tasks[tid].state = TASK_STATE_STOPPED;
            tasks[tid].pending_signals &= ~(1u << sig);
            rq_remove(tid);
            write_serial_string("[SIG] default ");
            write_serial_hex(sig);
            write_serial_string(": stopped tid=");
            write_serial_hex(tid);
            write_serial_string("\n");
        } else {
            terminate_task(tid, 128 + sig);
        }
        spin_unlock(&task_lock);
        __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
        return 0;
    }

    // A handler is installed: mark pending and wake the task if it is parked,
    // so it returns to user mode and the handler runs there. If the handler is
    // SA_RESTART and the task was parked in SYS_SLEEP, remember the remaining
    // ticks so delivery can re-enter the sleep after the handler returns.
    tasks[tid].pending_signals |= (1u << sig);
    if (tasks[tid].state == TASK_STATE_BLOCKED || tasks[tid].state == TASK_STATE_SLEEP) {
        if ((tasks[tid].sig_flags[sig] & SA_RESTART) && tasks[tid].state == TASK_STATE_SLEEP) {
            tasks[tid].sig_restart_ticks = tasks[tid].sleep_ticks;
        }
        tasks[tid].state = TASK_STATE_READY;
        rq_enqueue_wake(tid);
    }
    spin_unlock(&task_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
    return 0;
}

// Deliver one pending signal into the task's return frame. `frame` is the
// registers_t* about to be iret'd back to Ring 3 (kernel stack).
//
// Handler signals: a sigframe with the full saved context is pushed on the
// user stack, the stack is arranged as `handler(sig)` with a return address
// pointing at the kernel-mapped trampoline (which performs SYS_SIGRETURN),
// and the frame's EIP/USERESP are rewritten to enter the handler.
//
// Default-action signals terminate the task; the frame is patched to park in
// the kernel so we never iret a dead task's user context.
int task_deliver_signals(void* frame) {
    registers_t* r = (registers_t*)frame;
    if ((r->cs & 3) != 3) return 0;

    int tid = get_current_task();
    if (tid <= 0 || tid >= MAX_TASKS) return 0;
    if (tasks[tid].state == TASK_STATE_FREE || tasks[tid].state == TASK_STATE_ZOMBIE) return 0;
    if (tasks[tid].pending_signals == 0) return 0;
    if (tasks[tid].sig_frame_esp != 0) return 0;  // already inside a handler

    for (int sig = 1; sig < SIG_MAX; sig++) {
        if (!(tasks[tid].pending_signals & (1u << sig))) continue;
        // A blocked signal stays pending (delivered only after sigprocmask
        // unblocks it). SIGKILL/SIGSTOP/SIGCONT bypass the mask.
        if ((tasks[tid].blocked_signals & (1u << sig)) &&
            sig != SIGKILL && sig != SIGSTOP && sig != SIGCONT) continue;
        tasks[tid].pending_signals &= ~(1u << sig);

        void* h = tasks[tid].signal_handlers[sig];
        if (h == SIG_IGN_SENTINEL) continue;

        if (h == NULL) {
            if (sig == SIGCHLD) continue;   // default: ignore
            terminate_task(tid, 128 + sig);
            // Never iret back into a dead task: park in the kernel instead.
            r->eip = (uint32_t)(uintptr_t)&task_dead_park;
            r->cs  = 0x08;
            r->ds  = 0x10;
            r->fs  = 0x10;
            r->gs  = 0x10;
            return 1;
        }

        // ---- User handler delivery ----
        uint32_t esp = r->useresp;
        // ~64 bytes of sigframe + handler call frame below the current stack
        // top. Check the pages are mapped & user-writable so the direct write
        // below cannot fault at CPL 0 (validate against this task's own
        // page directory, which is active right now).
        extern int validate_user_ptr(const void* ptr, uint32_t size);
        if (!validate_user_ptr((void*)(esp - 128), 128)) {
            // Broken stack: cannot deliver — terminate instead.
            terminate_task(tid, 128 + sig);
            r->eip = (uint32_t)(uintptr_t)&task_dead_park;
            r->cs  = 0x08;
            r->ds  = 0x10;
            r->fs  = 0x10;
            r->gs  = 0x10;
            return 1;
        }

        esp -= sizeof(sigframe_t);
        sigframe_t* f = (sigframe_t*)esp;
        f->sig         = (uint32_t)sig;
        f->saved_eax   = r->eax;
        f->saved_ecx   = r->ecx;
        f->saved_edx   = r->edx;
        f->saved_ebx   = r->ebx;
        f->saved_esi   = r->esi;
        f->saved_edi   = r->edi;
        f->saved_ebp   = r->ebp;
        f->saved_eip   = r->eip;
        f->saved_cs    = r->cs;
        f->saved_eflags= r->eflags;
        f->saved_esp   = r->useresp;
        f->saved_ss    = r->ss;
        // Save the current mask so SYS_SIGRETURN can restore it. While the
        // handler runs, the delivered signal itself is auto-blocked (unless
        // SA_NODEFER) together with sa_mask — this is what prevents a handler
        // from re-entering itself or clobbering a signal the app is holding.
        f->saved_blocked = tasks[tid].blocked_signals;
        // SA_RESTART: hand the interrupted SYS_SLEEP's remaining ticks to the
        // sigreturn path, which re-parks the task before returning to user.
        // Only the delivered signal's own SA_RESTART flag qualifies — if
        // several signals were pending when the sleep was interrupted, the
        // restart must not ride along on a non-SA_RESTART handler's frame.
        f->restart = (tasks[tid].sig_flags[sig] & SA_RESTART) &&
                     (tasks[tid].sig_restart_ticks > 0) ? 1 : 0;
        f->restart_arg = tasks[tid].sig_restart_ticks;
        tasks[tid].sig_restart_ticks = 0;
        uint32_t run_mask = tasks[tid].blocked_signals;
        run_mask |= tasks[tid].sig_masks[sig];
        if (!(tasks[tid].sig_flags[sig] & SA_NODEFER)) run_mask |= (1u << sig);
        tasks[tid].blocked_signals = run_mask;
        tasks[tid].sig_frame_esp = (uint32_t)esp;

        // cdecl handler(sig): [ret addr][arg] on the user stack. The handler
        // `ret`s into the trampoline, which issues SYS_SIGRETURN.
        esp -= 8;
        ((uint32_t*)esp)[0] = SIG_TRAMPOLINE_VA;
        ((uint32_t*)esp)[1] = (uint32_t)sig;

        r->eip     = (uint32_t)h;
        r->useresp = esp;
        return 1;
    }
    return 0;
}

// Deliver a synchronous fault signal (SIGSEGV, ...) to the CURRENT task from
// exception context (the isr_handler path in idt.c — an unresolvable user
// #PF/#GP/#UD, interrupt gate, IF=0). Reuses the normal delivery machinery:
// a user-installed handler gets its sigframe pushed and EIP/USERESP rewritten
// so the handler runs; the default action (no handler, or SIG_IGN — a
// synchronous fault cannot be meaningfully ignored) terminates the task with
// exit status 128+sig and parks the frame so we never iret a dead task's user
// context. Returns 1 (frame always rewritten: handler or park).
int task_fault_signal(int sig, void* frame) {
    registers_t* r = (registers_t*)frame;
    if ((r->cs & 3) != 3) return 0;
    int tid = get_current_task();
    if (tid <= 0 || tid >= MAX_TASKS) return 0;
    if (sig <= 0 || sig >= SIG_MAX) return 0;

    // Exception context: interrupts already disabled (interrupt gate), and
    // terminate_task/task_deliver_signals require task_lock + IF=0.
    spin_lock(&task_lock);

    void* h = tasks[tid].signal_handlers[sig];
    if (h == NULL || h == SIG_IGN_SENTINEL) {
        // Default action: terminate (SIG_IGN on a fault is treated as kill).
        terminate_task(tid, 128 + sig);
        r->eip = (uint32_t)(uintptr_t)&task_dead_park;
        r->cs  = 0x08;
        r->ds  = 0x10;
        r->fs  = 0x10;
        r->gs  = 0x10;
        spin_unlock(&task_lock);
        return 1;
    }

    // Handler installed: mark pending and deliver right now. If delivery
    // fails (e.g. a fault inside an already-running handler — sig_frame_esp
    // set — or a broken user stack), terminate rather than re-fault forever.
    tasks[tid].pending_signals |= (1u << sig);
    int delivered = task_deliver_signals(frame);
    if (!delivered) {
        terminate_task(tid, 128 + sig);
        r->eip = (uint32_t)(uintptr_t)&task_dead_park;
        r->cs  = 0x08;
        r->ds  = 0x10;
        r->fs  = 0x10;
        r->gs  = 0x10;
    }
    spin_unlock(&task_lock);
    return 1;
}

// Reap zombies whose parent is gone or that outlived the timeout. Called from
// the BSP main loop once per second as a safety net for parents that launch
// children but never call waitpid() (e.g. the terminal's `run`).
void task_reap_zombies(void) {
    uint32_t now = get_ticks();
    __asm__ volatile("cli");
    spin_lock(&task_lock);
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_STATE_ZOMBIE) continue;
        int p = tasks[i].parent;
        int parent_gone = (p <= 0 || p >= MAX_TASKS ||
                           tasks[p].state == TASK_STATE_FREE ||
                           tasks[p].state == TASK_STATE_ZOMBIE);
        if (parent_gone || (now - tasks[i].zombie_since) > ZOMBIE_REAP_MS) {
            tasks[i].state = TASK_STATE_FREE;
            num_tasks--;
        }
    }
    spin_unlock(&task_lock);
    __asm__ volatile("sti");
}

int get_task_info(int tid, task_info_t* info) {
    if (tid < 0 || tid >= MAX_TASKS) return 0;
    if (tasks[tid].state == TASK_STATE_FREE) return 0;
    
    info->id = tid;
    info->state = tasks[tid].state;
    info->ring = tasks[tid].ring;
    info->priority = tasks[tid].priority;
    info->sleep_ticks = tasks[tid].sleep_ticks;
    info->stack_watermark = tasks[tid].stack_watermark;
    return 1;
}

int task_enum(int after, task_info_t* info) {
    for (int i = after + 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_STATE_FREE) continue;
        info->id = i;
        info->state = tasks[i].state;
        info->ring = tasks[i].ring;
        info->priority = tasks[i].priority;
        info->sleep_ticks = tasks[i].sleep_ticks;
        info->stack_watermark = tasks[i].stack_watermark;
        return i;
    }
    return -1;
}

void task_set_launch_arg(int tid, const char* arg) {
    if (tid < 0 || tid >= MAX_TASKS) return;
    int i = 0;
    if (arg) {
        for (; i < 127 && arg[i]; i++) tasks[tid].launch_arg[i] = arg[i];
    }
    tasks[tid].launch_arg[i] = '\0';
}

const char* task_get_launch_arg(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return "";
    return tasks[tid].launch_arg;
}

// ---- Trusted shell hosts (v38.53 privilege fix) ----
// SYS_EXEC_CMD and SYS_KILL_TASK are privileged: their builtins call VFS
// internals that bypass permission checks, and KILL_TASK can hit any task.
// The old gate compared the caller's launch_arg STRING against a whitelist —
// but any Ring 3 app can SYS_EXEC("apps/terminal.mct", "terminal.mct") and
// have the loader store that exact string, so the gate was forgeable. Trust
// is now a kernel-computed flag:
//
//   granted = image basename is a shell host
//             AND (spawner is kernel-space / uid 0 / already trusted)
//
// A rogue app exec'ing terminal.mct itself stays untrusted because its own
// flag is 0; the real terminal spawning another host (or `run` inside the
// terminal) keeps working. Fork inherits, exec recomputes.
static int shell_host_image(const char* path) {
    static const char* const hosts[] = {
        "terminal.mct", "explorer.mct", "taskmgr.mct",
    };
    if (!path) return 0;
    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    for (int i = 0; i < (int)(sizeof(hosts) / sizeof(hosts[0])); i++) {
        const char* h = hosts[i];
        const char* b = base;
        while (*h && *b && *h == *b) { h++; b++; }
        if (*h == '\0' && *b == '\0') return 1;
    }
    return 0;
}

int task_grant_trusted_shell(int parent_tid, const char* image_name) {
    if (!shell_host_image(image_name)) return 0;
    if (parent_tid < 0 || parent_tid >= MAX_TASKS) return 1;   // kernel spawn
    if (tasks[parent_tid].state == TASK_STATE_FREE) return 1;  // pre-init spawn
    if (task_in_kernel_space(parent_tid)) return 1;
    if (tasks[parent_tid].uid == ROOT_UID) return 1;
    return tasks[parent_tid].trusted_shell;
}

int task_is_trusted_shell(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return 0;
    return tasks[tid].trusted_shell;
}

void task_set_trusted_shell(int tid, int trusted) {
    if (tid < 0 || tid >= MAX_TASKS) return;
    tasks[tid].trusted_shell = trusted ? 1 : 0;
}

uint32_t task_get_heap_ptr(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return 0x08000000;
    return tasks[tid].heap_ptr;
}

void task_set_heap_ptr(int tid, uint32_t ptr) {
    if (tid < 0 || tid >= MAX_TASKS) return;
    tasks[tid].heap_ptr = ptr;
}

uint32_t task_get_shm_bits(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return 0;
    return tasks[tid].shm_bits;
}

void task_set_shm_bits(int tid, uint32_t bits) {
    if (tid < 0 || tid >= MAX_TASKS) return;
    tasks[tid].shm_bits = bits;
}

// ============================================================
// mmap() — reserve a VA range now, materialize frames on fault
// ============================================================

// Free every file-backed dirty bitmap a task holds. Called when an address
// space is discarded (exec, task death) — no writeback: a dead task's
// changes are lost, matching anonymous-mapping discard semantics.
static void mmap_free_dirty_bitmaps(int tid) {
    for (int j = 0; j < MMAP_MAX_REGIONS; j++) {
        if (tasks[tid].mmap_regions[j].dirty) {
            kfree(tasks[tid].mmap_regions[j].dirty);
            tasks[tid].mmap_regions[j].dirty = NULL;
        }
    }
}

// Find a free region slot and the lowest gap in [MMAP_BASE, MMAP_END) that
// fits `size`. Returns 1 with *slot/*base set, 0 on failure. Shared by the
// anonymous and file-backed mmap paths.
static int mmap_region_alloc(int tid, uint32_t size, int* slot, uint32_t* base) {
    int s = -1;
    for (int j = 0; j < MMAP_MAX_REGIONS; j++) {
        if (tasks[tid].mmap_regions[j].base == 0) { s = j; break; }
    }
    if (s < 0) {
        write_serial_string("[MMAP] too many regions for TID ");
        write_serial_hex(tid);
        write_serial_string("\n");
        return 0;
    }
    uint32_t cursor = MMAP_BASE;
    for (;;) {
        int fits = 1;
        for (int j = 0; j < MMAP_MAX_REGIONS; j++) {
            uint32_t b = tasks[tid].mmap_regions[j].base;
            if (b == 0) continue;
            uint32_t e = b + tasks[tid].mmap_regions[j].size;
            if (cursor < e && (cursor + size) > b) { fits = 0; break; }
        }
        if (fits) break;
        cursor = ((cursor + size + 0xFFF) & ~0xFFFu) + 4096;
        if (cursor >= MMAP_END) {
            write_serial_string("[MMAP] no VA space left for TID ");
            write_serial_hex(tid);
            write_serial_string("\n");
            return 0;
        }
    }
    *slot = s;
    *base = cursor;
    return 1;
}

// Reserve a page-aligned range in the task's mmap VA window (MMAP_BASE..
// MMAP_END). No physical frames are committed — the page fault handler
// (task_mmap_handle_fault) lazily maps a fresh zeroed frame per page on first
// access. Returns the base VA or 0 on failure.
uint32_t task_mmap_reserve(uint32_t size) {
    int cid = get_cid();
    if (current_task[cid] < 0) return 0;
    int tid = current_task[cid];
    if (size == 0) return 0;

    size = (size + 0xFFF) & ~0xFFFu;
    if (size == 0) size = 4096;
    if (size > (MMAP_END - MMAP_BASE)) return 0;
    // RLIMIT_AS (v38.28): the reservation counts against the task's soft
    // address-space limit. Check BEFORE the region slot is consumed.
    if (!task_rlimit_as_allows(size)) return 0;

    int slot;
    uint32_t base;
    if (!mmap_region_alloc(tid, size, &slot, &base)) return 0;

    tasks[tid].mmap_regions[slot].base = base;
    tasks[tid].mmap_regions[slot].size = size;
    tasks[tid].mmap_regions[slot].file_size = 0;
    tasks[tid].mmap_regions[slot].vfs_node = -1;
    tasks[tid].mmap_regions[slot].map_flags = 0;
    tasks[tid].mmap_regions[slot].dirty = NULL;

    write_serial_string("[MMAP] reserved ");
    write_serial_hex(base);
    write_serial_string(" size=");
    write_serial_hex(size);
    write_serial_string(" for TID ");
    write_serial_hex(tid);
    write_serial_string("\n");
    return base;
}

// Map an open VFS FILE fd into the task's mmap window. No bytes are read yet:
// each page faults in FROM THE DISK on first access (task_mmap_handle_fault).
// The mapping records the VFS node itself, so closing the fd afterwards is
// legal. Flags: MMAP_FILE_SHARED (dirty pages write back on msync/munmap).
// Returns the base VA or 0 on failure.
uint32_t task_mmap_file(int fd, int flags) {
    int cid = get_cid();
    if (current_task[cid] < 0) return 0;
    int tid = current_task[cid];
    if (flags != MMAP_FILE_SHARED) return 0;

    // Resolve the fd to a VFS node. Only plain FS_FILE nodes can back a
    // mapping (ext2/proc/dev are rejected for now).
    extern global_fd_t global_fds[];
    int gfd = task_get_fd(tid, fd);
    if (gfd < 0 || gfd >= MAX_GLOBAL_FDS || !global_fds[gfd].in_use) return 0;
    if (global_fds[gfd].type != FD_TYPE_FILE) return 0;
    int node = global_fds[gfd].vfs_node;
    if (node < 0 || node >= MAX_NODES || !fs_nodes[node].in_use) return 0;
    if (fs_nodes[node].type != FS_FILE) return 0;

    uint32_t file_size = (fs_nodes[node].size > 0) ? (uint32_t)fs_nodes[node].size : 0;
    uint32_t size = (file_size + 0xFFF) & ~0xFFFu;
    if (size == 0) size = 4096;
    if (size > (MMAP_END - MMAP_BASE)) return 0;
    // RLIMIT_AS (v38.28): file-backed reservations count against the soft AS
    // limit like anonymous ones.
    if (!task_rlimit_as_allows(size)) return 0;

    int slot;
    uint32_t base;
    if (!mmap_region_alloc(tid, size, &slot, &base)) return 0;

    uint32_t npages = size / 4096;
    uint8_t* dirty = kmalloc((npages + 7) / 8);
    if (!dirty) return 0;
    memset(dirty, 0, (npages + 7) / 8);

    tasks[tid].mmap_regions[slot].base = base;
    tasks[tid].mmap_regions[slot].size = size;
    tasks[tid].mmap_regions[slot].file_size = file_size;
    tasks[tid].mmap_regions[slot].vfs_node = node;
    tasks[tid].mmap_regions[slot].map_flags = MMAP_FILE_SHARED;
    tasks[tid].mmap_regions[slot].dirty = dirty;

    write_serial_string("[MMAP] file map ");
    write_serial_hex(base);
    write_serial_string(" size=");
    write_serial_hex(size);
    write_serial_string(" node=");
    write_serial_hex((uint32_t)node);
    write_serial_string(" file_size=");
    write_serial_hex(file_size);
    write_serial_string(" for TID ");
    write_serial_hex(tid);
    write_serial_string("\n");
    return base;
}

// SYS_FB_MAP: map the VBE linear framebuffer READ/WRITE into the calling
// task's address space — the foundation a Ring 3 display server builds on.
// The mapping is DEVICE memory: every PTE carries PAGE_DEV, so fork never
// COWs or refcounts it (the clone walker skips PAGE_DEV entries; the frame
// address is MMIO far above the RAM bitmap), the free walkers skip it, and
// the child does not inherit the scanout. Pages are mapped EAGERLY (no
// demand-fill fault will ever fire for this VA range) with RW+USER and, per
// the v38.49 W^X policy, NX when EFER.NXE is active — pixel data must never
// be executable.
//
// Authorization (logind-style, not setuid-style): the scanout goes to the
// task that owns the ACTIVE console session — uid 0 (kernel services and the
// future compositor) or whoever is the controlling terminal's foreground
// process group right now. Background jobs are refused: they must not read
// pixels of windows they are not presenting. When Mectov grows real seats /
// multiuser this check narrows to the active SEAT; the call site is one if.
//
// The reservation goes through the normal mmap region table so it counts
// against RLIMIT_AS and is torn down on exec/exit like any other.
//
// Locking: fb_map_lock serializes the owner-check + reserve + eager-map
// sequence across cores (process-context discipline: irqsave, per
// docs/kernel-locking.md). It is always acquired BEFORE vmm_lock (taken
// inside vmm_map_page) and nowhere else, so no ordering cycle exists.

int task_fb_map(fb_info_t* uinfo) {
    extern uint32_t* fb_addr;
    extern uint32_t  fb_width, fb_height, fb_pitch;
    extern uint8_t   fb_bpp;
    extern int       is_vbe;
    extern int validate_user_ptr(const void* ptr, uint32_t size);

    if (!uinfo || !validate_user_ptr(uinfo, sizeof(fb_info_t))) return -1;
    int cid = get_cid();
    if (current_task[cid] < 0) return -1;
    int tid = current_task[cid];
    // A kernel task (page_dir 0 = boot identity map) must never take the
    // scanout: mapping device pages into the SHARED boot page tables would
    // expose them to every address space and outlive the caller.
    if (tasks[tid].page_dir == 0) return -1;
    if (!is_vbe || !fb_addr || fb_width == 0 || fb_height == 0) return -1;

    // Active-session authorization (see block comment above): root, or the
    // controlling terminal's foreground process group.
    // First check before lock for fast path, re-checked after lock to close
    // TOCTOU where fg switches between check and map.
    int fg = task_get_fg_pgrp();
    if (tasks[tid].uid != ROOT_UID &&
        !(fg > 0 && tasks[tid].pgrp == fg)) {
        write_serial_string("[FBMAP] denied: tid ");
        write_serial_hex((uint32_t)tid);
        write_serial_string(" is not the active session (uid!=0, pgrp!=fg)\n");
        return -1;
    }

    // One page-aligned span covering the whole scanout buffer.
    uint32_t fb_bytes = fb_pitch * fb_height;
    uint32_t size = (fb_bytes + 0xFFF) & ~0xFFFu;
    if (size == 0 || size > (MMAP_END - MMAP_BASE)) return -1;
    if (!task_rlimit_as_allows(size)) return -1;

    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&fb_map_lock);

    // Re-check auth after taking the lock (closes TOCTOU vs concurrent
    // task_set_fg_pgrp from shell run).
    fg = task_get_fg_pgrp();
    if (tasks[tid].uid != ROOT_UID &&
        !(fg > 0 && tasks[tid].pgrp == fg)) {
        write_serial_string("[FBMAP] denied (raced fg): tid ");
        write_serial_hex((uint32_t)tid);
        write_serial_string("\n");
        goto out_busy;
    }

    // A display server is a singleton for now: one live fb mapping at a time
    // keeps "who owns the scanout" unambiguous. Re-map by the same task is
    // idempotent — return the existing mapping instead of leaking a new slot.
    if (fb_owner_tid == tid) {
        for (int j = 0; j < MMAP_MAX_REGIONS; j++) {
            if (tasks[tid].mmap_regions[j].map_flags == MMAP_FLAG_DEVICE &&
                tasks[tid].mmap_regions[j].base != 0) {
                uinfo->base   = tasks[tid].mmap_regions[j].base;
                uinfo->width  = fb_width;
                uinfo->height = fb_height;
                uinfo->pitch  = fb_pitch;
                uinfo->bpp    = fb_bpp;
                spin_unlock(&fb_map_lock);
                __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
                return 0;
            }
        }
    }
    if (fb_owner_tid >= 0 && fb_owner_tid != tid && task_is_alive(fb_owner_tid)) {
        write_serial_string("[FBMAP] busy: held by TID ");
        write_serial_hex((uint32_t)fb_owner_tid);
        write_serial('\n');
        goto out_busy;
    }

    int slot;
    uint32_t base;
    if (!mmap_region_alloc(tid, size, &slot, &base)) goto out_busy;

    tasks[tid].mmap_regions[slot].base = base;
    tasks[tid].mmap_regions[slot].size = size;
    tasks[tid].mmap_regions[slot].file_size = 0;
    tasks[tid].mmap_regions[slot].vfs_node = -1;
    tasks[tid].mmap_regions[slot].map_flags = MMAP_FLAG_DEVICE;
    tasks[tid].mmap_regions[slot].dirty = NULL;

    uint32_t nx = paging_nx_enabled() ? PAGE_NX : 0;
    uint64_t flags = PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_DEV | nx;
    for (uint32_t off = 0; off < size; off += 4096) {
        if (vmm_map_page(tasks[tid].page_dir, base + off,
                         (uint32_t)(uintptr_t)fb_addr + off, flags) != 0) {
            // Roll back what was mapped so no half-window survives.
            for (uint32_t undo = 0; undo < off; undo += 4096)
                vmm_unmap_page(tasks[tid].page_dir, base + undo);
            tasks[tid].mmap_regions[slot].base = 0;
            tasks[tid].mmap_regions[slot].size = 0;
            goto out_busy;
        }
    }
    fb_owner_tid = tid;

    uinfo->base   = base;
    uinfo->width  = fb_width;
    uinfo->height = fb_height;
    uinfo->pitch  = fb_pitch;
    uinfo->bpp    = fb_bpp;

    write_serial_string("[FBMAP] tid=");
    write_serial_hex((uint32_t)tid);
    write_serial_string(" va=");
    write_serial_hex(base);
    write_serial_string(" pa=");
    write_serial_hex((uint32_t)(uintptr_t)fb_addr);
    write_serial_string(" wxh=");
    write_serial_hex(fb_width);
    write_serial('x');
    write_serial_hex(fb_height);
    write_serial_string(" bpp=");
    write_serial_hex(fb_bpp);
    write_serial('\n');
    spin_unlock(&fb_map_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
    return 0;

out_busy:
    spin_unlock(&fb_map_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
    return -1;
}

// SYS_FB_RELEASE: give the scanout back. Unmaps the device pages from the
// caller's address space, frees the region slot, and clears ownership so the
// kernel main loop repaints the desktop on its next tick. Only the current
// owner may release (a non-owner cannot yank someone else's scanout).
int task_fb_release(void) {
    int cid = get_cid();
    if (current_task[cid] < 0) return -1;
    int tid = current_task[cid];

    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&fb_map_lock);

    if (fb_owner_tid != tid) {
        spin_unlock(&fb_map_lock);
        __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
        return -1;
    }

    // Find this task's device region and tear it down page by page.
    for (int j = 0; j < MMAP_MAX_REGIONS; j++) {
        mmap_region_t* r = &tasks[tid].mmap_regions[j];
        if (r->base == 0 || r->map_flags != MMAP_FLAG_DEVICE) continue;
        for (uint32_t off = 0; off < r->size; off += 4096)
            vmm_unmap_page(tasks[tid].page_dir, r->base + off);
        r->base = 0;
        r->size = 0;
        r->map_flags = 0;
        break;  // singleton ownership: exactly one device region exists
    }

    fb_owner_tid = -1;
    spin_unlock(&fb_map_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));

    write_serial_string("[FBMAP] released by tid ");
    write_serial_hex((uint32_t)tid);
    write_serial('\n');
    return 0;
}


// Copy one dirty page of task tid's region r into dst. Resolves the page's
// physical frame through the OWNER's page tables and copies it via the kernel
// identity map, so it works whether or not tid's address space is the active
// one. Caller guarantees tid cannot run/unmap concurrently (see the flush
// comment above); a dirty page is always present, so a missing PTE is a
// defensive no-op that leaves the on-disk bytes in place.
static void mmap_copy_page_from_owner(int tid, mmap_region_t* r, uint32_t page, char* dst, uint32_t len) {
    if (tasks[tid].page_dir == 0) return;   // kernel identity space: no user walk
    uint32_t va = r->base + page * 4096;
    pte_t* pdpt = (pte_t*)(uintptr_t)tasks[tid].page_dir;
    if (!(pdpt[pdpt_index(va)] & PAGE_PRESENT)) return;
    pte_t* pd = (pte_t*)(uintptr_t)(uint32_t)(pdpt[pdpt_index(va)] & PTE_ADDR_MASK);
    if (!(pd[pd_index(va)] & PAGE_PRESENT)) return;
    pte_t* pt = (pte_t*)(uintptr_t)(uint32_t)(pd[pd_index(va)] & PTE_ADDR_MASK);
    if (!(pt[pt_index(va)] & PAGE_PRESENT)) return;
    uint32_t paddr = (uint32_t)(pt[pt_index(va)] & PTE_ADDR_MASK);
    memcpy(dst, (void*)(uintptr_t)paddr, len);
}

// Downgrade one mapped page of task tid's region r back to READ-ONLY. The
// dirty tracker fires on the RO→RW write fault, so after a write-back clears
// a page's dirty bit the page MUST be RO again or the next write would slip
// through without re-arming it (a silent data-loss window until the next
// flush). In the self context (msync/fsync of the caller's own mapping) the
// invlpg flushes our own TLB. In a cross-task flush the owner is not running
// and its next context switch reloads CR3 — this OS has no TLB-shootdown IPI
// (vmm.c v38.55 note), so the stale RW TLB entry cannot outlive the owner's
// current run slice either way.
static void mmap_page_make_ro(int tid, mmap_region_t* r, uint32_t page) {
    if (tasks[tid].page_dir == 0) return;   // kernel identity space: no user walk
    uint32_t va = r->base + page * 4096;
    pte_t* pdpt = (pte_t*)(uintptr_t)tasks[tid].page_dir;
    if (!(pdpt[pdpt_index(va)] & PAGE_PRESENT)) return;
    pte_t* pd = (pte_t*)(uintptr_t)(uint32_t)(pdpt[pdpt_index(va)] & PTE_ADDR_MASK);
    if (!(pd[pd_index(va)] & PAGE_PRESENT)) return;
    pte_t* pt = (pte_t*)(uintptr_t)(uint32_t)(pd[pd_index(va)] & PTE_ADDR_MASK);
    if (!(pt[pt_index(va)] & PAGE_PRESENT)) return;
    pt[pt_index(va)] &= ~PAGE_RW;
    __asm__ __volatile__("invlpg (%0)" : : "r"(va));
}

// Write every dirty page of a file-backed mapping back to its VFS file.
//
// Since v38.61 this runs in three contexts, all holding the same invariant —
// the owning task's page tables cannot mutate while we read them:
//   1. self-context (msync/munmap syscalls): the owner is the caller, so it
//      cannot concurrently unmap or fault (single-threaded syscall).
//   2. fsync()/sync() syscalls: the caller holds task_lock and flushes only
//      regions whose owner is NOT currently running on another core (a task
//      mutates its mappings only while running) or IS the caller itself.
//   3. the kernel's periodic write-back (BSP main loop, task 0): same as 2.
// In contexts 2/3 the caller holds task_lock with IF=0, so the owner cannot
// be scheduled either; the VFS calls below take vfs_lock (documented order
// task_lock > vfs_lock), and vfs_lock holders run non-preemptible (irqsave),
// so no lock cycle exists.
//
// Dirty page CONTENT is read through the OWNER's page tables — not the
// caller's address space, which differs — via the kernel identity map
// (0..RAM = phys, so a physical frame is directly addressable). That is what
// lets a flush of some OTHER task's mapping work without switching CR3. The
// file's CURRENT size is the write-back bound: bytes a task wrote past EOF
// (pages beyond the original file size) are dropped, matching POSIX — a
// MAP_SHARED mapping never grows a file by itself; growth comes from
// write()/ftruncate.
//
// Returns 1 when dirty pages were written back (or dropped because the file
// vanished), 0 when the region was clean, -1 when the flush failed.
static int mmap_flush_file(int tid, mmap_region_t* r) {
    uint32_t npages = r->size / 4096;
    int any = 0;
    for (uint32_t i = 0; i < npages; i++) {
        if (r->dirty[i / 8] & (1u << (i % 8))) { any = 1; break; }
    }
    if (!any) return 0;

    uint32_t file_size = 0;
    if (r->vfs_node >= 0 && r->vfs_node < MAX_NODES && fs_nodes[r->vfs_node].in_use) {
        file_size = (uint32_t)fs_nodes[r->vfs_node].size;
    }
    if (file_size == 0) {
        // File is gone or empty: nothing can be persisted. Drop the dirty
        // marks (the bytes are lost, like an unlinked mapping's contents).
        memset(r->dirty, 0, (npages + 7) / 8);
        return 1;
    }

    char path[256];
    if (vfs_get_abs_path(r->vfs_node, path, 256) < 0) return -1;
    char* buf = kmalloc(file_size);
    if (!buf) return -1;
    // Start from the current on-disk content so bytes never written through
    // this mapping (another fd, another process) are preserved.
    int cur = vfs_read_file(path, buf, (int)file_size);
    if (cur < 0) cur = 0;
    for (uint32_t i = 0; i < npages; i++) {
        if (r->dirty[i / 8] & (1u << (i % 8))) {
            uint32_t off = i * 4096;
            if (off >= file_size) continue;  // past EOF: never persisted
            uint32_t copylen = 4096;
            if (copylen > file_size - off) copylen = file_size - off;
            mmap_copy_page_from_owner(tid, r, i, buf + off, copylen);
        }
    }
    int w = vfs_write_file(path, buf, (int)file_size);
    kfree(buf);
    if (w < 0) return -1;
    // Re-arm the dirty tracker: every page just written back goes RO again so
    // a later write faults and re-sets its bit (the RO→RW fault IS the dirty
    // mark). Without this, one dirtying per mapping lifetime is all the
    // tracker ever sees and every write after the first flush is silently
    // invisible to fsync/periodic write-back. Bytes past EOF stay RW — they
    // are never persisted anyway (mapping cannot grow the file).
    for (uint32_t i = 0; i < npages; i++) {
        if (!(r->dirty[i / 8] & (1u << (i % 8)))) continue;
        if (i * 4096 >= file_size) continue;
        mmap_page_make_ro(tid, r, i);
    }
    memset(r->dirty, 0, (npages + 7) / 8);
    write_serial_string("[MMAP] flushed ");
    write_serial_hex(r->base);
    write_serial_string(" size=");
    write_serial_hex(file_size);
    write_serial_string("\n");
    return 1;
}

// Flush dirty pages of a file-backed mapping back to its file (msync).
// Returns 1 on success (or nothing to write), 0 on failure.
uint32_t task_msync(uint32_t addr) {
    int cid = get_cid();
    if (current_task[cid] < 0) return 0;
    int tid = current_task[cid];

    for (int j = 0; j < MMAP_MAX_REGIONS; j++) {
        mmap_region_t* r = &tasks[tid].mmap_regions[j];
        if (r->base != addr) continue;
        if (r->map_flags != MMAP_FILE_SHARED) return 1; // nothing to sync
        write_serial_string("[MMAP] msync ");
        write_serial_hex(addr);
        write_serial_string("\n");
        int rc = mmap_flush_file(tid, r);
        return (rc < 0) ? 0 : 1;
    }
    return 0;
}

// ---- v38.61 fsync()/sync(): flush dirty file-backed mmap pages on demand --
//
// Everything else in the I/O stack (MECTOVFS, ext2, FAT32, the pcache) is
// synchronous write-through, so dirty file mmap pages are the ONLY data that
// can be lost on a power cut before msync/munmap. task_sync_dirty() flushes
// every flushable dirty region (fsync: restricted to one VFS node).
//
// Flushability: a region is flushed only when reading its frames is safe —
// owner == caller, or owner is not currently running on any core (and, under
// task_lock with IF=0, cannot be scheduled until the flush completes).
// Regions owned by a task running elsewhere right now are SKIPPED and left
// for the next sync/period — the alternative (racing its page faults or
// munmap) is worse than waiting one interval.
//
// Caller must NOT hold task_lock. Returns the number of regions flushed
// (>0 means something was written back), -1 only on caller error.
static int task_running_on_any_cpu(int tid) {
    for (int c = 0; c < 16; c++) {
        if (current_task[c] == tid) return 1;
    }
    return 0;
}

static int task_sync_dirty(int node_filter) {
    int cid = get_cid();
    int self = (cid >= 0 && current_task[cid] >= 0) ? current_task[cid] : -1;
    int flushed = 0;

    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&task_lock);
    for (int tid = 0; tid < MAX_TASKS; tid++) {
        if (tasks[tid].is_idle || tasks[tid].page_dir == 0) continue;
        if (tasks[tid].state == TASK_STATE_FREE || tasks[tid].state == TASK_STATE_ZOMBIE) continue;
        if (tid != self && task_running_on_any_cpu(tid)) continue;  // owner busy: retry later
        for (int j = 0; j < MMAP_MAX_REGIONS; j++) {
            mmap_region_t* r = &tasks[tid].mmap_regions[j];
            if (r->base == 0 || r->map_flags != MMAP_FILE_SHARED || !r->dirty) continue;
            if (node_filter >= 0 && r->vfs_node != node_filter) continue;
            int rc = mmap_flush_file(tid, r);
            if (rc > 0) flushed++;
        }
    }
    spin_unlock(&task_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
    return flushed;
}

// fsync(fd): flush dirty mmap pages of the file behind the calling task's fd.
// Returns 0 on success (or nothing pending), -1 when the fd is invalid.
int task_fsync_fd(int fd) {
    int cid = get_cid();
    if (cid < 0 || current_task[cid] < 0) return -1;
    int tid = current_task[cid];

    extern global_fd_t global_fds[];
    int gfd = task_get_fd(tid, fd);
    if (gfd < 0 || gfd >= MAX_GLOBAL_FDS || !global_fds[gfd].in_use) return -1;
    if (global_fds[gfd].type != FD_TYPE_FILE) return 0;  // pipes/dev: nothing pending
    int node = global_fds[gfd].vfs_node;
    if (node < 0 || node >= MAX_NODES || !fs_nodes[node].in_use) return -1;
    // ext2/FAT32/proc/dev file data is synchronous already — only FS_FILE
    // (MECTOVFS) nodes can carry dirty mmap pages.
    if (fs_nodes[node].type != FS_FILE) return 0;
    task_sync_dirty(node);
    return 0;
}

// sync(): flush every flushable dirty region system-wide. Returns 0.
int task_sync_all(void) {
    task_sync_dirty(-1);
    return 0;
}

// Handle a page fault inside a reserved mmap region. Anonymous mappings get a
// fresh zeroed frame mapped writable. File-backed mappings pull the page's
// bytes straight off the disk (vfs_read_file_offset — no vfs_lock, so a fault
// taken while another path holds vfs_lock cannot self-deadlock) and map the
// page read-only; the FIRST WRITE then faults again, marks the page dirty and
// upgrades it to writable — that RO→RW transition is the dirty tracker.
// File pages carry PAGE_SHARED so fork() leaves them shared, never COW'd.
// Returns 1 if handled (resume), 0 if not ours.
int task_mmap_handle_fault(uint32_t addr, uint32_t cr3, uint32_t err) {
    int cid = get_cid();
    if (current_task[cid] < 0) return 0;
    int tid = current_task[cid];
    int write_fault = (err & 2) != 0;

    for (int j = 0; j < MMAP_MAX_REGIONS; j++) {
        mmap_region_t* r = &tasks[tid].mmap_regions[j];
        uint32_t b = r->base;
        if (b == 0) continue;
        if (addr < b || addr >= b + r->size) continue;

        uint32_t va = addr & ~0xFFFu;
        pte_t* pdpt = (pte_t*)(uintptr_t)cr3;

        // Page already present: only a write to a read-only file page needs
        // action (mark dirty + upgrade to writable). A present page can never
        // be missing its entry, so this early-out is just the RO→RW upgrade
        // for file pages and a safety net for everything else.
        if (pdpt[pdpt_index(va)] & PAGE_PRESENT) {
            pte_t* pd = (pte_t*)(uintptr_t)(uint32_t)(pdpt[pdpt_index(va)] & PTE_ADDR_MASK);
            if (pd[pd_index(va)] & PAGE_PRESENT) {
                pte_t* pt = (pte_t*)(uintptr_t)(uint32_t)(pd[pd_index(va)] & PTE_ADDR_MASK);
                if (pt[pt_index(va)] & PAGE_PRESENT) {
                    if (write_fault && !(pt[pt_index(va)] & PAGE_RW)) {
                        if (r->map_flags == MMAP_FILE_SHARED) {
                            uint32_t page = (va - b) / 4096;
                            r->dirty[page / 8] |= (uint8_t)(1u << (page % 8));
                        }
                        pt[pt_index(va)] |= PAGE_RW;
                        __asm__ __volatile__("invlpg (%0)" : : "r"(va));
                    }
                    return 1;
                }
            }
        }

        uint32_t phys = frame_alloc();
        if (phys == 0) {
            write_serial_string("[MMAP] OOM on fault at ");
            write_serial_hex(addr);
            write_serial_string("\n");
            return 0;  // let it become a crash
        }

        if (r->map_flags == MMAP_FILE_SHARED) {
            // Lazy file backing: zero the frame, then read THIS page's bytes
            // from the disk at the matching file offset.
            memset((void*)(uintptr_t)phys, 0, 4096);
            uint32_t off = va - b;
            if (off < r->file_size) {
                int take = r->file_size - off;
                if (take > 4096) take = 4096;
                int got = vfs_read_file_offset(r->vfs_node, (int)off, (char*)(uintptr_t)phys, take);
                if (got < 0) {
                    frame_free(phys);
                    write_serial_string("[MMAP] file read failed on fault\n");
                    return 0;
                }
            }
            uint32_t flags = PAGE_PRESENT | PAGE_USER | PAGE_SHARED;
            if (write_fault) {
                flags |= PAGE_RW;
                uint32_t page = (va - b) / 4096;
                r->dirty[page / 8] |= (uint8_t)(1u << (page % 8));
            }
            vmm_map_page(cr3, va, phys, flags);
            write_serial_string("[MMAP] file paged ");
            write_serial_hex(va);
            write_serial_string("\n");
            return 1;
        }

        // Anonymous: zero-filled writable frame, as before.
        vmm_map_page(cr3, va, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
        memset((void*)(uintptr_t)va, 0, 4096);
        write_serial_string("[MMAP] demand paged ");
        write_serial_hex(va);
        write_serial_string(" for TID ");
        write_serial_hex(tid);
        write_serial_string("\n");
        return 1;
    }
    return 0;
}

// Unmap a reserved region: flush dirty pages to the backing file (file
// mappings), free any frames already faulted in, then release the
// reservation. addr must be the exact base returned by mmap()/mmap_file().
uint32_t task_munmap(uint32_t addr) {
    int cid = get_cid();
    if (current_task[cid] < 0) return 0;
    int tid = current_task[cid];
    uint32_t page_dir = tasks[tid].page_dir;

    for (int j = 0; j < MMAP_MAX_REGIONS; j++) {
        mmap_region_t* r = &tasks[tid].mmap_regions[j];
        if (r->base != addr) continue;
        uint32_t size = r->size;

        // File-backed: write dirty pages back to the file before dropping
        // the frames, then free the dirty bitmap.
        if (r->map_flags == MMAP_FILE_SHARED) {
            mmap_flush_file(tid, r);
            if (r->dirty) {
                kfree(r->dirty);
                r->dirty = NULL;
            }
        }

        // Free every frame that was faulted in. The page fault handler maps
        // exactly the pages in [base, base+size), so walk the PTEs.
        for (uint32_t va = addr; va < addr + size; va += 4096) {
            pte_t* pdpt = (pte_t*)(uintptr_t)page_dir;
            if (!(pdpt[pdpt_index(va)] & PAGE_PRESENT)) continue;
            pte_t* pd = (pte_t*)(uintptr_t)(uint32_t)(pdpt[pdpt_index(va)] & PTE_ADDR_MASK);
            if (!(pd[pd_index(va)] & PAGE_PRESENT)) continue;
            pte_t* pt = (pte_t*)(uintptr_t)(uint32_t)(pd[pd_index(va)] & PTE_ADDR_MASK);
            if (pt[pt_index(va)] & PAGE_PRESENT) {
                uint32_t paddr = (uint32_t)(pt[pt_index(va)] & PTE_ADDR_MASK);
                if (paddr >= (KERNEL_RESERVED_PAGES * 4096)) frame_free(paddr);
                pt[pt_index(va)] = 0;
                __asm__ __volatile__("invlpg (%0)" : : "r"(va));
            }
        }

        memset(r, 0, sizeof(*r));
        write_serial_string("[MMAP] unmapped ");
        write_serial_hex(addr);
        write_serial_string(" for TID ");
        write_serial_hex(tid);
        write_serial_string("\n");
        return 1;
    }
    return 0;
}
