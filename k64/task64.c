/* M4 preemptive multitasking — M5 per-task PML4 — M7 symmetric SMP.
 *
 * Every user task owns a PRIVATE address space cloned from the boot
 * template (kernel half shared by pointer, never written by tasks). Each
 * task also owns a 16KB kernel stack, a 16KB user stack (4KB guard hole
 * below), and a 512B eager FPU image. task[0] is the boot context itself —
 * its kernel stack IS the boot stack. fork() COW-shares pages, clone()
 * shares them truly.
 *
 * M7 LOCKING (sched_lock guards the task table + all task states):
 * - alloc reserves a slot as T_NEW (invisible to scheduling/waiting);
 *   wiring (mem ops) happens unlocked; publish flips READY at the end.
 * - schedule/exit/wait/sleep mutate states only under sched_lock.
 * - Slow teardown (frames + shootdown waits) runs OUTSIDE sched_lock;
 *   zombies/claimed slots are invisible to other CPUs meanwhile.
 * - Order: sched_lock -> mem_lock, never reverse (mem paths never take
 *   sched_lock; shootdown waits hold NO lock — see mem64.c header).
 * - `cur` is per-CPU (cur_cpu[]) — every core runs its own schedule loop.
 */
#include "cpu64.h"
#include "spin64.h"

#define NTASK 32
#define KSTACK_SIZE 16384

static task64_t tasks[NTASK];
static task64_t *volatile cur_cpu[NCPU_MAX];
static u8 kstacks[NTASK][KSTACK_SIZE] __attribute__((aligned(16)));
static volatile u64 ticks = 0;
static spin64_t sched_lock = SPIN64_INIT;
/* M7.1 per-CPU idle tasks: parking INSIDE an IRQ handler (sti/hlt/cli in
 * schedule) nests one frame per tick and overflows the kstack in <1s once
 * the system goes idle (seen live: wild RIP=0x3 #UD after demos finished).
 * Instead schedule always switches — to a real task, or here — so every
 * IRQ unwinds fully. Idle bodies are hlt loops in TASK context (fixed
 * depth, IF=1 from their 0x202 frames). Not in tasks[]: never scanned,
 * never waited on, never reaped. */
static task64_t idle_tasks[NCPU_MAX];
static u8 idle_stacks[NCPU_MAX][4096] __attribute__((aligned(16)));
/* M7.1 DEBUG: last sched_lock owner (site RIP + cpu), readable via QEMU
 * monitor when wedged. Remove once the wedge is found. */
volatile u64 sched_owner_rip = 0;
volatile int sched_owner_cpu = -9;
#define SCHED_LOCK() ({ u64 _f = spin64_lock_irqsave(&sched_lock); sched_owner_cpu = smp_cpu_index(); sched_owner_rip = (u64)__builtin_return_address(0); _f; })
#define SCHED_UNLOCK(f) do { sched_owner_cpu = -9; sched_owner_rip = 0; spin64_unlock_irqrestore(&sched_lock, (f)); } while (0)

#define cur (cur_cpu[smp_cpu_index()])

extern char stack_top64[];
extern u64 pml4_boot[];
extern void tss64_set_rsp0_cpu(int cpu, u64 rsp0);

/* Temporarily run on another address space (IF=0 required: no scheduler
 * interference while a foreign PML4 is loaded). Returns the previous CR3. */
static u64 cr3_swap(u64 new_cr3) {
    u64 old = cpu_read_cr3() & ~0xFFFULL;
    if (new_cr3 != old) cpu_load_cr3(new_cr3);
    return old;
}

u64 k64_ticks(void) {
    return ticks;
}

int task64_current_id(void) {
    task64_t *c = cur;
    return c ? c->id : -1;
}

task64_t *task64_self(void) {
    return cur;
}

static void fx_init(task64_t *t) {
    __asm__ __volatile__("fninit");
    __asm__ __volatile__("fxsave %0" : "=m"(t->fx));
}

static int namecpy(char *d, const char *s) {
    int i = 0;
    for (; i < 15 && s[i]; i++) d[i] = s[i];
    d[i] = '\0';
    return i;
}

