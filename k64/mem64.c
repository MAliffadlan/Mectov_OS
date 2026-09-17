/* M3 64-bit physical memory + paging (M5: COW/teardown, M7: SMP-safe).
 *
 * LOCKING (mem_lock guards bitmap, refcounts, ALL table state):
 * - High-level ops (spawn/exec/fork/brk/fault/teardown) hold mem_lock
 *   across table mutation; inner helpers assume it (no relock inside).
 * - pmm_alloc/free/put/inc/raw are LOCKED wrappers for simple callers;
 *   inner code uses the __ variants.
 * - Shootdown waits happen with NO lock held (an AP spinning on mem_lock
 *   with IF=0 could never ack -> deadlock). Pattern: mutate -> release ->
 *   flush -> (re-lock to) free. Frame frees always post-date the flush so
 *   no stale AP TLB can touch a reallocated frame.
 * - New mappings need no flush (never translated); downgrades/removals do.
 * - Boot paths (APs parked, sched_running=0) skip all shootdowns: parked
 *   APs hold no user translations, and boot VAs are first-mapped.
 */
#include "cpu64.h"
#include "spin64.h"

static spin64_t mem_lock = SPIN64_INIT;
static volatile int sched_running = 0;

void sched_set_running(void) {
    sched_running = 1;
}

/* Wiring sequences (spawn/exec/brk) hold this across vmm_root switches +
 * multi-page wiring; inner __ primitives assume it. */
u64 mem_lock_acquire(void) {
    return spin64_lock_irqsave(&mem_lock);
}

void mem_lock_release(u64 f) {
    spin64_unlock_irqrestore(&mem_lock, f);
}

static void tlb_others_single(u64 va) {
    if (!sched_running || smp_cpu_count() < 2) return;
    smp_shootdown(va);
}
static void tlb_others_all(void) {
    if (!sched_running || smp_cpu_count() < 2) return;
    smp_shootdown_all();
}

/* Publish freshly wired user mappings (spawn/exec/brk): VA reuse across
 * task lifetimes means APs may hold stale entries. Lock-free (broadcast). */
void vmm_publish(void) {
    tlb_others_all();
}

/* CR3 forensic ring: lock-free single-writer-per-CPU? No — all CPUs append
 * (one u64 index via xadd-ish builtin; entries may interleave but each
 * entry itself is written in order; BSS always mapped, safe in faults). */
#define NTRACE 32
static struct {
    char ev;
    char cpu;
    short task;
    u32 _pad;
    u64 cr3;
} cr3_ring[NTRACE];
static volatile int trace_idx = 0;

void trace_cr3(char ev, int task, u64 cr3) {
    int i = __sync_fetch_and_add(&trace_idx, 1) % NTRACE;
    if (i < 0) i += NTRACE;
    cr3_ring[i].cr3 = cr3;
    cr3_ring[i].task = (short)task;
    cr3_ring[i].cpu = 0;
    cr3_ring[i].ev = ev;
}

void trace_cr3_dump(void) {
    int n = trace_idx;
    if (n > NTRACE) n = NTRACE;
    int start = trace_idx - n;
    for (int k = 0; k < n; k++) {
        int i = (start + k) % NTRACE;
        if (i < 0) i += NTRACE;
        s_rawc('[');
        s_rawc(cr3_ring[i].ev ? cr3_ring[i].ev : '.');
        s_rawc(' ');
        s_rawu((u64)(u32)cr3_ring[i].task);
        s_rawc(' ');
        s_rawx(cr3_ring[i].cr3);
        s_raws("]\n");
    }
}
/* M3 design notes (still current):
 *  - PMM: 4KB frames, static 128KB bitmap (1M frames = 4GB), first-free
 *    scan. All frames start reserved; usable Multiboot2 ranges free them.
 *    [0,2MB) stays reserved wholesale (BIOS/GRUB/kernel/tables/stacks/MB
 *    info all live there — M4/M7 can tighten this to _kernel_end).
 *  - Identity: low RAM mapped 1:1 with 2MB large pages (extends the boot
 *    tables in place — no CR3 switch while running, then one CR3 reload to
 *    flush the TLB). The framebuffer gets PCD|PWT|NX 2MB pages; a 2MB page
 *    intersecting the FB (low-FB hardware) is skipped by identity and left
 *    to the FB mapper.
 *  - Walker: 4KB-only vmm_map/unmap/translate with canonical checks. It
 *    refuses to split 2MB pages (identity/FB ranges) — user mappings live
 *    outside those ranges (M4).
 *
 * Ordering caveat: during mem64_init only the boot map (0-8MB) exists, so
 * every frame the init path touches must be <8MB. All table frames are
 * therefore pre-allocated FIRST (first-free scan hands out the lowest
 * frames), and only then do the fill loops run.
 */

/* PTE bits (4-level paging). */
#define PTE_P    1ULL
#define PTE_RW   2ULL
#define PTE_US   4ULL
#define PTE_PWT  8ULL
#define PTE_PCD  16ULL
#define PTE_PS   0x80ULL
#define PTE_NX   (1ULL << 63)
#define PTE_ADDR 0x000FFFFFFFFFF000ULL
#define PTE_COW  0x200ULL /* M5: copy-on-write user page (avail bit 9) */

#define PAGE4K 4096ULL
#define PAGE2M (512ULL * PAGE4K)
#define RESERVE_TOP (2ULL * 1024 * 1024) /* [0,2MB) never handed out */
#define PMM_MAX_FRAMES (1024ULL * 1024)  /* 4GB worth of 4KB frames */

/* Boot tables (boot64.asm .bss, identity-mapped). */
extern u64 pml4_boot[];
extern u64 pdpt_low[];
extern u64 pd_low[];

