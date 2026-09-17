/* M6 SMP: AP long-mode bring-up + fixed IPIs + TLB-shootdown plumbing.
 *
 * Model (deliberately minimal — M7 adds per-CPU scheduling): the BSP runs
 * everything; APs park in hlt (IF=1) after setup and only service IPIs.
 * Only LAPIC IDs 1..3 are started (NCPU_MAX=4); larger -smp values leave
 * the extras parked in reset. IPI sequences mirror the proven 32-bit
 * kernel (INIT 0x4500/0x8500, SIPI 0x4600 vec 8, dumb-loop delays).
 *
 * No locks in M6: bring-up is sequential (BSP waits per AP), the BSP is
 * the only IPI sender, and ack slots are per-CPU. M7 needs real spinlocks
 * (k64 has none yet — single core until now) for its AP scheduler.
 */
#include "cpu64.h"

#define LAPIC_BASE 0xFEE00000ULL
#define LAPIC_ID 0x020
#define LAPIC_SIVR 0x0F0
#define LAPIC_TPR 0x080
#define LAPIC_EOI 0x0B0
#define LAPIC_ICR_HI 0x310
#define LAPIC_ICR_LO 0x300
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_DCR 0x3E0
#define LAPIC_INITCNT 0x380
#define LAPIC_CURCNT 0x390

/* Mailbox (tramp64.asm): fixed phys, identity-mapped, PMM-reserved. */
#define MB_STACK 0x7000
#define MB_CR3 0x7008
#define MB_IDX 0x7010
#define MB_ENTRY 0x7018

#define TRAMP_ADDR 0x8000

static volatile u32 apic_ids[NCPU_MAX];
static volatile int ap_ready[NCPU_MAX];
static volatile int ncpus = 1;
static volatile u64 test_ack[NCPU_MAX];
static volatile u64 tlb_ack[NCPU_MAX];
static volatile u64 spur_count[NCPU_MAX];
static volatile u64 pending_va[NCPU_MAX];
static volatile u32 lapic_tick_count = 0; /* M7: LAPIC ticks per 10ms */
static volatile int ap_timer_go[NCPU_MAX];
static u8 ap_stacks[NCPU_MAX][16384] __attribute__((aligned(16)));

extern char _binary_k64_tramp64_bin_start[];
extern char _binary_k64_tramp64_bin_end[];
extern void ap_entry_c(void);

static inline void lapic_write(u32 reg, u32 val) {
    *(volatile u32 *)(LAPIC_BASE + reg) = val;
}
static inline u32 lapic_read(u32 reg) {
    return *(volatile u32 *)(LAPIC_BASE + reg);
}
static u32 lapic_id(void) {
    return lapic_read(LAPIC_ID) >> 24;
}

/* Public LAPIC EOI (AP timer handler; ack paths use it internally too). */
void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

/* Freeze every other CPU with an NMI (it ignores IF). Used once, at the
 * start of a fatal dump: a concurrent cascade (triple fault -> reset)
 * would otherwise truncate the log mid-line. Best effort: if our own
 * tables lack the LAPIC page this faults (nested -> reset), same outcome
 * as without it. NMI handler (vec 2) parks with no prints, no locks. */
void smp_halt_others(void) {
    lapic_write(LAPIC_ICR_HI, 0);
    lapic_write(LAPIC_ICR_LO, 0xC4400); /* shorthand all-excl-self, NMI */
    u64 spin = 100000;
    while ((lapic_read(LAPIC_ICR_LO) & (1 << 12)) && spin--) {
        __asm__ __volatile__("pause");
    }
}
static void lapic_enable(void) {
    /* SIVR: software enable + spurious vector 0xFF (gate installed in
     * idt64_init; spurious needs no EOI). TPR 0: accept all priorities. */
    lapic_write(LAPIC_SIVR, lapic_read(LAPIC_SIVR) | 0x1FF);
    lapic_write(LAPIC_TPR, 0);
}

static void delay_loops(u64 n) {
    for (volatile u64 i = 0; i < n; i++) {
    }
}

/* PIT channel-0 latched read (16-bit down-counter at 1193182Hz). */
static u16 pit_read(void) {
    u8 lo, hi;
    __asm__ __volatile__("outb %0, $0x43" :: "a"((u8)0x00));
    __asm__ __volatile__("inb $0x40, %0" : "=a"(lo));
    __asm__ __volatile__("inb $0x40, %0" : "=a"(hi));
    return (u16)lo | ((u16)hi << 8);
}

/* Measure LAPIC ticks (divide-16 units) over ~20ms of PIT time. Requires
 * the PIT programmed (100Hz, divisor 11931) before smp_init runs. */
static void lapic_calibrate(void) {
    lapic_write(LAPIC_DCR, 0x3);
    lapic_write(LAPIC_LVT_TIMER, 1 << 16); /* masked one-shot */
    lapic_write(LAPIC_INITCNT, 0xFFFFFFFFu);
    u32 acc = 0;
    u16 prev = pit_read();
    while (acc < 2 * 11931) {
        u16 cur = pit_read();
        if (cur <= prev)
            acc += (u32)(prev - cur);
        else
            acc += (u32)prev + (11931 - cur); /* reload wrap */
        prev = cur;
    }
    u32 left = lapic_read(LAPIC_CURCNT);
    lapic_tick_count = (0xFFFFFFFFu - left) / 2; /* per 10ms */
}