/* Reserve a slot (T_NEW: invisible until published). Caller publishes. */
static task64_t *alloc_slot(const char *name) {
    u64 f = SCHED_LOCK();
    task64_t *t = 0;
    for (int i = 1; i < NTASK; i++) {
        if (tasks[i].state == T_FREE) {
            tasks[i].id = i;
            tasks[i].state = T_NEW;
            tasks[i].rsp = 0;
            tasks[i].user_base = 0;
            tasks[i].cr3 = 0;
            tasks[i].kstack_top = (u64)(kstacks[i] + KSTACK_SIZE);
            tasks[i].parent = -1;
            tasks[i].waiting_for = -2;
            tasks[i].wakeup_tick = 0;
            tasks[i].exit_code = 0;
            tasks[i].last_cpu = -1;
            tasks[i].heap_base = 0;
            tasks[i].heap_brk = 0;
            namecpy(tasks[i].name, name);
            fx_init(&tasks[i]);
            t = &tasks[i];
            break;
        }
    }
    SCHED_UNLOCK(f);
    return t;
}

static void task_publish(task64_t *t) {
    u64 f = SCHED_LOCK();
    if (t->state == T_NEW) t->state = T_READY;
    SCHED_UNLOCK(f);
}

static void task_unreserve(task64_t *t) {
    u64 f = SCHED_LOCK();
    if (t->state == T_NEW) t->state = T_FREE;
    SCHED_UNLOCK(f);
}

static u64 ustack_top(int id) {
    return USTACK_BASE + (u64)id * USTACK_SLOT + USTACK_SLOT;
}

/* Map a 16KB user stack with a 4KB guard hole beneath it. Caller holds
 * mem_lock and has vmm_root pointed at the target space. */
static int map_ustack_locked(int id) {
    u64 top = ustack_top(id);
    for (int i = 0; i < 4; i++) {
        u64 pa = __pmm_alloc();
        if (!pa) return -1;
        if (__vmm_map_page(top - USTACK_SIZE + (u64)i * 4096, pa,
                           VMM_RW | VMM_US)) {
            __frame_ref_put(pa);
            return -1;
        }
    }
    return 0;
}
/* Fabricate the frame isr64_common's pop sequence + iretq consume:
 * [15 GPRs][vec][err][rip][cs][rflags][rsp][ss], task->rsp points at r15. */
static void build_frame(task64_t *t, u64 rip, u64 cs, u64 rsp, u64 ss) {
    u64 *sp = (u64 *)t->kstack_top;
    sp -= 5; /* [rip][cs][rflags][rsp][ss] */
    sp[0] = rip;
    sp[1] = cs;
    sp[2] = 0x202; /* IF=1 */
    sp[3] = rsp;
    sp[4] = ss;
    sp -= 2; /* [vec][err] */
    sp[0] = 0;
    sp[1] = 0;
    /* 15 GPR slots (popped r15..rax order, top of stack = r15). */
    for (int i = 0; i < 15; i++) *(--sp) = 0;
    t->rsp = (u64)sp;
}

/* The idle body: hlt with IF=1 (from the 0x202 entry frame). IRQs preempt
 * normally and unwind back here — fixed stack depth, forever. Never
 * returns; not in tasks[] (never scanned/waited/reaped). */
void idle_main(void) {
    for (;;) __asm__ __volatile__("hlt");
}

void task64_init(void) {
    for (int i = 0; i < NTASK; i++) {
        tasks[i].id = i;
        tasks[i].state = T_FREE;
        tasks[i].rsp = 0;
        tasks[i].kstack_top = 0;
        tasks[i].user_base = 0;
        tasks[i].cr3 = 0;
        tasks[i].parent = -1;
        tasks[i].waiting_for = -2;
        tasks[i].wakeup_tick = 0;
        tasks[i].exit_code = 0;
        tasks[i].last_cpu = -1;
        tasks[i].heap_base = 0;
        tasks[i].heap_brk = 0;
        tasks[i].name[0] = '\0';
    }
    for (int i = 0; i < NCPU_MAX; i++) cur_cpu[i] = 0;
    /* task[0] = boot context: runs on the boot stack, rsp captured at the
     * first timer tick; it is a fully symmetric task after that. */
    tasks[0].state = T_RUNNING;
    tasks[0].kstack_top = (u64)stack_top64;
    tasks[0].cr3 = (u64)pml4_boot;
    tasks[0].parent = -1;
    tasks[0].last_cpu = 0;
    namecpy(tasks[0].name, "idle");
    fx_init(&tasks[0]);
    cur_cpu[0] = &tasks[0];
    /* Per-CPU idle tasks (M7.1): entry frames = kernel hlt loops. */
    extern u64 pml4_boot[];
    for (int i = 0; i < NCPU_MAX; i++) {
        task64_t *t = &idle_tasks[i];
        t->id = 100 + i;
        t->state = T_READY;
        t->rsp = 0;
        t->kstack_top = (u64)(idle_stacks[i] + sizeof(idle_stacks[i]));
        t->user_base = 0;
        t->cr3 = (u64)pml4_boot;
        t->parent = -1;
        t->waiting_for = -2;
        t->wakeup_tick = 0;
        t->exit_code = 0;
        t->last_cpu = i;
        namecpy(t->name, "cpu-idle");
        fx_init(t);
        build_frame(t, (u64)idle_main, 0x08, t->kstack_top, 0x10);
    }
}