static u64 pmm_bits[PMM_MAX_FRAMES / 64];
static u16 frame_ref[PMM_MAX_FRAMES]; /* M5: mapping refcount per frame */
static u64 pmm_total = 0;
static u64 pmm_free_n = 0;
static u64 mem_top = 0;
static int nx_on = 0;
static u64 g_fb = 0, g_fbsize = 0;
/* Mapping root for vmm_map/unmap (default: boot PML4; spawn points it at a
 * child space while wiring it — single core + IF=0 at all such sites).
 * translate/user_ok instead walk the LIVE CR3 (== caller's space). */
static u64 *vmm_root;

static struct { u64 base, len; } ranges[32];
static int nranges = 0;

static inline void invlpg(u64 va) {
    __asm__ __volatile__("invlpg (%0)" :: "r"(va) : "memory");
}
static inline void cr3_reload(void) {
    __asm__ __volatile__("mov %%cr3, %%rax\n\tmov %%rax, %%cr3"
                         ::: "rax", "memory");
}

/* ---- PMM ---- */

static void pmm_mark(u64 frame, int used) {
    if (frame >= PMM_MAX_FRAMES) return;
    u64 i = frame / 64, b = (u64)1 << (frame % 64);
    int was = (pmm_bits[i] & b) != 0;
    if (used && !was) { pmm_bits[i] |= b; pmm_free_n--; }
    if (!used && was) { pmm_bits[i] &= ~b; pmm_free_n++; pmm_total++; }
}

u64 __pmm_alloc(void) {
    for (u64 i = 0; i < PMM_MAX_FRAMES / 64; i++) {
        u64 inv = ~pmm_bits[i];
        if (!inv) continue;
        unsigned b = (unsigned)__builtin_ctzll(inv);
        u64 f = i * 64 + b;
        pmm_bits[i] |= (u64)1 << b;
        pmm_free_n--;
        frame_ref[f] = 1;
        /* Zero via the identity map (safe: every PMM frame is identity
         * mapped once mem64_init completes; init-time allocs are <8MB). */
        volatile u64 *p = (volatile u64 *)(f * PAGE4K);
        for (int k = 0; k < 512; k++) p[k] = 0;
        return f * PAGE4K;
    }
    return 0;
}

u64 pmm_alloc(void) {
    u64 f = spin64_lock_irqsave(&mem_lock);
    u64 r = __pmm_alloc();
    spin64_unlock_irqrestore(&mem_lock, f);
    return r;
}

static void __bitmap_free_frame(u64 fr) {
    u64 i = fr / 64;
    u64 b = (u64)1 << (fr % 64);
    if (pmm_bits[i] & b) { pmm_bits[i] &= ~b; pmm_free_n++; }
}

/* Refcounted release (data pages): frees the frame at the last mapping. */
int __frame_ref_put(u64 pa) {
    u64 f = pa >> 12;
    if (f >= PMM_MAX_FRAMES) return 0;
    if (frame_ref[f] > 0) frame_ref[f]--;
    if (frame_ref[f] == 0) __bitmap_free_frame(f);
    return frame_ref[f];
}

int frame_ref_put(u64 pa) {
    u64 f = spin64_lock_irqsave(&mem_lock);
    int r = __frame_ref_put(pa);
    spin64_unlock_irqrestore(&mem_lock, f);
    return r;
}

static void __frame_ref_inc(u64 pa) {
    u64 f = pa >> 12;
    if (f < PMM_MAX_FRAMES && frame_ref[f] < 0xFFFF) frame_ref[f]++;
}

void frame_ref_inc(u64 pa) {
    u64 f = spin64_lock_irqsave(&mem_lock);
    __frame_ref_inc(pa);
    spin64_unlock_irqrestore(&mem_lock, f);
}

void pmm_free(u64 pa) {
    if (pa & 0xFFF) return;
    u64 f = spin64_lock_irqsave(&mem_lock);
    __frame_ref_put(pa);
    spin64_unlock_irqrestore(&mem_lock, f);
}

/* Raw release (page-table frames, never COW-shared): bitmap only. */
static void __pmm_free_raw(u64 pa) {
    u64 f = pa >> 12;
    if (f >= PMM_MAX_FRAMES) return;
    __bitmap_free_frame(f);
}

void pmm_free_raw(u64 pa) {
    if (pa & 0xFFF) return;
    u64 f = spin64_lock_irqsave(&mem_lock);
    __pmm_free_raw(pa);
    spin64_unlock_irqrestore(&mem_lock, f);
}

u64 pmm_free_frames(void) { return pmm_free_n; }
u64 pmm_total_frames(void) { return pmm_total; }
int cpu_nx_enabled(void) { return nx_on; }

/* M7: per-CPU EFER.NXE (MSRs are per-core; the BSP got its via boot64.asm
 * with a CPUID gate). Every AP must enable it before running user code:
 * NX-marked pages (demand heap, W^X data) raise reserved-bit #PF on cores
 * without NXE (seen live as fault err=P|RSVD on an AP touching heap). */
void paging_enable_nxe_ap(void) {
    u32 a, b, c, d;
    __asm__ __volatile__("cpuid"
                         : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(0x80000001));
    if (!(d & (1u << 20))) return; /* no NX feature */
    u64 e = cpu_read_efer() | (1ULL << 11);
    u32 lo = (u32)e, hi = (u32)(e >> 32);
    __asm__ __volatile__("mov $0xC0000080, %%ecx\n\twrmsr" ::"a"(lo),
                         "d"(hi)
                         : "ecx");
}

int vmm_is_canonical(u64 va) {
    u64 hi = va >> 48;
    if ((va >> 47) & 1) return hi == 0xFFFF;
    return hi == 0;
}