/* INIT/SIPI + fixed IPIs. Bounded delivery-status wait (no infinite hang
 * if the LAPIC is missing entirely — ncpus just stays 1). */
static void ipi_send(u32 dest, u32 lo) {
    lapic_write(LAPIC_ICR_HI, dest << 24);
    lapic_write(LAPIC_ICR_LO, lo);
    u64 spin = 100000;
    while ((lapic_read(LAPIC_ICR_LO) & (1 << 12)) && spin--) {
        __asm__ __volatile__("pause");
    }
}

int smp_cpu_count(void) {
    return ncpus;
}

int smp_cpu_index(void) {
    u32 id = lapic_id();
    for (int i = 0; i < ncpus; i++)
        if (apic_ids[i] == id) return i;
    return -1;
}

void smp_ack_test(void) {
    int i = smp_cpu_index();
    if (i >= 0) test_ack[i]++;
    lapic_write(LAPIC_EOI, 0);
}

void smp_ack_tlb(u64 unused) {
    (void)unused;
    int i = smp_cpu_index();
    if (i >= 0) {
        u64 va = pending_va[i];
        if (va == ~0ULL) {
            /* Full flush (clone/exec/teardown publish): reload CR3. */
            u64 cr3;
            __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
            __asm__ __volatile__("mov %0, %%cr3" :: "r"(cr3) : "memory");
        } else {
            __asm__ __volatile__("invlpg (%0)" :: "r"(va) : "memory");
        }
        tlb_ack[i]++;
    }
    lapic_write(LAPIC_EOI, 0);
}

void smp_ack_spurious(void) {
    int i = smp_cpu_index();
    if (i >= 0) spur_count[i]++;
    /* No EOI for the spurious vector, by architecture. */
}

/* LAPIC-free CPU guess for fatal dumps (smp_cpu_index needs MMIO, which
 * may be exactly what's unmapped). AP boot stacks are static: hit = AP. */
int smp_cpu_by_stack(u64 rsp) {
    for (int i = 1; i < NCPU_MAX; i++) {
        u64 lo = (u64)ap_stacks[i], hi = lo + sizeof(ap_stacks[i]);
        if (rsp >= lo && rsp < hi) return i;
    }
    return 0; /* BSP (boot/task stacks) or unknown */
}

/* AP C entry (from trampoline via ap_entry_c, RDI = index, IF=0). Phase 1
 * parks after setup (BSP runs self-tests); phase 2 starts the LAPIC timer
 * and falls into the scheduler on its first tick (never returns here). */
void ap_main(u64 idx) {
    if (idx == 0 || idx >= (u64)NCPU_MAX) {
        for (;;) __asm__ __volatile__("cli; hlt");
    }
    int i = (int)idx;
    gdt64_ap_load(i, (u64)(ap_stacks[i] + sizeof(ap_stacks[i])));
    idt64_load();
    paging_enable_nxe_ap(); /* before any user NX page is touched */
    lapic_enable();
    /* No ExtINT/NMI via the PIC on APs (BSP keeps the legacy IRQs). */
    lapic_write(LAPIC_LVT_LINT0, 0x10000);
    lapic_write(LAPIC_LVT_LINT1, 0x10000);
    apic_ids[i] = lapic_id();
    ap_ready[i] = 1;
    __asm__ __volatile__("sti");
    while (!ap_timer_go[i]) __asm__ __volatile__("hlt"); /* phase-1 park */
    /* Phase 2: 100Hz periodic LAPIC timer (BSP-calibrated count). */
    lapic_write(LAPIC_DCR, 0x3);
    lapic_write(LAPIC_LVT_TIMER, VEC_AP_TIMER | 0x20000);
    lapic_write(LAPIC_INITCNT, lapic_tick_count ? lapic_tick_count : 1000000);
    for (;;) __asm__ __volatile__("hlt"); /* first tick -> scheduler */
}

/* M7: BSP-only, after sti. Releases APs into the scheduler (a TEST IPI
 * wakes each hlt-parked AP; its handler acks harmlessly). */
void smp_wake_aps(void) {
    for (int i = 1; i < ncpus; i++) ap_timer_go[i] = 1;
    for (int i = 1; i < ncpus; i++) ipi_send(apic_ids[i], VEC_IPI_TEST);
}