/* Spawn an embedded image by registry name with a PRIVATE address space:
 * clone the boot template, load via the MCT2/ELF64 loader, wire a fresh
 * user stack, and fabricate the Ring-3 entry frame. Wiring holds mem_lock
 * (vmm_root is global); the publish flush afterwards needs none. */
int task64_spawn_image(const char *kname) {
    u64 len = 0;
    const u8 *img = exec_lookup(kname, &len);
    if (!img) return -1;
    task64_t *t = alloc_slot(kname);
    if (!t) return -1;
    u64 child = vmm_clone_space(cpu_read_cr3() & ~0xFFFULL);
    if (!child) { task_unreserve(t); return -1; }
    t->cr3 = child;
    t->parent = 0;
    trace_cr3('N', t->id, child);
    u64 entry = 0, base = 0, img_end = 0;
    if (loader_map_image(child, img, len, &entry, &base, &img_end)) {
        vmm_teardown_space(child);
        task_unreserve(t);
        return -1;
    }
    u64 mf = mem_lock_acquire();
    u64 *saved_root = vmm_get_root();
    vmm_set_root((u64 *)child);
    int ok = (map_ustack_locked(t->id) == 0);
    vmm_set_root(saved_root);
    mem_lock_release(mf);
    if (!ok) {
        vmm_teardown_space(child);
        task_unreserve(t);
        return -1;
    }
    /* New VA mappings at reused image bases: flush APs (publish). */
    vmm_publish();
    t->user_base = base;
    t->heap_base = t->heap_brk = (img_end + 4095) & ~4095ULL;
    build_frame(t, entry, 0x1B, ustack_top(t->id), 0x23);
    task_publish(t);
    return t->id;
}

/* M7.2 spawn with argv (shell `run`). kargv are kernel-side NUL-terminated
 * strings (each <=128, copied+validated by the syscall layer). The child
 * enters at base+DEMO_START_ARGS_OFF with argc/argv on its stack. */
int task64_spawn_args(const char *kname, int argc, char **kargv) {
    if (argc < 0 || argc > 16) return -22;
    u64 len = 0;
    const u8 *img = exec_lookup(kname, &len);
    if (!img) return -2;
    task64_t *t = alloc_slot(kname);
    if (!t) return -12;
    u64 child = vmm_clone_space(cpu_read_cr3() & ~0xFFFULL);
    if (!child) { task_unreserve(t); return -12; }
    t->cr3 = child;
    t->parent = cur ? cur->id : 0;
    u64 entry = 0, base = 0, img_end = 0;
    (void)entry;
    if (loader_map_image(child, img, len, &entry, &base, &img_end)) {
        vmm_teardown_space(child);
        task_unreserve(t);
        return -8;
    }
    u64 mf = mem_lock_acquire();
    u64 *saved_root = vmm_get_root();
    vmm_set_root((u64 *)child);
    int ok = (map_ustack_locked(t->id) == 0);
    vmm_set_root(saved_root);
    mem_lock_release(mf);
    if (!ok) {
        vmm_teardown_space(child);
        task_unreserve(t);
        return -12;
    }
    /* argv layout under the child's tables (IF=0 here). Total < 2.5KB.
     * _start_args pops argc/argv then CALLS args_main, so the entry RSP
     * must be 0 mod 16 (after call+push it is 8 mod 16 per SysV; anything
     * else misaligns the callee's movaps -> #GP. Seen live with argc=2:
     * aligned-24-16 left RSP 8 mod 16 at entry). Strings pack from the top;
     * the array+slots are reserved and the final RSP forced aligned — the
     * ≤15 pad bytes land in mapped stack, never in the guard hole. */
    u64 old = cr3_swap(child);
    u64 p = ustack_top(t->id);
    u64 strva[16];
    for (int i = 0; i < argc; i++) {
        u64 sl = 0;
        while (sl < 128 && kargv[i][sl]) sl++;
        p -= sl + 1;
        for (u64 k = 0; k <= sl; k++) ((volatile u8 *)p)[k] = (u8)kargv[i][k];
        strva[i] = p;
    }
    p = (p - ((u64)(argc + 1) * 8 + 16)) & ~15ULL;
    u64 argv_va = p + 16; /* [p]=argc [p+8]=argv, array follows */
    *(volatile u64 *)p = (u64)argc;
    *(volatile u64 *)(p + 8) = argv_va;
    for (int i = 0; i < argc; i++)
        *(volatile u64 *)(argv_va + (u64)i * 8) = strva[i];
    *(volatile u64 *)(argv_va + (u64)argc * 8) = 0;
    cr3_swap(old);
    vmm_publish();
    t->user_base = base;
    t->heap_base = t->heap_brk = (img_end + 4095) & ~4095ULL;
    build_frame(t, base + DEMO_START_ARGS_OFF, 0x1B, p, 0x23);
    task_publish(t);
    return t->id;
}