/* Next-level table under vmm_root, allocating with `inter` flags (P|RW
 * [+US]). Refuses large pages (returns 0) — M3 never splits identity/FB
 * 2MB mappings. A VMM_US walk also widens pre-existing static entries
 * (PML4[0] and friends were born supervisor-only at boot). */
static u64 *next_tab(u64 *tab, u64 idx, u64 inter) {
    u64 e = tab[idx];
    if (e & PTE_P) {
        if (e & PTE_PS) return 0;
        if ((inter & PTE_US) && !(e & PTE_US))
            tab[idx] = e | PTE_US;
        return (u64 *)(tab[idx] & PTE_ADDR);
    }
    u64 nf = __pmm_alloc();
    if (!nf) return 0;
    tab[idx] = nf | inter;
    return (u64 *)nf;
}

/* Inner wiring (caller holds mem_lock, or boot single-threaded init). */
int __vmm_map_page(u64 va, u64 pa, u64 flags) {
    if (!vmm_is_canonical(va) || (va & 0xFFF) || (pa & 0xFFF)) return -1;
    if ((flags & VMM_NX) && !nx_on) return -1;
    u64 inter = PTE_P | PTE_RW | ((flags & VMM_US) ? PTE_US : 0);
    u64 *pdpt = next_tab(vmm_root, (va >> 39) & 511, inter);
    if (!pdpt) return -1;
    u64 *pd = next_tab(pdpt, (va >> 30) & 511, inter);
    if (!pd) return -1;
    u64 *pt = next_tab(pd, (va >> 21) & 511, inter);
    if (!pt) return -1;
    u64 *slot = &pt[(va >> 12) & 511];
    if (*slot & PTE_P) return -1;
    u64 e = (pa & PTE_ADDR) | PTE_P;
    if (flags & VMM_RW) e |= PTE_RW;
    if (flags & VMM_US) e |= PTE_US;
    if (flags & VMM_UC) e |= PTE_PWT | PTE_PCD;
    if (flags & VMM_NX) e |= PTE_NX;
    *slot = e;
    invlpg(va);
    return 0;
}

int vmm_map_page(u64 va, u64 pa, u64 flags) {
    u64 f = spin64_lock_irqsave(&mem_lock);
    int r = __vmm_map_page(va, pa, flags);
    spin64_unlock_irqrestore(&mem_lock, f);
    return r;
}

/* M10: map in an EXPLICIT root. The kernel heap must place its pages in
 * boot's tables whatever root the caller happens to run on (kmalloc is
 * reachable from a task context), and it must not retarget the global
 * vmm_root to do it: that pointer is shared by every CPU, so swapping it
 * outside the lock would let a concurrent mapper wire a PTE into the wrong
 * address space. Same reservation rule as vmm_set_root, but atomic. */
int vmm_map_page_in(u64 *root, u64 va, u64 pa, u64 flags) {
    u64 f = spin64_lock_irqsave(&mem_lock);
    u64 *saved = vmm_root;
    vmm_root = root;
    int r = __vmm_map_page(va, pa, flags);
    vmm_root = saved;
    spin64_unlock_irqrestore(&mem_lock, f);
    return r;
}

int __vmm_unmap_page(u64 va) {
    if (!vmm_is_canonical(va) || (va & 0xFFF)) return -1;
    u64 e = vmm_root[(va >> 39) & 511];
    if (!(e & PTE_P) || (e & PTE_PS)) return -1;
    u64 *pdpt = (u64 *)(e & PTE_ADDR);
    e = pdpt[(va >> 30) & 511];
    if (!(e & PTE_P) || (e & PTE_PS)) return -1;
    u64 *pd = (u64 *)(e & PTE_ADDR);
    e = pd[(va >> 21) & 511];
    if (!(e & PTE_P) || (e & PTE_PS)) return -1;
    u64 *pt = (u64 *)(e & PTE_ADDR);
    u64 *slot = &pt[(va >> 12) & 511];
    if (!(*slot & PTE_P)) return -1;
    *slot = 0;
    invlpg(va);
    return 0;
}

int vmm_unmap_page(u64 va) {
    u64 f = spin64_lock_irqsave(&mem_lock);
    int r = __vmm_unmap_page(va);
    spin64_unlock_irqrestore(&mem_lock, f);
    return r;
}

/* Inner translate (caller holds mem_lock, or boot single-threaded init):
 * walks the LIVE CR3 (== caller's space in all uses). */
u64 __vmm_translate(u64 va) {
    if (!vmm_is_canonical(va)) return 0;
    u64 live = cpu_read_cr3() & PTE_ADDR;
    u64 *root = (u64 *)live;
    u64 e = root[(va >> 39) & 511];
    if (!(e & PTE_P)) return 0;
    if (e & PTE_PS) return (e & PTE_ADDR) + (va & 0x7FFFFFFFFFULL);
    u64 *pdpt = (u64 *)(e & PTE_ADDR);
    e = pdpt[(va >> 30) & 511];
    if (!(e & PTE_P)) return 0;
    if (e & PTE_PS) return (e & PTE_ADDR) + (va & 0x3FFFFFFFULL);
    u64 *pd = (u64 *)(e & PTE_ADDR);
    e = pd[(va >> 21) & 511];
    if (!(e & PTE_P)) return 0;
    if (e & PTE_PS) return (e & PTE_ADDR) + (va & 0x1FFFFFULL);
    u64 *pt = (u64 *)(e & PTE_ADDR);
    e = pt[(va >> 12) & 511];
    if (!(e & PTE_P)) return 0;
    return (e & PTE_ADDR) + (va & 0xFFF);
}