void smp_init(void) {
    lapic_enable(); /* BSP first: ICR/SIVR need a live LAPIC */
    apic_ids[0] = lapic_id();
    ap_ready[0] = 1;
    ncpus = 1;
    lapic_calibrate(); /* PIT must already run at 100Hz (kernel order) */

    /* Mailbox: zero (raw BIOS-area RAM, never assume zeroed). */
    volatile u64 *mb = (volatile u64 *)MB_STACK;
    for (int i = 0; i < 8; i++) mb[i] = 0;

    /* Trampoline -> 0x8000 (both sides identity-mapped low). */
    u64 tlen =
        (u64)(_binary_k64_tramp64_bin_end - _binary_k64_tramp64_bin_start);
    if (!tlen || tlen > 2048) {
        s_puts("[K64] M6 no trampoline image, SMP disabled\n");
        return;
    }
    u8 *d = (u8 *)TRAMP_ADDR;
    u8 *s = (u8 *)_binary_k64_tramp64_bin_start;
    for (u64 i = 0; i < tlen; i++) d[i] = s[i];

    for (int ap = 1; ap < NCPU_MAX; ap++) {
        u32 lapic = (u32)ap; /* QEMU enumerates LAPIC IDs sequentially */
        *(volatile u64 *)MB_STACK =
            (u64)(ap_stacks[ap] + sizeof(ap_stacks[ap]));
        *(volatile u64 *)MB_CR3 = cpu_read_cr3() & ~0xFFFULL;
        *(volatile u32 *)MB_IDX = (u32)ap;
        *(volatile u64 *)MB_ENTRY = (u64)ap_entry_c;

        ipi_send(lapic, 0x4500); /* INIT assert */
        delay_loops(1000000);
        ipi_send(lapic, 0x8500); /* INIT deassert */
        delay_loops(1000000);
        ipi_send(lapic, 0x4600 | 0x08); /* SIPI 1 */
        delay_loops(20000);
        ipi_send(lapic, 0x4600 | 0x08); /* SIPI 2 */

        u64 spin = 20000000;
        while (!ap_ready[ap] && spin--) {
            __asm__ __volatile__("pause");
        }
        if (ap_ready[ap]) {
            ncpus = ap + 1;
            s_printf("[K64] M6 AP%u awake (LAPIC %u)\n", (u64)ap,
                     (u64)apic_ids[ap]);
        } else {
            s_printf("[K64] M6 AP%u timeout (not started)\n", (u64)ap);
        }
    }
    s_puts("[K64] M6 ncpus=");
    s_dec64((u64)ncpus);
    s_puts("\n");
}

/* Shoot one VA (or ~0ULL for a full flush) on every started AP.
 * Returns acked count. LOCKING: callers must hold NO spinlock across this
 * (an AP spinning on that lock with IF=0 could never ack -> deadlock).
 * AP handlers take no locks, so the wait always terminates (bounded).
 *
 * The wait runs with IF=1: a simultaneous shootdown from another CPU would
 * otherwise deadlock us mutually (both IF=0, neither acking — observed
 * live: all 4 CPUs parked in this loop). Task switches are suppressed for
 * this CPU while waiting (smp_in_shootdown): timer IRQs still refresh the
 * current frame, but no other task may start — otherwise a nested
 * shootdown could corrupt these same ack slots, and boot-time waits could
 * start demos before boot finishes. */
static volatile int shoot_busy[NCPU_MAX];

int smp_in_shootdown(void) {
    int me = smp_cpu_index();
    return (me >= 0 && me < NCPU_MAX) ? shoot_busy[me] : 0;
}

int smp_shootdown(u64 va) {
    if (ncpus < 2) return 0;
    int me = smp_cpu_index();
    for (int i = 1; i < ncpus; i++) {
        pending_va[i] = va;
        tlb_ack[i] = 0;
        ipi_send(apic_ids[i], VEC_IPI_TLB);
    }
    if (me >= 0 && me < NCPU_MAX) shoot_busy[me] = 1;
    __asm__ __volatile__("sti"); /* take IPIs (incl. others' shootdowns) */
    int ok = 0;
    for (int i = 1; i < ncpus; i++) {
        u64 spin = 20000000;
        while (!tlb_ack[i] && spin--) {
            __asm__ __volatile__("pause");
        }
        if (tlb_ack[i]) ok++;
    }
    __asm__ __volatile__("cli");
    if (me >= 0 && me < NCPU_MAX) shoot_busy[me] = 0;
    return ok;
}

void smp_shootdown_all(void) {
    smp_shootdown(~0ULL);
}
int smp_selftest(void) {
    if (ncpus < 2) {
        s_puts("[K64] M6 single-CPU, IPI test skipped\n");
        return 1;
    }
    int tp = 0, lp = 0;
    for (int i = 1; i < ncpus; i++) {
        test_ack[i] = 0;
        ipi_send(apic_ids[i], VEC_IPI_TEST);
    }
    for (int i = 1; i < ncpus; i++) {
        u64 spin = 20000000;
        while (!test_ack[i] && spin--) {
            __asm__ __volatile__("pause");
        }
        if (test_ack[i]) tp++;
    }
    /* Single-VA shootdown (scratch VA; invlpg never faults) plus one full
     * flush through the same public API the vmm paths use. */
    lp = (smp_shootdown(0x90000000ULL) == ncpus - 1) ? ncpus - 1 : 0;
    smp_shootdown_all();
    s_printf("[K64] M6 IPI-OK %u/%u TLB-OK %u/%u\n", (u64)tp,
             (u64)(ncpus - 1), (u64)lp, (u64)(ncpus - 1));
    return tp == ncpus - 1 && lp == ncpus - 1;
}