/* M7.2 ps snapshot for the shell (under sched_lock, bounded copy). */
int task64_ps(ps_entry_t *out, int max) {
    if (!out || max <= 0 || max > 64) return -22;
    u64 f = SCHED_LOCK();
    int n = 0;
    for (int i = 0; i < NTASK && n < max; i++) {
        if (tasks[i].state == T_FREE || tasks[i].state == T_NEW) continue;
        out[n].id = tasks[i].id;
        out[n].state = tasks[i].state;
        out[n].parent = tasks[i].parent;
        out[n].cpu = tasks[i].last_cpu;
        for (int k = 0; k < 16; k++) {
            out[n].name[k] = tasks[i].name[k];
            if (!tasks[i].name[k]) break;
        }
        n++;
    }
    SCHED_UNLOCK(f);
    return n;
}

/* M5 EXEC: replace the current image with a registry image (MCT2/ELF64).
 * Old user space is dropped (fresh clone from the boot template), stacks
 * are fresh, FPU is reset — POSIX exec semantics minus argv/env (M7.2). */
u64 task64_exec(const char *uname, regs64_t *r) {
    task64_t *self = cur;
    if (!self || self->id == 0) { r->rax = (u64)(long)-1; return (u64)r; }
    if (!vmm_user_ok((u64)uname, 1)) {
        r->rax = (u64)(long)-14;
        return (u64)r;
    }
    char kname[17];
    int i = 0;
    for (; i < 16; i++) {
        char c = ((volatile const char *)uname)[i];
        kname[i] = c;
        if (!c) break;
    }
    kname[16] = '\0';
    if (i == 16) { r->rax = (u64)(long)-22; return (u64)r; } /* EINVAL */
    u64 len = 0;
    const u8 *img = exec_lookup(kname, &len);
    if (!img) { r->rax = (u64)(long)-2; return (u64)r; } /* ENOENT */
    extern u64 pml4_boot[];
    u64 fresh = vmm_clone_space((u64)pml4_boot);
    if (!fresh) { r->rax = (u64)(long)-12; return (u64)r; } /* ENOMEM */
    u64 entry = 0, base = 0, img_end = 0;
    if (loader_map_image(fresh, img, len, &entry, &base, &img_end)) {
        vmm_teardown_space(fresh);
        r->rax = (u64)(long)-8; /* ENOEXEC */
        return (u64)r;
    }
    u64 mf = mem_lock_acquire();
    u64 *saved_root = vmm_get_root();
    vmm_set_root((u64 *)fresh);
    int ok = (map_ustack_locked(self->id) == 0);
    vmm_set_root(saved_root);
    mem_lock_release(mf);
    if (!ok) {
        vmm_teardown_space(fresh);
        r->rax = (u64)(long)-12;
        return (u64)r;
    }
    vmm_publish();
    /* Commit: run on the fresh space, drop the old one, enter the image. */
    u64 old = self->cr3;
    trace_cr3('E', self->id, fresh);
    cpu_load_cr3(fresh);
    self->cr3 = fresh;
    vmm_teardown_space(old);
    self->user_base = base;
    self->heap_base = self->heap_brk = (img_end + 4095) & ~4095ULL;
    u64 sf = SCHED_LOCK();
    namecpy(self->name, kname);
    SCHED_UNLOCK(sf);
    fx_init(self); /* exec resets FPU (POSIX) */
    r->rip = entry;
    r->cs = 0x1B;
    r->rflags = 0x202;
    r->rsp = ustack_top(self->id);
    r->ss = 0x23;
    r->rax = 0;
    r->rbx = r->rcx = r->rdx = r->rsi = r->rdi = r->rbp = 0;
    r->r8 = r->r9 = r->r10 = r->r11 = r->r12 = 0;
    r->r13 = r->r14 = r->r15 = 0;
    return (u64)r;
}

