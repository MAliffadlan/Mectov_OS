/* M4 interrupt dispatcher (single core, no locks yet — M6 adds SMP).
 *
 * Returns the rsp to resume: the same frame to continue, or another task's
 * saved rsp after the scheduler (timer/YIELD/EXIT) switches. The asm stub
 * does `mov rsp, rax` before popping GPRs, so the popped frame + iretq
 * target always belong to the resumed task.
 *
 * Vector map: 0-31 CPU exceptions, 32 PIT timer (preempt), 33 keyboard,
 * 44 PS/2 mouse (M9: IRQ12 via the unmasked cascade line), 39/47
 * spurious-IRQ-tolerant EOIs, 128 Ring-3 syscalls (M4 ABI0).
 */
#include "cpu64.h"

static inline void outb(u16 port, u8 v) {
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline u8 inb(u16 port) {
    u8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

#define EOI_MASTER() outb(0x20, 0x20)
#define EOI_SLAVE()  outb(0xA0, 0x20)

u64 isr64_handler(regs64_t *r) {
    u64 v = r->vec;

    if (v == 32) { /* PIT timer (BSP): EOI, then maybe preempt */
        EOI_MASTER();
        return task64_on_tick(r);
    }
    if (v == VEC_AP_TIMER) { /* M7 AP LAPIC timer: reschedule only */
        lapic_eoi();
        return task64_ap_tick(r);
    }
    if (v == 128) { /* Ring-3 syscall (may switch tasks on YIELD/EXIT) */
        return syscall64_dispatch(r);
    }
    if (v == 33) { /* M7.2 keyboard: buffer scancode, EOI */
        /* M9: the 8042's output buffer is shared with the mouse, so route
         * each byte by the AUX status bit instead of trusting the IRQ. */
        ps2_drain64();
        EOI_MASTER();
        return (u64)r;
    }
    if (v == 44) { /* M9 mouse (IRQ12): same drain, both PICs acknowledge */
        ps2_drain64();
        EOI_SLAVE();
        EOI_MASTER();
        return (u64)r;
    }
    if (v == 39) { /* spurious IRQ7: master EOI only */
        EOI_MASTER();
        return (u64)r;
    }
    if (v == 47) { /* spurious IRQ15 */
        EOI_MASTER();
        EOI_SLAVE();
        return (u64)r;
    }
    if (v == VEC_IPI_TEST) { /* M6 SMP self-test ping (ack does LAPIC EOI) */
        smp_ack_test();
        return (u64)r;
    }
    if (v == VEC_IPI_TLB) { /* M6 TLB-shootdown plumbing (ack does EOI) */
        smp_ack_tlb(r->rip); /* invlpg inside; rip arg unused, kept for dbg */
        return (u64)r;
    }
    if (v == VEC_SPURIOUS) { /* LAPIC spurious: NO EOI by architecture */
        smp_ack_spurious();
        return (u64)r;
    }
    if (v == 3) { /* breakpoint: our int3 self-test */
        s_puts("[K64] EXC3 bp rip=");
        s_hex64(r->rip);
        s_puts("\n");
        return (u64)r;
    }
    if (v == 2) { /* NMI: smp_halt_others park (see below) — halt now. */
        for (;;) __asm__ __volatile__("cli; hlt");
    }
    if (v == 14) { /* page fault ladder: COW -> demand -> kill -> FATAL */
        u64 cr2 = cpu_read_cr2();
        if ((r->err & 7) == 7) {
            int cw = vmm_cow_resolve(cr2);
            if (cw >= 0) return (u64)r; /* 0 resolved, 1 spurious (raced) */
        }
        if (!(r->err & 1) && (r->err & 4)) {
            /* Not-present user fault: heap demand window? */
            if (task64_demand(cr2) >= 0) return (u64)r;
        }
        /* Protection faults from Ring 3 that aren't COW/demand: NX fetch
         * and read-only write kill just the task (Unix SIGSEGV ~ 139);
         * anything else (kernel faults, bogus addresses) halts. */
        if (r->cs == 0x1B) {
            u64 fl = 0;
            if (vmm_probe(cr2, &fl) == 0) {
                if ((r->err & 16) && (fl & 8))
                    return task64_kill_fault(r, "NX-KILL");
                if ((r->err & 2) && (fl & 1) && !(fl & 2))
                    return task64_kill_fault(r, "RO-KILL");
            }
        }
        /* Park on the immortal boot tables BEFORE diagnosing: the faulting
         * CR3 may itself be garbage (freed/reused PML4), in which case code
         * fetches, BSS reads and LAPIC MMIO all fault nestedly and the
         * machine triple-resets with zero forensics. Boot maps all of
         * kernel/BSS/MMIO/PMM. Report the saved fault_cr3, halt after. */
        u64 fault_cr3 = cpu_read_cr3() & ~0xFFFULL;
        extern u64 pml4_boot[];
        /* M14 order matters here, and the reason is worth stating: the
         * desktop's console dirty hook composites on every printed character,
         * so a dump printed while it is still attached re-runs the very code
         * that is faulting — the log fills with the dump's first character
         * instead of the dump. Freeze the console first, THEN park on the
         * immortal boot tables, then freeze the other CPUs (NMI ignores IF). */
        cons_set_dirty_hook(0);
        cons_set_hide_hook(0);
        cons_freeze();
        if (fault_cr3 != (u64)pml4_boot) cpu_load_cr3((u64)pml4_boot);
        smp_halt_others();
        s_raws("[K64] FATAL PF err=");
        s_rawx(r->err);
        s_raws(" addr=");
        s_rawx(cr2);
        s_raws(" rip=");
        s_rawx(r->rip);
        s_raws(" cr3=");
        s_rawx(fault_cr3);
        s_raws(" rsp=");
        s_rawx(r->rsp);
        s_raws(" cpu~");
        s_rawu((u64)smp_cpu_by_stack(r->rsp));
        /* M14: walk BOTH roots for the faulting address. One walk says "not
         * present" equally at every level; two walks say whether the page is
         * missing from this address space only (a sharing/clone bug) or from
         * boot's tables as well (a bad pointer). */
        s_raws(" walk(fault):");
        vmm_dump_walk(fault_cr3, cr2);
        s_raws("\n walk(boot) :");
        vmm_dump_walk((u64)pml4_boot, cr2);
        s_raws("\n");
        trace_cr3_dump();
        s_printf("[K64] PFCTX cpu~%u cr3=%x rsp=%x\n",
                 (u64)smp_cpu_by_stack(r->rsp), cpu_read_cr3() & ~0xFFFULL,
                 r->rsp);
        for (;;) __asm__ __volatile__("cli; hlt");
    }
    if (v < 32) {
        u64 fault_cr3 = cpu_read_cr3() & ~0xFFFULL;
        extern u64 pml4_boot[];
        if (fault_cr3 != (u64)pml4_boot) cpu_load_cr3((u64)pml4_boot);
        cons_set_dirty_hook(0); /* M14: see the PF path above */
        cons_set_hide_hook(0);
        cons_freeze();
        smp_halt_others();
        s_raws("[K64] FATAL EXC vec=");
        s_rawu(v);
        s_raws(" err=");
        s_rawx(r->err);
        s_raws(" rip=");
        s_rawx(r->rip);
        s_raws(" cs=");
        s_rawx(r->cs);
        s_raws(" cr3=");
        s_rawx(cpu_read_cr3() & ~0xFFFULL);
        s_raws(" rsp=");
        s_rawx(r->rsp);
        s_raws(" cpu~");
        s_rawu((u64)smp_cpu_by_stack(r->rsp));
        s_raws("\n");
        trace_cr3_dump();
        for (;;) __asm__ __volatile__("cli; hlt");
    }
    s_puts("[K64] unexpected vec=");
    s_dec64(v);
    s_puts("\n");
    if (v >= 40) EOI_SLAVE();
    if (v >= 32) EOI_MASTER();
    return (u64)r;
}