u64 vmm_translate(u64 va) {
    u64 f = spin64_lock_irqsave(&mem_lock);
    u64 r = __vmm_translate(va);
    spin64_unlock_irqrestore(&mem_lock, f);
    return r;
}

/* Leaf probe for fault classification (NX fetch / RO write kills).
 * Returns 0 with *flags = P|RW|US|(NX>>60) (bit3 = NX), or -1 unmapped. */
int vmm_probe(u64 va, u64 *flags) {
    if (!vmm_is_canonical(va) || !flags) return -1;
    u64 f = spin64_lock_irqsave(&mem_lock);
    u64 *root = (u64 *)(cpu_read_cr3() & PTE_ADDR);
    u64 e = root[(va >> 39) & 511];
    int r = -1;
    if (!(e & PTE_P)) goto out;
    if (e & PTE_PS) goto leaf;
    e = ((u64 *)(e & PTE_ADDR))[(va >> 30) & 511];
    if (!(e & PTE_P)) goto out;
    if (e & PTE_PS) goto leaf;
    e = ((u64 *)(e & PTE_ADDR))[(va >> 21) & 511];
    if (!(e & PTE_P)) goto out;
    if (e & PTE_PS) goto leaf;
    e = ((u64 *)(e & PTE_ADDR))[(va >> 12) & 511];
    if (!(e & PTE_P)) goto out;
leaf:
    *flags = (e & 7) | ((e >> 63) ? 8 : 0);
    r = 0;
out:
    spin64_unlock_irqrestore(&mem_lock, f);
    return r;
}

/* True when [va, va+len) is fully mapped PRESENT|USER (large pages
 * accepted at whichever level maps them). Used to vet Ring-3 pointers. */
int vmm_user_ok(u64 va, u64 len) {
    if (!len || len > (1ULL << 20)) return 0;
    if (!vmm_is_canonical(va)) return 0;
    u64 start = va & ~0xFFFULL;
    u64 end = (va + len + 0xFFF) & ~0xFFFULL;
    if (end < start || !vmm_is_canonical(end - 1)) return 0;
    u64 *root = (u64 *)(cpu_read_cr3() & PTE_ADDR); /* caller's space */
    u64 fl = spin64_lock_irqsave(&mem_lock);
    int ok = 1;
    for (u64 p = start; p < end; p += PAGE4K) {
        u64 e = root[(p >> 39) & 511];
        if (!(e & PTE_P)) { ok = 0; break; }
        const u64 want = PTE_P | PTE_US;
        if (e & PTE_PS) {
            if ((e & want) != want) { ok = 0; break; }
            continue;
        }
        u64 *pdpt = (u64 *)(e & PTE_ADDR);
        e = pdpt[(p >> 30) & 511];
        if (!(e & PTE_P)) { ok = 0; break; }
        if (e & PTE_PS) {
            if ((e & want) != want) { ok = 0; break; }
            continue;
        }
        u64 *pd = (u64 *)(e & PTE_ADDR);
        e = pd[(p >> 21) & 511];
        if (!(e & PTE_P)) { ok = 0; break; }
        if (e & PTE_PS) {
            if ((e & want) != want) { ok = 0; break; }
            continue;
        }
        u64 *pt = (u64 *)(e & PTE_ADDR);
        e = pt[(p >> 12) & 511];
        if ((e & want) != want) { ok = 0; break; }
        if (p + PAGE4K < p) break; /* overflow guard (end is bounded) */
    }
    spin64_unlock_irqrestore(&mem_lock, fl);
    return ok;
}

/* ---- M5: address-space clone (COW) + teardown ---- */

void vmm_set_root(u64 *root) {
    vmm_root = root;
}

u64 *vmm_get_root(void) {
    return vmm_root;
}

/* Count tables a clone must allocate (reservation pass so the copy pass
 * cannot OOM midway and leak a half-built space). */
static void count_clone(u64 *tab, int level, u64 *n) {
    for (int i = 0; i < 512; i++) {
        u64 e = tab[i];
        if (!(e & PTE_P) || (e & PTE_PS)) continue;
        if (!(e & PTE_US)) continue; /* kernel-only subtree: shared */
        if (level == 0) continue;    /* PT's US children are pages */
        (*n)++;
        count_clone((u64 *)(e & PTE_ADDR), level - 1, n);
    }
}

/* Deep-copy pass. Rules:
 * - PML4 (level 3): PDPTs are ALWAYS fresh-copied, even US-free ones.
 *   (A shared PDPT would receive one task's future user mappings into
 *   every sharer's space. PDs and below share freely while US-free.)
 * - US subtrees: copied, 4KB US pages COW-marked on BOTH sides + refcount.
 * - Supervisor pages/tables: shared by pointer (immutable, never freed).
 * - Large US pages: impossible in M5 (all user maps are 4KB) -> fail shut.
 * Tables are identity-accessible from any CR3 (all < mem_top). */