/* M4 CLONE: child shares every user page truly (tables duplicated, pages
 * RW-shared + refcounted), starts at `func` with a fresh user stack whose
 * return address is the parent image's _thread_exit. */
int task64_clone(u64 func) {
    task64_t *self = cur;
    if (!vmm_user_ok(func, 1) || !self) return -1;
    task64_t *t = alloc_slot("clone");
    if (!t) return -1;
    u64 child_pml4 = vmm_share_space(self->cr3);
    if (!child_pml4) { task_unreserve(t); return -1; }
    t->cr3 = child_pml4;
    t->parent = self->id;
    u64 mf = mem_lock_acquire();
    u64 *saved_root = vmm_get_root();
    vmm_set_root((u64 *)child_pml4);
    int ok = (map_ustack_locked(t->id) == 0);
    vmm_set_root(saved_root);
    mem_lock_release(mf);
    if (!ok) {
        vmm_teardown_space(child_pml4);
        task_unreserve(t);
        return -1;
    }
    vmm_publish();
    t->user_base = self->user_base;
    t->heap_base = self->heap_base;
    t->heap_brk = self->heap_brk;
    u64 csp = ustack_top(t->id) - 8;
    u64 old = cr3_swap(child_pml4);
    *(volatile u64 *)csp = self->user_base + DEMO_THREAD_EXIT_OFF;
    cr3_swap(old);
    build_frame(t, func, 0x1B, csp, 0x23);
    task_publish(t);
    return t->id;
}

void task64_exit(int status) {
    task64_t *self = cur;
    if (!self) {
        for (;;) __asm__ __volatile__("cli; hlt");
    }
    s_printf("[K64] task %u (%s) exited status=%u\n", (u64)self->id,
             self->name, (u64)(long)status);
    u64 f = SCHED_LOCK();
    self->exit_code = status;
    self->state = T_ZOMBIE;
    /* Orphans go to idle(0). */
    for (int i = 1; i < NTASK; i++)
        if (tasks[i].parent == self->id && tasks[i].state != T_FREE)
            tasks[i].parent = 0;
    task64_t *p =
        (self->parent >= 0 && self->parent < NTASK) ? &tasks[self->parent] : 0;
    if (p && p->state == T_BLOCKED &&
        (p->waiting_for == self->id || p->waiting_for == -1))
        p->state = T_READY; /* waiter reaps us on wakeup */
    int reap_now =
        !p || p->state == T_FREE || p->id == 0;
    SCHED_UNLOCK(f);
    if (reap_now) {
        /* Nobody will ever waitpid: reap now (teardown takes mem_lock +
         * shootdown waits OUTSIDE sched_lock — see header). FIRST park on
         * the immortal boot tables: tearing down our own live space and
         * then running schedule/switch code on the gutted mappings is
         * instant death (a stale TLB only hides it briefly — seen live as
         * silent resets right after an exit). Boot maps all of kernel/BSS/
         * PMM, so the rest of EXIT runs fine there until schedule loads
         * the next task's space. */
        extern u64 pml4_boot[];
        cpu_load_cr3((u64)pml4_boot);
        vmm_teardown_space(self->cr3);
        f = SCHED_LOCK();
        self->cr3 = 0;
        self->state = T_FREE;
        SCHED_UNLOCK(f);
    }
    /* Caller (syscall dispatch) schedules away NOW; zombies never resume. */
}

/* Blocking syscalls rewind RIP by 2 (the `int $0x80` itself) so wakeup
 * re-executes the call: waitpid then finds its zombie, sleep finds its
 * deadline passed. No extra restart machinery needed. */