static u64 clone_level(u64 *src, int level, int cow) {
    u64 nf = __pmm_alloc();
    if (!nf) return 0;
    u64 *dst = (u64 *)nf;
    for (int i = 0; i < 512; i++) {
        u64 e = src[i];
        if (!(e & PTE_P)) continue;
        if (level > 0 && (e & PTE_PS)) {
            if (e & PTE_US) { __pmm_free_raw(nf); return 0; }
            dst[i] = e;
            continue;
        }
        if (level == 3 || (e & PTE_US)) {
            if (level == 0) { /* PT leaf: share or COW the user page.
                               * M7.3: only EVER-writable pages go COW —
                               * RX pages stay shared-RO (refcounted): a
                               * write there must RO-kill, not privatize.
                               * Threads (cow==0) keep RW-shared mappings. */
                u64 pa = e & PTE_ADDR;
                if (cow && (e & PTE_US) && (e & PTE_RW)) {
                    u64 c = (pa | PTE_P | PTE_US | PTE_COW) & ~PTE_RW;
                    if (e & PTE_NX) c |= PTE_NX;
                    src[i] = c;
                    dst[i] = c;
                    __frame_ref_inc(pa);
                } else {
                    dst[i] = e;
                    if (e & PTE_US) __frame_ref_inc(pa);
                }
                continue;
            }
            u64 c = clone_level((u64 *)(e & PTE_ADDR), level - 1, cow);
            if (!c) { __pmm_free_raw(nf); return 0; } /* unreachable */
            dst[i] = c | PTE_P | PTE_RW | PTE_US;
            continue;
        }
        dst[i] = e; /* kernel-only table: share pointer */
    }
    return nf;
}

static u64 clone_space_reserved(u64 src_pml4, int cow) {
    u64 *src = (u64 *)src_pml4;
    /* Reservation: 1 PML4 + fresh PDPT per present entry + US subtrees. */
    u64 need = 1;
    for (int i = 0; i < 512; i++)
        if (src[i] & PTE_P) need++;
    count_clone(src, 3, &need);
    if (pmm_free_frames() < need) return 0;
    return clone_level(src, 3, cow);
}

u64 vmm_clone_space(u64 src_pml4) {
    u64 f = spin64_lock_irqsave(&mem_lock);
    u64 r = clone_space_reserved(src_pml4, 1);
    spin64_unlock_irqrestore(&mem_lock, f);
    if (r) {
        /* Downgrades (RW->COW) need flushing HERE (this CPU: it keeps
         * running on these tables) and on APs (M5's lesson, now xCPU).
         * New mappings need none (never translated). */
        cr3_reload();
        tlb_others_all();
    }
    return r;
}

/* M4 CLONE under one address space per task: duplicate the tables but keep
 * every user page truly shared (RW preserved, refcounted for teardown). */
u64 vmm_share_space(u64 src_pml4) {
    u64 f = spin64_lock_irqsave(&mem_lock);
    u64 r = clone_space_reserved(src_pml4, 0);
    spin64_unlock_irqrestore(&mem_lock, f);
    if (r) {
        cr3_reload(); /* US-widening on shared paths, same reason */
        tlb_others_all();
    }
    return r;
}

/* Free a task's private space. Two phases (M7 locking rule): collect under
 * the lock (entries cleared, frames recorded), then flush APs, then free
 * the frames — no stale AP TLB can ever touch a reallocated frame, and no
 * lock is held across the shootdown wait. Boot's own space is never torn
 * down (only user tasks exit). */
#define TEAR_MAX_DATA 256
#define TEAR_MAX_TAB 64
typedef struct {
    u64 data[TEAR_MAX_DATA];
    int ndata;
    u64 tabs[TEAR_MAX_TAB];
    int ntabs;
} tear_list_t;

static void tear_flush_free(tear_list_t *L) {
    if (!L->ndata && !L->ntabs) return;
    tlb_others_all();
    u64 f = spin64_lock_irqsave(&mem_lock);
    for (int i = 0; i < L->ndata; i++) __frame_ref_put(L->data[i]);
    for (int i = 0; i < L->ntabs; i++) __pmm_free_raw(L->tabs[i]);
    spin64_unlock_irqrestore(&mem_lock, f);
    L->ndata = 0;
    L->ntabs = 0;
}

static void tear_record(tear_list_t *L, u64 pa, int is_tab) {
    if (is_tab) {
        if (L->ntabs >= TEAR_MAX_TAB) tear_flush_free(L);
        L->tabs[L->ntabs++] = pa;
    } else {
        if (L->ndata >= TEAR_MAX_DATA) tear_flush_free(L);
        L->data[L->ndata++] = pa;
    }
}

static void tear_collect(u64 *tab, u64 *btab, int level, tear_list_t *L) {
    if (tab == btab) return;
    for (int i = 0; i < 512; i++) {
        u64 e = tab[i];
        if (!(e & PTE_P)) continue;
        /* Shared-with-boot test MUST compare frame address (+size class)
         * only — never full-entry equality: the CPU sets Accessed/Dirty
         * status bits asymmetrically on each copy, so two mappings of the
         * same shared frame legitimately differ (seen live: teardown freed
         * a shared kernel table after a flag-only divergence -> kernel text
         * vanished mid-teardown -> triple-fault reset). Same frame = shared
         * (frames are never split); worst case of over-matching is a leak,
         * never corruption. */
        if (btab) {
            u64 b = btab[i];
            if ((b & PTE_P) && !!(b & PTE_PS) == !!(e & PTE_PS) &&
                (b & PTE_ADDR) == (e & PTE_ADDR))
                continue; /* shared with boot: hands off */
        }
        if (level == 0) {
            if (e & PTE_US) tear_record(L, e & PTE_ADDR, 0);
            tab[i] = 0;
            continue;
        }
        if (e & PTE_PS) { tab[i] = 0; continue; } /* shared large page */
        u64 *child = (u64 *)(e & PTE_ADDR);
        u64 be = btab ? btab[i] : 0;
        u64 *bchild =
            ((be & PTE_P) && !(be & PTE_PS)) ? (u64 *)(be & PTE_ADDR) : 0;
        tear_collect(child, bchild, level - 1, L);
        tear_record(L, (u64)child, 1);
        tab[i] = 0;
    }
}