u64 task64_waitpid(int pid, u64 status_ptr, int wnohang, regs64_t *r) {
    task64_t *self = cur;
    if (!self) { r->rax = (u64)(long)-10; return (u64)r; } /* ECHILD */
    if (status_ptr && !vmm_user_ok(status_ptr, 4)) {
        r->rax = (u64)(long)-14; /* EFAULT */
        return (u64)r;
    }
    u64 f = SCHED_LOCK();
    int have = 0, zid = -1;
    for (int i = 1; i < NTASK; i++) {
        if (tasks[i].parent != self->id || tasks[i].state == T_FREE) continue;
        if (pid != -1 && tasks[i].id != pid) continue;
        have = 1;
        if (tasks[i].state == T_ZOMBIE) { zid = i; break; }
    }
    if (zid >= 0) {
        /* Claim under lock (invisible to others), tear down outside it. */
        int status = tasks[zid].exit_code;
        u64 zcr3 = tasks[zid].cr3;
        tasks[zid].cr3 = 0;
        tasks[zid].state = T_FREE;
        SCHED_UNLOCK(f);
        if (status_ptr) *(volatile u32 *)status_ptr = (u32)status;
        vmm_teardown_space(zcr3);
        r->rax = (u64)zid;
        return (u64)r;
    }
    if (!have) {
        SCHED_UNLOCK(f);
        r->rax = (u64)(long)-10;
        return (u64)r;
    } /* ECHILD */
    if (wnohang) {
        SCHED_UNLOCK(f);
        r->rax = 0;
        return (u64)r;
    }
    /* Save OUR blocking frame (M7.1 fix): without this the task resumes
     * from a stale demotion frame on wakeup (wrong code point, stale
     * regs — and after enough stack reuse, wild RIPs). The rewound RIP
     * below re-executes the syscall on wake. */
    self->rsp = (u64)r;
    self->waiting_for = pid;
    self->wakeup_tick = 0;
    self->state = T_BLOCKED;
    SCHED_UNLOCK(f);
    r->rip -= 2;
    return task64_schedule(r);
}

/* M7.3 brk: Linux-ish (returns the new brk, old brk when clamped).
 * Growth only bumps the pointer — pages materialize on first touch via
 * demand faults below. Shrink unmaps+frees immediately (batched). The heap
 * lives in [heap_base, heap_brk); idle (base 0) has none. */
#define BRK_MAX_SPAN (64ULL * 1024 * 1024)

static void brk_shrink(task64_t *t, u64 nw) {
    u64 ob = t->heap_brk;
    u64 va = (nw + 4095) & ~4095ULL;
    if (va >= ob || !t->heap_base) return;
    /* Self-only (brk is per-task): live tables are ours. Batch unmap,
     * single broadcast, then batch free (same collect/flush/free rule). */
    while (va < ob) {
        u64 pas[64];
        int n = 0;
        /* Live tables (own space), like demand above. */
        u64 f = mem_lock_acquire();
        u64 *saved_root = vmm_get_root();
        vmm_set_root((u64 *)(cpu_read_cr3() & ~0xFFFULL));
        for (; va < ob && n < 64; va += 4096) {
            u64 pa = __vmm_translate(va);
            if (!pa) continue;
            if (__vmm_unmap_page(va)) continue;
            pas[n++] = pa;
        }
        vmm_set_root(saved_root);
        mem_lock_release(f);
        if (!n) continue;
        vmm_publish();
        f = mem_lock_acquire();
        for (int i = 0; i < n; i++) __frame_ref_put(pas[i]);
        mem_lock_release(f);
    }
}

u64 task64_brk(u64 nw, regs64_t *r) {
    task64_t *self = cur;
    if (!self) { r->rax = 0; return (u64)r; }
    u64 ob = self->heap_brk, base = self->heap_base;
    if (!nw || !base || nw < base || nw > base + BRK_MAX_SPAN) {
        r->rax = ob;
        return (u64)r;
    }
    if (nw > ob) {
        self->heap_brk = nw;
    } else if (nw < ob) {
        brk_shrink(self, nw);
        self->heap_brk = nw;
    }
    r->rax = self->heap_brk;
    return (u64)r;
}

/* M7.3 demand paging: map a zero frame on first touch of [base, brk).
 * Returns 0 mapped, 1 already-present (lost race: flush + resume),
 * -1 outside the heap (caller kills/fatals). Runs in fault context. */