void vmm_teardown_space(u64 pml4_pa) {
    if (!pml4_pa) return;
    trace_cr3('T', -9, pml4_pa);
    tear_list_t L;
    L.ndata = 0;
    L.ntabs = 0;
    u64 f = spin64_lock_irqsave(&mem_lock);
    tear_collect((u64 *)pml4_pa, pml4_boot, 3, &L);
    tear_record(&L, pml4_pa, 1);
    spin64_unlock_irqrestore(&mem_lock, f);
    tear_flush_free(&L);
}

/* Resolve a COW write fault in the CURRENT space (fault context always
 * runs on the faulting task's CR3).
 *   0 = resolved (resume), 1 = already RW / not COW (spurious: invlpg and
 *   resume — a lost race with a concurrent resolver), -1 = not ours.
 * The old frame is released only after APs flushed: same rule as teardown.
 */
int vmm_cow_resolve(u64 va) {
    u64 f = spin64_lock_irqsave(&mem_lock);
    u64 *root = (u64 *)(cpu_read_cr3() & PTE_ADDR);
    u64 e = root[(va >> 39) & 511];
    if (!(e & PTE_P) || (e & PTE_PS)) goto out_no;
    u64 *pdpt = (u64 *)(e & PTE_ADDR);
    e = pdpt[(va >> 30) & 511];
    if (!(e & PTE_P) || (e & PTE_PS)) goto out_no;
    u64 *pd = (u64 *)(e & PTE_ADDR);
    e = pd[(va >> 21) & 511];
    if (!(e & PTE_P) || (e & PTE_PS)) goto out_no;
    u64 *pt = (u64 *)(e & PTE_ADDR);
    u64 *slot = &pt[(va >> 12) & 511];
    e = *slot;
    if (!(e & PTE_P) || !(e & PTE_US)) goto out_no;
    if (!(e & PTE_COW)) {
        /* Not COW: only an already-RW page (lost race with a concurrent
         * resolver) resumes here. Anything else (RX text, NX data, guard
         * holes) must fall through to the kill/FATAL paths — resuming a
         * read-only fault loops forever with no kill and no progress
         * (seen live: nxtest's RO child spinning silently). */
        if (!(e & PTE_RW)) goto out_no;
        invlpg(va);
        spin64_unlock_irqrestore(&mem_lock, f);
        return 1;
    }
    u64 pa = e & PTE_ADDR;
    u64 fr = pa >> 12;
    if (fr < PMM_MAX_FRAMES && frame_ref[fr] == 1) {
        *slot = (e | PTE_RW) & ~PTE_COW; /* sole owner: upgrade in place */
        invlpg(va);
        spin64_unlock_irqrestore(&mem_lock, f);
        return 0;
    }
    u64 nf = __pmm_alloc();
    if (!nf) {
        spin64_unlock_irqrestore(&mem_lock, f);
        return -1;
    }
    u64 *s = (u64 *)pa;   /* identity-mapped: readable from any CR3 */
    u64 *d = (u64 *)nf;
    for (int i = 0; i < 512; i++) d[i] = s[i];
    *slot = ((nf | PTE_P | PTE_RW | PTE_US) & ~PTE_COW) |
            (e & PTE_NX);
    invlpg(va);
    spin64_unlock_irqrestore(&mem_lock, f);
    tlb_others_single(va);
    f = spin64_lock_irqsave(&mem_lock);
    __frame_ref_put(pa);
    spin64_unlock_irqrestore(&mem_lock, f);
    return 0;
out_no:
    spin64_unlock_irqrestore(&mem_lock, f);
    return -1;
}

/* ---- init ---- */

static int range_overlaps_fb(u64 base, u64 len) {
    if (!g_fbsize) return 0;
    return base < g_fb + g_fbsize && g_fb < base + len;
}

void mem64_init(u64 mb_info) {
    u64 total = *(volatile u32 *)mb_info;
    u64 end = mb_info + total;
    vmm_root = pml4_boot;

    /* Pass 1: collect usable ranges + FB info from Multiboot2 tags. */
    u64 p = mb_info + 8;
    while (p + 8 <= end) {
        u32 type = *(volatile u32 *)p;
        u32 size = *(volatile u32 *)(p + 4);
        if (size < 8 || p + size > end) break;
        if (type == 0) break;
        if (type == 6 && nranges < 32) { /* memmap */
            u32 es = *(volatile u32 *)(p + 8);
            for (u64 e = p + 16; e + es <= p + size; e += es) {
                u64 base = *(volatile u64 *)e;
                u64 len = *(volatile u64 *)(e + 8);
                u32 et = *(volatile u32 *)(e + 16);
                if (et != 1 || !len) continue;
                if (base + len > mem_top) mem_top = base + len;
                /* Clip to the 4GB PMM window. */
                if (base >= 4ULL * 1024 * 1024 * 1024) continue;
                if (base + len > 4ULL * 1024 * 1024 * 1024 || base + len < base)
                    len = 4ULL * 1024 * 1024 * 1024 - base;
                if (nranges < 32) {
                    ranges[nranges].base = base;
                    ranges[nranges].len = len;
                    nranges++;
                }
            }
        } else if (type == 8) { /* framebuffer */
            g_fb = *(volatile u64 *)(p + 8);
            u32 pitch = *(volatile u32 *)(p + 16);
            u32 h = *(volatile u32 *)(p + 24);
            g_fbsize = (u64)pitch * h;
        }
        p += (size + 7) & ~7ULL;
    }

    /* Pass 2: bitmap starts all-reserved; free usable frames. */
    for (u64 i = 0; i < PMM_MAX_FRAMES / 64; i++) pmm_bits[i] = ~0ULL;
    pmm_total = 0;
    pmm_free_n = 0;
    for (int r = 0; r < nranges; r++) {
        u64 base = ranges[r].base, len = ranges[r].len;
        u64 first = (base + PAGE4K - 1) / PAGE4K;
        u64 last = (base + len) / PAGE4K; /* exclusive */
        if (first * PAGE4K < RESERVE_TOP)
            first = RESERVE_TOP / PAGE4K;
        for (u64 f = first; f < last; f++) pmm_mark(f, 0);
    }
    /* Re-reserve a low framebuffer sitting inside RAM (mirrors the 32-bit
     * phys_reserve_region; on QEMU the FB is at 0xFD000000, above RAM). */
    if (g_fbsize && g_fb < mem_top) {
        u64 first = g_fb / PAGE4K, last = (g_fb + g_fbsize + PAGE4K - 1) / PAGE4K;
        for (u64 f = first; f < last; f++) pmm_mark(f, 1);
    }

    nx_on = (cpu_read_efer() >> 11) & 1;

    if (!mem_top || !nranges) {
        s_puts("[K64] FAIL: no usable RAM in multiboot2 memmap\n");
        for (;;) __asm__ __volatile__("cli; hlt");
    }

    /* Pass 3: pre-allocate every extra PD table while only the 0-8MB boot
     * map exists (first-free scan = lowest frames, all <8MB, mapped). */
    u64 top2m = (mem_top + PAGE2M - 1) & ~(PAGE2M - 1);
    u64 need_idx[8];
    int nneed = 0;
    for (u64 idx = 1; idx <= ((top2m - 1) >> 30) && nneed < 8; idx++)
        need_idx[nneed++] = idx;
    u64 fb_pdpt = (g_fb >> 30) & 511;
    if (g_fbsize && fb_pdpt != 0 && ((top2m - 1) >> 30) < fb_pdpt && nneed < 8)
        need_idx[nneed++] = fb_pdpt;
    u64 pre_pd[8];
    for (int i = 0; i < nneed; i++) {
        pre_pd[i] = pmm_alloc();
        if (!pre_pd[i]) {
            s_puts("[K64] FAIL: out of low frames for page tables\n");
            for (;;) __asm__ __volatile__("cli; hlt");
        }
        pdpt_low[need_idx[i]] = pre_pd[i] | PTE_P | PTE_RW;
    }

    /* Pass 4: identity-fill RAM with 2MB pages (skip FB-overlapping ones). */
    for (u64 pa = 0; pa < top2m; pa += PAGE2M) {
        if (range_overlaps_fb(pa, PAGE2M)) continue;
        u64 pdpt_idx = (pa >> 30) & 511;
        u64 pd_idx = (pa >> 21) & 511;
        u64 *pd = (pdpt_idx == 0) ? pd_low
                                  : (u64 *)(pdpt_low[pdpt_idx] & PTE_ADDR);
        pd[pd_idx] = (pa & ~(PAGE2M - 1)) | PTE_P | PTE_RW | PTE_PS;
    }

    /* Pass 5: framebuffer as UC|NX 2MB pages (never executable data). */
    if (g_fbsize) {
        u64 e = pdpt_low[fb_pdpt];
        u64 *pd;
        if (fb_pdpt == 0) {
            pd = pd_low;
        } else {
            if (!(e & PTE_P)) { /* FB below 1GB but PD missing (tiny RAM) */
                u64 nf = pmm_alloc();
                pdpt_low[fb_pdpt] = nf | PTE_P | PTE_RW;
                pd = (u64 *)nf;
            } else {
                pd = (u64 *)(e & PTE_ADDR);
            }
        }
        u64 npages = (g_fbsize + PAGE2M - 1) / PAGE2M;
        for (u64 i = 0; i < npages; i++) {
            u64 pa = (g_fb & ~(PAGE2M - 1)) + i * PAGE2M;
            u64 pd_idx = (pa >> 21) & 511;
            /* FB crossing a 1GB boundary would need the next PD; QEMU's
             * 3MB FB never does — refuse rather than corrupt (M7). */
            if (pd_idx < ((g_fb >> 21) & 511)) break;
            u64 ent = pa | PTE_P | PTE_RW | PTE_PS | PTE_PWT | PTE_PCD;
            if (nx_on) ent |= PTE_NX;
            pd[pd_idx] = ent;
        }
    }

    /* Pass 6 (M6): PCI MMIO window 0xFE000000-0xFFFFFFFF (32MB, 16x2MB) as
     * UC supervisor pages in PDPT[3]'s PD (shared with the framebuffer PD —
     * same flags, kernel-only, never COW'd). Covers LAPIC 0xFEE00000 and
     * IOAPIC 0xFEC00000 for SMP bring-up; the frames are MMIO, never RAM,
     * so the PMM (sized from usable-RAM ranges) can never hand them out. */
    {
        u64 e3 = pdpt_low[3];
        u64 *mpd;
        if (e3 & PTE_P) {
            mpd = (u64 *)(e3 & PTE_ADDR);
        } else {
            u64 nf = pmm_alloc();
            if (!nf) {
                s_puts("[K64] FAIL: no frame for MMIO PD\n");
                for (;;) __asm__ __volatile__("cli; hlt");
            }
            pdpt_low[3] = nf | PTE_P | PTE_RW;
            mpd = (u64 *)nf;
        }
        for (int i = 0; i < 16; i++) {
            u64 pa = 0xFE000000ULL + (u64)i * PAGE2M;
            mpd[((pa >> 21) & 511)] =
                pa | PTE_P | PTE_RW | PTE_PS | PTE_PWT | PTE_PCD;
        }
    }

    cr3_reload(); /* flush TLB: identity/FB/MMIO entries are brand new */

    s_puts("[K64] M3 PMM top=");
    s_hex64(mem_top);
    s_puts(" total=");
    s_dec64(pmm_total_frames());
    s_puts(" free=");
    s_dec64(pmm_free_frames());
    s_puts(" NX=");
    s_putc(nx_on ? '1' : '0');
    s_puts(" FB=");
    s_hex64(g_fb);
    s_puts("\n");
}