int task64_demand(u64 va) {
    task64_t *self = cur;
    if (!self || !self->heap_base) return -1;
    u64 page = va & ~0xFFFULL;
    u64 top = (self->heap_brk + 4095) & ~4095ULL;
    if (page < self->heap_base || page >= top || top <= self->heap_base)
        return -1;
    u64 f = mem_lock_acquire();
    /* Demand maps into the LIVE tables (the faulting task's own space) —
     * vmm_root normally points at boot, which would wire (and leak) the
     * page into the wrong space and fault forever (seen live: brkdemo
     * spinning silently on its first touch). Restored on every path. */
    u64 *saved_root = vmm_get_root();
    vmm_set_root((u64 *)(cpu_read_cr3() & ~0xFFFULL));
    u64 pa = __pmm_alloc();
    if (!pa) {
        vmm_set_root(saved_root);
        mem_lock_release(f);
        return -1;
    }
    u64 fl = VMM_RW | VMM_US | (cpu_nx_enabled() ? VMM_NX : 0);
    int rc = __vmm_map_page(page, pa, fl);
    vmm_set_root(saved_root);
    mem_lock_release(f);
    if (rc) {
        pmm_free(pa); /* lost race: present now; drop ours, resume */
        return 1;
    }
    return 0;
}

/* M7.3 fault kill (NX fetch / RO write): Unix exit 139 (128+SIGSEGV),
 * reusable EXIT machinery (zombie + waiter wake + reap). */
u64 task64_kill_fault(regs64_t *r, const char *kind) {
    task64_t *self = cur;
    s_printf("[K64] %s pid=%u rip=%x\n", kind,
             (u64)(self ? self->id : -1), r->rip);
    task64_exit(139);
    return task64_schedule(r);
}

u64 task64_sleep(u64 delta, regs64_t *r) {
    task64_t *self = cur;
    if (!self || delta == 0) { r->rax = 0; return (u64)r; }
    u64 f = SCHED_LOCK();
    if (self->wakeup_tick == 0) self->wakeup_tick = ticks + delta;
    if ((long)(ticks - self->wakeup_tick) >= 0) {
        self->wakeup_tick = 0;
        SCHED_UNLOCK(f);
        r->rax = 0;
        return (u64)r;
    }
    self->rsp = (u64)r; /* blocking frame (same M7.1 fix as waitpid) */
    self->state = T_BLOCKED;
    SCHED_UNLOCK(f);
    r->rip -= 2;
    return task64_schedule(r);
}

/* Round-robin over READY slots, per CPU. Returns the rsp to resume.
 * Pick (mark RUNNING) happens under sched_lock; the low-level switch
 * (FPU/CR3/RSP0/cur) is lock-free (own TCB + immutable next image... the
 * next task's TCB is stable once RUNNING: only its own CPU touches it). */
u64 task64_schedule(regs64_t *r) {
    int me = smp_cpu_index();
    if (me < 0) me = 0;
    /* STALE-SLOT GUARD (M7.1): cur_cpu[me] is only a hint. After slot reuse
     * + migration it can point at a task running ELSEWHERE (or a recycled
     * slot): saving/demoting it corrupts that live task (double-schedule
     * -> shared stacks -> wild CR3s -> double teardown -> silent resets).
     * Only a RUNNING task whose last_cpu is me is truly ours. */
    task64_t *prev = cur_cpu[me];
    if (!prev || prev->state != T_RUNNING || prev->last_cpu != me) {
        prev = 0;
    } else {
        prev->rsp = (u64)r;
    }
    if (smp_in_shootdown()) {
        /* Shootdown wait continues on this same frame; do NOT demote (the
         * task stays RUNNING and owns this CPU). */
        if (prev) prev->rsp = (u64)r;
        return (u64)r;
    }
    task64_t *next = 0;
    u64 f = SCHED_LOCK();
    /* Re-validate under lock (slot may have changed since entry). */
    task64_t *p = cur_cpu[me];
    if (!p || p->state != T_RUNNING || p->last_cpu != me) p = 0;
    if (p) {
        p->rsp = (u64)r;
        p->state = T_READY;
    }
    prev = p;
    int start = prev ? prev->id : 0;
    next = 0;
    for (int i = 1; i <= NTASK; i++) {
        int j = (start + i) % NTASK;
        if (tasks[j].state == T_READY) { next = &tasks[j]; break; }
    }
    if (!next) {
        /* Nothing else runnable: keep our quantum if we still own it... */
        if (prev && prev->state == T_READY) next = prev;
        /* ...otherwise park on this CPU's idle task (M7.1: never hlt-spin
         * inside an IRQ handler — each nested tick would pile another
         * frame on this stack and overflow it within a second of idling).
         * Idle is always available and never scanned/waited/reaped. */
        else next = &idle_tasks[me];
    }
    next->state = T_RUNNING;
    next->last_cpu = me;
    cur_cpu[me] = next;
    /* Save prev's FPU HERE (under lock): after unlock another CPU could
     * pick prev and run it, and our late fxsave would clobber its live
     * image with this CPU's stale regs (rare FPU corruption). */
    if (prev && (prev->state == T_READY || prev->state == T_BLOCKED))
        __asm__ __volatile__("fxsave %0" : "=m"(prev->fx));
    SCHED_UNLOCK(f);
    if (next != prev) {
        /* Eager FPU: prev was saved under lock above; zombies/never-ran
         * tasks need no save; the next image always loads. TSS/CR3 follow
         * the task across CPUs (migration is safe: state always rests in
         * the TCB). */
        __asm__ __volatile__("fxrstor %0" :: "m"(next->fx));
        tss64_set_rsp0_cpu(me, next->kstack_top);
        if (next->cr3 != (cpu_read_cr3() & ~0xFFFULL)) {
            trace_cr3('S', next->id, next->cr3);
            cpu_load_cr3(next->cr3);
        }
    }
    return (next == prev) ? (u64)r : next->rsp;
}