/* ---- selftest (runs pre-STI: any bug faults into the M2 #PF dumper) ---- */

void mem64_selftest(void) {
    u64 f0 = pmm_alloc(), f1 = pmm_alloc(), f2 = pmm_alloc();
    if (!f0 || !f1 || !f2 || f0 == f1 || f0 == f2 || f1 == f2 ||
        f0 < RESERVE_TOP || f1 < RESERVE_TOP || f2 < RESERVE_TOP) {
        s_puts("[K64] FAIL: PMM alloc broke\n");
        for (;;) __asm__ __volatile__("cli; hlt");
    }

    /* Non-canonical VA must be refused (canonical-hole guard). */
    if (vmm_map_page(0x0000800000000000ULL, f0, VMM_RW) == 0) {
        s_puts("[K64] FAIL: non-canonical map accepted\n");
        for (;;) __asm__ __volatile__("cli; hlt");
    }

    /* 2GB scratch (PDPT[2]): outside identity (<1GB), clear of the M4 demo
     * images (PDPT[1]) and the framebuffer PD (PDPT[3]). How many walker
     * tables a fresh map here costs depends on what already exists, so count
     * the missing levels instead of hardcoding: since M10 the kernel heap maps
     * its first page at 0x90000000 (same PDPT slot, same PD table) during boot,
     * before tasks clone — so PDPT[2] and its PD are typically already wired
     * and only the PT is new. */
    const u64 SC = 0x80000000ULL;
    u64 tables = 0; /* how many walker tables the map below must still build */
    {
        u64 e = pml4_boot[(SC >> 39) & 511];
        if (!(e & PTE_P) || (e & PTE_PS)) {
            tables = 3; /* PDPT + PD + PT are all new */
        } else {
            u64 *pdpt = (u64 *)(e & PTE_ADDR);
            e = pdpt[(SC >> 30) & 511];
            if (!(e & PTE_P) || (e & PTE_PS)) {
                tables = 2; /* PD + PT are new */
            } else if (!(((u64 *)(e & PTE_ADDR))[(SC >> 21) & 511] & PTE_P)) {
                tables = 1; /* only the PT is new */
            }
        }
    }
    u64 free_pre = pmm_free_frames();
    if (vmm_map_page(SC, f0, VMM_RW) || vmm_map_page(SC + PAGE4K, f1, VMM_RW) ||
        vmm_map_page(SC + 2 * PAGE4K, f2, VMM_RW)) {
        s_puts("[K64] FAIL: vmm_map_page broke\n");
        for (;;) __asm__ __volatile__("cli; hlt");
    }
    /* The walker tables created above are RETAINED by design — only the 3
     * data frames must come back. */
    if (free_pre - pmm_free_frames() != tables) {
        s_puts("[K64] FAIL: page-table accounting broke\n");
        for (;;) __asm__ __volatile__("cli; hlt");
    }
    /* Double-map must be refused. */
    if (vmm_map_page(SC, f2, VMM_RW) == 0) {
        s_puts("[K64] FAIL: double map accepted\n");
        for (;;) __asm__ __volatile__("cli; hlt");
    }
    if (vmm_translate(SC) != f0 || vmm_translate(SC + PAGE4K) != f1 ||
        vmm_translate(SC + 2 * PAGE4K) != f2) {
        s_puts("[K64] FAIL: vmm_translate broke\n");
        for (;;) __asm__ __volatile__("cli; hlt");
    }

    /* Write through the new mapping, verify through the identity alias —
     * proves the PTE points at the right frame, not just that reads echo. */
    volatile u64 *v = (volatile u64 *)SC;
    volatile u64 *v1 = (volatile u64 *)(SC + PAGE4K);
    volatile u64 *v2 = (volatile u64 *)(SC + 2 * PAGE4K);
    v[0] = 0x1122334455667788ULL;
    v[511] = 0xA5A5A5A5A5A5A5A5ULL;
    v1[0] = 0xDEADBEEFCAFEBABEULL;
    v2[100] = 0x0123456789ABCDEFULL;
    if (*(volatile u64 *)f0 != 0x1122334455667788ULL ||
        *(volatile u64 *)(f0 + 511 * 8) != 0xA5A5A5A5A5A5A5A5ULL ||
        *(volatile u64 *)f1 != 0xDEADBEEFCAFEBABEULL ||
        *(volatile u64 *)(f2 + 100 * 8) != 0x0123456789ABCDEFULL) {
        s_puts("[K64] FAIL: mapping alias mismatch\n");
        for (;;) __asm__ __volatile__("cli; hlt");
    }

    if (vmm_unmap_page(SC) || vmm_unmap_page(SC + PAGE4K) ||
        vmm_unmap_page(SC + 2 * PAGE4K) || vmm_translate(SC) != 0) {
        s_puts("[K64] FAIL: vmm_unmap broke\n");
        for (;;) __asm__ __volatile__("cli; hlt");
    }
    pmm_free(f0);
    pmm_free(f1);
    pmm_free(f2);
    /* The 3 data frames return; the walker tables stay (by design). */
    if (pmm_free_frames() != free_pre - tables + 3) {
        s_puts("[K64] FAIL: PMM leak\n");
        for (;;) __asm__ __volatile__("cli; hlt");
    }

    s_puts("[K64] M3 SELFTEST OK (alloc/map/alias/unmap/free exact)\n");
}