/* M5 FORK: COW-clone the address space, copy the kernel stack + live FPU
 * state. Child resumes at the same RIP with RAX=0; parent gets the child
 * id. Runs with IF=0 (syscall gate): no preemption mid-clone. */
int task64_fork(regs64_t *r) {
    task64_t *self = cur;
    if (!self) return -1;
    task64_t *t = alloc_slot(self->name);
    if (!t) return -12; /* ENOMEM */
    u64 child_pml4 = vmm_clone_space(self->cr3);
    if (!child_pml4) { task_unreserve(t); return -12; }
    t->cr3 = child_pml4;
    t->parent = self->id;
    t->user_base = self->user_base;
    t->heap_base = self->heap_base;
    t->heap_brk = self->heap_brk;
    /* Kernel stack copy (both sides identity-mapped). The live stack is the
     * 16KB BELOW kstack_top — copying [top, top+16K) instead duplicates the
     * neighbor slot and leaves the child frame zeroed (iretq to CS:RIP=0). */
    u8 *sp = (u8 *)(self->kstack_top - KSTACK_SIZE);
    u8 *dp = (u8 *)(t->kstack_top - KSTACK_SIZE);
    for (int i = 0; i < KSTACK_SIZE; i++) dp[i] = sp[i];
    t->rsp = t->kstack_top - (self->kstack_top - (u64)r);
    *(volatile u64 *)(t->rsp + 14 * 8) = 0; /* child sees fork() == 0 */
    /* Live FPU state (stale TCB image is NOT enough: current may have used
     * FPU since its last switch-in). fxsave keeps CPU state intact. */
    __asm__ __volatile__("fxsave %0" : "=m"(self->fx));
    for (int i = 0; i < 512; i += 8)
        *(volatile u64 *)(t->fx + i) = *(volatile u64 *)(self->fx + i);
    task_publish(t);
    return t->id;
}

/* BSP timer tick: time-slice accounting + sleeper wakeup, then preempt. */
u64 task64_on_tick(regs64_t *r) {
    ticks++;
    if ((ticks % 100) == 0) {
        s_puts("[K64] tick ");
        s_dec64(ticks);
        s_puts("\n");
    }
    u64 f = SCHED_LOCK();
    for (int i = 1; i < NTASK; i++) {
        if (tasks[i].state == T_BLOCKED && tasks[i].wakeup_tick != 0 &&
            (long)(ticks - tasks[i].wakeup_tick) >= 0)
            tasks[i].state = T_READY;
    }
    SCHED_UNLOCK(f);
    return task64_schedule(r);
}

/* M7 AP LAPIC tick: pure reschedule (ticks/deadlines advance on the BSP).
 * NOTE: wakeup_tick is cleared only by the re-executed sleep() itself. */
u64 task64_ap_tick(regs64_t *r) {
    return task64_schedule(r);
}
