#include "../include/idt.h"
#include "../include/io.h"
#include "../include/task.h"
#include "../include/utils.h"
#include "../include/serial.h"
#include "../include/panic.h"

idt_entry_t idt[256];
idt_ptr_t   idt_ptr;
isr_t       interrupt_handlers[256];

// Assembly stubs (from interrupt_entry.asm)
extern void isr0();
extern void isr1();
extern void isr2();
extern void isr3();
extern void isr4();
extern void isr5();
extern void isr6();
extern void isr7();
extern void isr8();
extern void isr9();
extern void isr10();
extern void isr11();
extern void isr12();
extern void isr13();
extern void isr14();
extern void isr16();
extern void isr17();
extern void isr18();
extern void isr19();
extern void isr20();
extern void isr21();
extern void isr128();
extern void isr_default();
extern void irq0();
extern void irq1();
extern void irq5();
extern void irq11();
extern void irq12();
extern void irq14();
extern void irq15();
extern void idt_flush(uint32_t);

// Per-CPU exception stacks (interrupt_entry.asm): one 4KB stack per CPU so
// a corrupt kernel stack cannot prevent a #PF/#DF handler from running, and
// concurrent faults on different cores (e.g. COW promotion under fork load)
// never clobber each other's saved ESP. Each CPU's top must be set before
// any interrupt can fire; idt_init() runs before interrupts are enabled.
extern uint32_t fault_stacks[];
extern uint32_t fault_stack_tops[];

void idt_init_fault_stacks(void) {
    for (int i = 0; i < 16; i++) {
        fault_stack_tops[i] = (uint32_t)(uintptr_t)fault_stacks + (i + 1) * 4096;
    }
}

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_lo = base & 0xFFFF;
    idt[num].base_hi = (base >> 16) & 0xFFFF;
    idt[num].sel     = sel;
    idt[num].always0 = 0;
    idt[num].flags   = flags;
}
// Overflow exception handler (INT 4 / INTO instruction)
// GCC -fPIC generates INTO instructions for bounds checking.
// We just log and return — overflow is benign for our apps.
static void overflow_handler(registers_t* regs) {
    (void)regs;
    write_serial_string("[INT4] Overflow (ignored)\n");
}

// #NM (vector 7, device not available): the eager FPU scheme (fpu.c,
// v38.41) never sets CR0.TS, so this trap means FPU state got out of sync
// somewhere. Clear TS, log, and continue rather than fault-looping.
static void nm_handler(registers_t* regs) {
    __asm__ __volatile__("clts");
    write_serial_string("[FPU] unexpected #NM (TS was set, cleared), EIP=");
    write_serial_hex(regs->eip);
    write_serial_string("\n");
}

// #XM (vector 19, SIMD floating-point exception): an unmasked SSE
// exception from Ring 3. Route it through the signal framework like any
// other synchronous fault — an installed SIGFPE handler runs, the default
// action terminates. Kernel-mode #XM is a kernel bug (the kernel core
// never emits SIMD instructions): dump and halt.
static void xm_handler(registers_t* regs) {
    if ((regs->cs & 3) == 3) {
        extern int task_fault_signal(int sig, void* frame);
        task_fault_signal(SIGFPE, regs);
        return;
    }
    write_serial_string("\n[KERNEL PANIC] SIMD exception (#XM) in kernel mode, EIP=");
    write_serial_hex(regs->eip);
    write_serial_string("\n");
    panic_finish();
    for (;;) { __asm__ __volatile__("hlt"); }
}

// Atomic-line helpers for the exception path. Every line is built into a
// local buffer and emitted with ONE write_serial_try() call: locked when the
// lock is free (no interleaving with other CPUs), raw when it is not (the
// pre-exception context already holds serial_lock — spinning would freeze the
// whole system).
static char* str_append(char* p, const char* s) {
    while (*s) *p++ = *s++;
    return p;
}

static char* hex_append(char* p, uint32_t v) {
    *p++ = '0'; *p++ = 'x';
    for (int i = 28; i >= 0; i -= 4) {
        int n = (int)((v >> i) & 0xF);
        *p++ = (char)(n < 10 ? '0' + n : 'A' + (n - 10));
    }
    return p;
}

void idt_init() {
    idt_init_fault_stacks();
    idt_ptr.limit = sizeof(idt_entry_t) * 256 - 1;
    idt_ptr.base  = (uint32_t)&idt;
    memset(&idt, 0, sizeof(idt_entry_t) * 256);
    for (int i = 0; i < 256; i++) {
        interrupt_handlers[i] = 0;
        idt_set_gate(i, (uint32_t)isr_default, 0x08, 0x8E); // DPL=0 interrupt gate
    }

    // If APIC is present, disable PIC
    extern uint32_t smp_lapic_addr;
    if (smp_lapic_addr) {
        write_serial_string("[IDT] Disabling legacy PIC\n");
        outb(0x21, 0xFF);
        outb(0xA1, 0xFF);
        // Initialization of APIC/IOAPIC will happen in kernel_main
    } else {
        // PIC: remap IRQ0-7 → INT 32-39, IRQ8-15 → INT 40-47
        outb(0x20, 0x11); outb(0xA0, 0x11);
        outb(0x21, 0x20); outb(0xA1, 0x28);
        outb(0x21, 0x04); outb(0xA1, 0x02);
        outb(0x21, 0x01); outb(0xA1, 0x01);
        // Unmask: IRQ0 (timer), IRQ1 (keyboard), IRQ2 (cascade), IRQ5 (SB16),
        // IRQ11 (RTL8139 NIC), IRQ12 (mouse), IRQ14/15 (IDE BMIDE DMA)
        // Primary: 0x98 = 1001_1000: unmask bits 0,1,2,3,5,6 (IRQ0 timer,
        // IRQ1 kb, IRQ2 cascade, IRQ3 COM2, IRQ5 SB16, IRQ14 IDE primary);
        // mask the rest. (0xD8 previously masked IRQ14, so BMIDE DMA would
        // never see its completion interrupt.)
        outb(0x21, 0x98);
        // Slave: 0x67 = 0110_0111: unmask bits 0,1,2,3,5,7 (IRQ8 RTC, IRQ9,
        // IRQ10, IRQ11 NIC, IRQ13 FPU, IRQ15 IDE secondary).
        outb(0xA1, 0x67);
    }

    // CPU Exceptions. Every real fault vector gets a dedicated stub so the
    // frame is never misaligned by isr_default's dummy error code (see the
    // ISR_WITH_ERR/ISR_NO_ERR macros in interrupt_entry.asm): a CPU-pushed
    // error code shifted registers_t by 4 bytes, so an unregistered fault
    // (e.g. #DF/#TS/#SS) silently triple-faulted instead of panicking with
    // a real int_no. All are DPL=0 interrupt gates so Ring 3 cannot invoke
    // them directly.
    // v38.53: #DB and #BP are now DPL=0 like every other fault. The GDB stub
    // is entered from the kernel side (F12 in the main loop, the serial poll
    // path, and real kernel traps), so Ring 3 never needed these gates open —
    // and with DPL=3 ANY user program could fire `int3`/`int1` and walk the
    // machine into the debugger loop (freeze / full register+memory control).
    idt_set_gate(0,  (uint32_t)isr0,  0x08, 0x8E);  // #DE Division by Zero
    idt_set_gate(1,  (uint32_t)isr1,  0x08, 0x8E);  // #DB Debug/single-step
    idt_set_gate(2,  (uint32_t)isr2,  0x08, 0x8E);  // NMI
    idt_set_gate(3,  (uint32_t)isr3,  0x08, 0x8E);  // #BP Breakpoint
    idt_set_gate(4,  (uint32_t)isr4,  0x08, 0xEE);  // #OF Overflow (DPL=3, INTO from Ring 3)
    idt_set_gate(5,  (uint32_t)isr5,  0x08, 0x8E);  // #BR Bound Range Exceeded
    idt_set_gate(6,  (uint32_t)isr6,  0x08, 0x8E);  // #UD Invalid Opcode
    idt_set_gate(7,  (uint32_t)isr7,  0x08, 0x8E);  // #NM Device Not Available
    // #DF Double Fault as a TASK GATE (selector 0x30 = the dedicated df_tss
    // in gdt.c). A stack overflow usually faults mid-push, so the CPU cannot
    // push the #DF frame on the corrupt stack and would triple-fault before
    // any interrupt-gate handler ran; the hardware task switch never touches
    // the old stack (state goes into the old TSS) and runs df_task_handler on
    // its own stack + CR3, which prints a clean panic and halts.
    idt_set_gate(8,  0,          0x30, 0x85);  // #DF Double Fault (task gate)
    idt_set_gate(9,  (uint32_t)isr9,  0x08, 0x8E);  // Coprocessor Segment Overrun
    idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8E);  // #TS Invalid TSS
    idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8E);  // #NP Segment Not Present
    idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8E);  // #SS Stack-Segment Fault
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E);  // #GP General Protection Fault
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E);  // #PF Page Fault
    idt_set_gate(16, (uint32_t)isr16, 0x08, 0x8E);  // #MF x87 Floating-Point Error
    idt_set_gate(17, (uint32_t)isr17, 0x08, 0x8E);  // #AC Alignment Check
    idt_set_gate(18, (uint32_t)isr18, 0x08, 0x8E);  // #MC Machine Check
    idt_set_gate(19, (uint32_t)isr19, 0x08, 0x8E);  // #XF SIMD Floating-Point Exception
    idt_set_gate(20, (uint32_t)isr20, 0x08, 0x8E);  // #VE Virtualization Exception
    idt_set_gate(21, (uint32_t)isr21, 0x08, 0x8E);  // #CP Control Protection

    // Hardware IRQs
    idt_set_gate(32, (uint32_t)irq0,  0x08, 0x8E);  // Timer
    idt_set_gate(33, (uint32_t)irq1,  0x08, 0x8E);  // Keyboard
    idt_set_gate(37, (uint32_t)irq5,  0x08, 0x8E);  // Sound (SB16, IRQ5)
    idt_set_gate(43, (uint32_t)irq11, 0x08, 0x8E);  // Network (RTL8139, IRQ11)
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);  // Mouse
    idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E);  // IDE primary (BMIDE DMA, IRQ14)
    idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E);  // IDE secondary (BMIDE DMA, IRQ15)

    // Syscall gate: int 0x80, DPL=3 (Ring 3 can call this).
    // TRAP gate (0xEF, not 0xEE): the CPU does NOT auto-clear IF on entry, so
    // the syscall handler is preemptible by the timer. isr128 manually cli()s
    // while pushing its frame so the registers_t stays deterministically at the
    // top of the kernel stack (fork/exec rely on that), then sti()s before
    // calling the handler. This keeps the door open for preemptible kernel
    // handlers while preserving every existing frame-layout assumption.
    idt_set_gate(0x80, (uint32_t)isr128, 0x08, 0xEF);

    idt_flush((uint32_t)&idt_ptr);

    // Register Overflow handler (Exception 4) — just ignore and return
    register_interrupt_handler(4, overflow_handler);

    // FPU exception routing (v38.41): #NM as a belt-and-braces safety net
    // for the eager fxsave scheduler, #XM as a synchronous SIGFPE for
    // Ring 3 SSE code.
    register_interrupt_handler(7, nm_handler);
    register_interrupt_handler(19, xm_handler);

    // NMI (vector 2): during a panic dump this snapshots the core's registers
    // and parks (see panic.c); otherwise it returns so GDB/machine-check keep
    // working. Registered here so every CPU (APs reload the same IDT) can
    // answer the BSP's NMI-IPI during a multi-core panic dump.
    register_interrupt_handler(2, panic_nmi_handler);
}

void register_interrupt_handler(uint8_t n, isr_t handler) {
    interrupt_handlers[n] = handler;
}

void idt_load_ap() {
    idt_flush((uint32_t)&idt_ptr);
    write_serial_string("[IDT] Loaded for AP\n");
}

// IRQ handler — called from irq_common_stub
// Returns (possibly new) ESP for context switching
uint32_t irq_handler(uint32_t esp) {
    registers_t *r = (registers_t*)esp;
    
    // Send EOI
    extern uint32_t smp_lapic_addr;
    extern void apic_send_eoi(void);
    
    if (smp_lapic_addr) {
        apic_send_eoi();
    } else {
        if (r->int_no >= 40) outb(0xA0, 0x20);
        outb(0x20, 0x20);
    }
    
    // Call registered handler
    if (interrupt_handlers[r->int_no] != 0) {
        isr_t handler = interrupt_handlers[r->int_no];
        handler(r);
    }
    
    // Context switch on timer interrupt (IRQ0 = int 32)
    if (r->int_no == 32) {
        return schedule(esp);
    }
    return esp;
}

// ISR handler — called from isr_common_stub and isr128 (syscalls)
// Does NOT do context switching or EOI
#include "../include/vga.h"
#include "../include/vmm.h"
#include "../include/mem.h"

// Lazy zero-fill for a demand-paged Ring 3 page (heap or user stack). A
// WRITE fault gets a private zeroed frame mapped RW; a READ fault maps the
// shared zero page read-only with the PAGE_COW marker so the first write
// duplicates it. Returns 1 when the fault is resolved (resume the faulting
// instruction), 0 when no frame could be produced (fall through to kill).
// Runs in exception context (fault stack, IF=0) — logging is try-write only.
// v38.49: heap/stack pages are DATA — mark them no-execute when EFER.NXE is
// on, so executing from either faults (W^X for the default user layout).
static int demand_map_zero(uint32_t cr3_val, uint32_t addr, uint32_t err_code) {
    uint64_t nx = 0;
    extern int paging_nx_enabled(void);
    if (paging_nx_enabled()) nx = PAGE_NX;
    uint32_t phys;
    if (err_code & 2) {  // write fault -> private zeroed frame
        phys = frame_alloc();
        if (phys == 0) return 0;
        vmm_map_page(cr3_val, addr, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER | nx);
        memset((void*)(addr & 0xFFFFF000), 0, 4096);
    } else {  // read fault -> shared zero page (RO + COW marker)
        phys = phys_get_zero_page();
        if (phys == 0) return 0;
        vmm_map_page(cr3_val, addr, phys, PAGE_PRESENT | PAGE_USER | PAGE_COW | nx);
    }
    char b[96];
    char* q = b;
    q = str_append(q, "[VMM] demand paged ");
    q = hex_append(q, addr);
    q = str_append(q, (err_code & 2) ? " (write, private frame)\n" : " (read, shared zero)\n");
    write_serial_try(b, (int)(q - b));
    return 1;
}

void isr_handler(registers_t *r) {
    // GDB stub: single-step (int 1) and breakpoint (int 3) traps are
    // consumed here when the debugger is active, so they never reach the
    // normal exception path (which would kill the task / panic the kernel).
    if (r->int_no == 1 || r->int_no == 3) {
        extern int gdb_stub_handle_trap(registers_t*);
        if (gdb_stub_handle_trap(r)) return;
    }

    // #DF Double Fault: the CPU faulted while handling another fault —
    // usually a corrupt kernel stack (overflow, or a fault inside a fault
    // handler). No task can survive this; panic immediately with a clear
    // message instead of falling into the generic path (which would print
    // EIP/CS and hlt — fine, but the dedicated banner makes the failure
    // mode obvious in serial logs).
    if (r->int_no == 8) {
        char buf[96];
        char* p = buf;
        p = str_append(p, "\n[PANIC] DOUBLE FAULT (int 8) at EIP=");
        p = hex_append(p, r->eip);
        p = str_append(p, " CS=");
        p = hex_append(p, r->cs);
        p = str_append(p, " - kernel stack corruption, halting\n");
        write_serial_try(buf, (int)(p - buf));
        print("\n[KERNEL PANIC] Double Fault - kernel stack corrupted\n", 0x0C);
        panic_finish();
    }

    if (r->int_no == 14) {
        uint32_t faulting_address;
        __asm__ __volatile__("mov %%cr2, %0" : "=r"(faulting_address));

        // ---- Kernel stack overflow detection ----
        // The 4KB page below every task's kernel stack is unmapped; a fault
        // into one means the stack ran past its end. The register frame (and
        // often the page tables) are already corrupted, so the only safe
        // action is a clear panic. In the worst case the CPU faults while
        // pushing the #PF frame itself and we get a #DF instead — that has
        // its own banner above.
        extern int task_is_stack_guard(uint32_t addr);
        if (task_is_stack_guard(faulting_address)) {
            extern int get_current_task(void);
            extern uint32_t task_stack_top(int tid);
            int ctid = get_current_task();
            char buf[128];
            char* p = buf;
            p = str_append(p, "\n[PANIC] KERNEL STACK OVERFLOW at ");
            p = hex_append(p, faulting_address);
            p = str_append(p, " task ");
            p = hex_append(p, (uint32_t)ctid);
            p = str_append(p, " esp0=");
            p = hex_append(p, task_stack_top(ctid));
            p = str_append(p, " EIP=");
            p = hex_append(p, r->eip);
            p = str_append(p, " CS=");
            p = hex_append(p, r->cs);
            p = str_append(p, "\n");
            write_serial_try(buf, (int)(p - buf));
            print("\n[KERNEL PANIC] Kernel stack overflow - halting\n", 0x0C);
            panic_finish();
        }

        uint32_t cr3_val;
        __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3_val));

        // Instruction-fetch fault (err bit 4 = I/D): with W^X active this is
        // an execute-from-data violation — a present NX page, or an unmapped
        // fetch target. NEVER demand-map it (mapping zeroed data and running
        // it is exactly what NX exists to prevent): log once and fall to the
        // generic path below, which delivers SIGSEGV (Ring 3) or panics
        // (Ring 0). There is no demand-paged CODE in this kernel — images
        // and the signal trampoline are mapped eagerly — so every fetch
        // fault is genuine.
        if (r->err_code & 0x10) {
            char w[96];
            char* wp = w;
            wp = str_append(wp, "[W^X] execute fault at ");
            wp = hex_append(wp, faulting_address);
            wp = str_append(wp, " EIP=");
            wp = hex_append(wp, r->eip);
            wp = str_append(wp, "\n");
            write_serial_try(w, (int)(wp - w));
        } else {

        // PAE walk: CR3 -> PDPT -> PD -> PT (v38.49). All structures live in
        // the identity map, so the physical pointers dereference directly.
        pte_t* pdpt = (pte_t*)(uintptr_t)cr3_val;
        pte_t* pd = 0;
        pte_t* pt = 0;
        uint32_t pt_idx = pt_index(faulting_address);

        if (pdpt[pdpt_index(faulting_address)] & PAGE_PRESENT) {
            pd = (pte_t*)(uintptr_t)(uint32_t)(pdpt[pdpt_index(faulting_address)] & PTE_ADDR_MASK);
            if (pd[pd_index(faulting_address)] & PAGE_PRESENT) {
                pt = (pte_t*)(uintptr_t)(uint32_t)(pd[pd_index(faulting_address)] & PTE_ADDR_MASK);
            }
        }

        if (pt && (pt[pt_idx] & PAGE_PRESENT)) {
            pte_t entry = pt[pt_idx];
            if (entry & PAGE_COW) {
                uint32_t old_paddr = (uint32_t)(entry & PTE_ADDR_MASK);

                // Sole ownership optimization. The refcount read is locked
                // against frame_alloc/frame_free on other cores (kernel
                // locking audit): an unlocked read could observe a torn
                // count mid-update and wrongly promote a still-shared page.
                extern void vmm_lock_acquire_irq(void);
                extern void vmm_lock_release_irq(void);
                vmm_lock_acquire_irq();
                int sole_owner = (frame_ref_count[old_paddr / 4096] == 1);
                vmm_lock_release_irq();
                if (sole_owner) {
                    // Keep every non-address flag (incl. PAGE_NX) exactly as
                    // it was; only RW/COW change.
                    entry = (entry & ~PTE_ADDR_MASK & ~PAGE_COW) | PAGE_RW | old_paddr;
                    pt[pt_idx] = entry;
                    __asm__ __volatile__("invlpg (%0)" : : "r"(faulting_address));

                    // Exception context (fault stack, IF=0): atomic try-write
                    // so logging can never self-deadlock on serial_lock.
                    char b1[96];
                    char* q1 = b1;
                    q1 = str_append(q1, "[COW] Promoted sole-owned page to writable at ");
                    q1 = hex_append(q1, faulting_address);
                    q1 = str_append(q1, "\n");
                    // Routine diagnostic: skip when the serial lock is
                    // contended so this log never garbles another core's
                    // locked line with raw bytes (seen: "fork: child"
                    // split under real SMP). Panic/error paths should
                    // keep write_serial_try's raw fallback.
                    write_serial_if_free(b1, (int)(q1 - b1));
                    return; // Resume execution
                }

                // Duplicate page
                uint32_t new_paddr = frame_alloc();
                if (new_paddr == 0) {
                    char b2[96];
                    char* q2 = b2;
                    q2 = str_append(q2, "[COW] OOM during COW fault at ");
                    q2 = hex_append(q2, faulting_address);
                    q2 = str_append(q2, " - falling through to kill task\n");
                    write_serial_try(b2, (int)(q2 - b2));
                    // Falls through to the unhandled-exception path below,
                    // which calls task_exit() for Ring 3 or panics for Ring 0.
                } else {
                    memcpy((void*)(uintptr_t)new_paddr, (void*)(uintptr_t)old_paddr, 4096);

                    entry = (entry & ~PTE_ADDR_MASK & ~PAGE_COW) | PAGE_RW | new_paddr;
                    pt[pt_idx] = entry;

                    frame_free(old_paddr);
                    __asm__ __volatile__("invlpg (%0)" : : "r"(faulting_address));

                    char b3[128];
                    char* q3 = b3;
                    q3 = str_append(q3, "[COW] Duplicated page at ");
                    q3 = hex_append(q3, faulting_address);
                    q3 = str_append(q3, " (Old: ");
                    q3 = hex_append(q3, old_paddr);
                    q3 = str_append(q3, " -> New: ");
                    q3 = hex_append(q3, new_paddr);
                    q3 = str_append(q3, ")\n");
                    // Routine diagnostic: skip when serial lock is
                    // contended (same rationale as above).
                    write_serial_if_free(b3, (int)(q3 - b3));
                    return; // Resume execution
                }
            }
        }
        // --- Demand Paging for mmap() regions ---
        // Reserved mmap ranges live at 0x40000000..0x80000000 with no frames;
        // lazily map a zeroed frame per page on first access. err_code tells
        // the handler whether this is a write fault (file-backed mappings map
        // pages RO and mark them dirty + RW on the first write).
        if ((r->cs & 3) == 3 && faulting_address >= MMAP_BASE && faulting_address < MMAP_END) {
            extern int task_mmap_handle_fault(uint32_t addr, uint32_t cr3, uint32_t err);
            if (task_mmap_handle_fault(faulting_address, cr3_val, r->err_code)) {
                return; // Resume execution, instruction will restart
            }
        }
        // --- Demand Paging for Ring 3 Heap (lazy zero-fill) ---
        if ((r->cs & 3) == 3 && faulting_address >= 0x08000000 && faulting_address < 0x20000000) {
            extern int get_current_task(void);
            extern uint32_t task_get_heap_ptr(int tid);
            int tid = get_current_task();
            uint32_t max_heap = task_get_heap_ptr(tid);
            
            if (faulting_address < max_heap) {
                // Address is within the reserved heap space. A write gets a
                // private zeroed frame; a read maps the shared zero page
                // (RO + COW) so untouched heap costs no private frames.
                if (demand_map_zero(cr3_val, faulting_address, r->err_code)) {
                    return; // Resume execution, instruction will restart
                }
                write_serial_try("[VMM] OUT OF MEMORY during Heap Demand Paging!\n", (int)strlen("[VMM] OUT OF MEMORY during Heap Demand Paging!\n"));
            }
        }

        // --- Demand Paging for Ring 3 User Stack (lazy zero-fill) ---
        // Each task's stack occupies [TOP-(tid+2)*SIZE, TOP-(tid+1)*SIZE) in
        // its address space (task.c uses the same slot discipline). The page
        // below that range is the guard page and stays permanently unmapped,
        // so a genuine stack overflow faults there and kills the task instead
        // of corrupting whatever sits underneath.
        if ((r->cs & 3) == 3) {
            extern int get_current_task(void);
            int stid = get_current_task();
            uint32_t s_top = USER_STACK_TOP - ((uint32_t)stid + 1) * USER_STACK_SIZE;
            if (faulting_address >= (s_top - USER_STACK_SIZE) && faulting_address < s_top) {
                if (demand_map_zero(cr3_val, faulting_address, r->err_code)) {
                    return; // Resume execution, instruction will restart
                }
                write_serial_try("[VMM] OUT OF MEMORY during Stack Demand Paging!\n", (int)strlen("[VMM] OUT OF MEMORY during Stack Demand Paging!\n"));
            }
        }
        }  // end of the non-fetch (data) fault paths
    }

    if (interrupt_handlers[r->int_no] != 0) {
        isr_t handler = interrupt_handlers[r->int_no];
        handler(r);
    } else {
        // Unhandled exception — one atomic line (try-write, deadlock-free).
        {
            char buf[160];
            char* p = buf;
            p = str_append(p, "\n[EXCEPTION] int_no=");
            p = hex_append(p, r->int_no);
            p = str_append(p, " CS=");
            p = hex_append(p, r->cs);
            p = str_append(p, " err=");
            p = hex_append(p, r->err_code);
            p = str_append(p, " EIP=");
            p = hex_append(p, r->eip);
            p = str_append(p, " EFL=");
            p = hex_append(p, r->eflags);
            p = str_append(p, " DS=");
            p = hex_append(p, r->ds);
            p = str_append(p, " EAx=");
            p = hex_append(p, r->eax);
            p = str_append(p, " ECx=");
            p = hex_append(p, r->ecx);
            p = str_append(p, " EDx=");
            p = hex_append(p, r->edx);
            p = str_append(p, "\n");
            write_serial_try(buf, (int)(p - buf));
        }
        
        uint32_t cs = r->cs;
        if ((cs & 3) == 3) {
            // Ring 3 fault. If the task installed a SIGSEGV handler it runs
            // (frame rewritten by task_fault_signal; the faulting instruction
            // is re-executed when the handler returns, so a handler that
            // fixes the cause — or exits — makes progress); otherwise the
            // default action terminates the task with exit status 128+SIGSEGV
            // after full cleanup (WM + VMM + fd). Never iret back into the
            // faulting user instruction when killing: task_fault_signal parks
            // the frame in the kernel.
            extern int get_current_task(void);
            int crashed_tid = get_current_task();
            {
                char buf[96];
                char* p = buf;
                p = str_append(p, "[CRASH] Ring 3 fault, task ");
                p = hex_append(p, (uint32_t)crashed_tid);
                p = str_append(p, "\n");
                write_serial_try(buf, (int)(p - buf));
            }
            if (r->int_no == 14) {
                uint32_t cr2;
                __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
                char buf[96];
                char* p = buf;
                p = str_append(p, "PF addr: ");
                p = hex_append(p, cr2);
                p = str_append(p, " err=");
                p = hex_append(p, r->err_code);
                p = str_append(p, "\n");
                write_serial_try(buf, (int)(p - buf));
            }
            {
                char buf[160];
                char* p = buf;
                p = str_append(p, "EIP: ");
                p = hex_append(p, r->eip);
                p = str_append(p, " ESP: ");
                p = hex_append(p, r->useresp);
                p = str_append(p, " EBP: ");
                p = hex_append(p, r->ebp);
                p = str_append(p, " EAX: ");
                p = hex_append(p, r->eax);
                p = str_append(p, " EBX: ");
                p = hex_append(p, r->ebx);
                p = str_append(p, " ECX: ");
                p = hex_append(p, r->ecx);
                p = str_append(p, " EDX: ");
                p = hex_append(p, r->edx);
                p = str_append(p, " CR3: ");
                uint32_t cr3_val;
                __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3_val));
                p = hex_append(p, cr3_val);
                p = str_append(p, "\n");
                write_serial_try(buf, (int)(p - buf));
            }
            // Deliver SIGSEGV through the normal signal path: a user handler
            // runs (frame rewritten to enter it), the default action kills
            // with 128+SIGSEGV (frame parked). Either way we must NOT return
            // into the faulting instruction unless the handler took over.
            extern int task_fault_signal(int sig, void* frame);
            task_fault_signal(SIGSEGV, r);
            return;
        } else {
            // Kernel crash
            print("\n[KERNEL PANIC] Unhandled Exception: ", 0x0C);
            p_int(r->int_no, 0x0C);
            
            if (r->int_no == 13) {
                print(" (GPF, err=", 0x0C);
                p_int(r->err_code, 0x0C);
                print(")", 0x0C);
            }
            if (r->int_no == 14) {
                uint32_t cr2;
                __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
                {
                    char buf[96];
                    char* p = buf;
                    p = str_append(p, "PF addr: ");
                    p = hex_append(p, cr2);
                    p = str_append(p, "\n");
                    write_serial_try(buf, (int)(p - buf));
                }
                print(" (Page Fault at ", 0x0C);
                p_int(cr2, 0x0C);
                print(")", 0x0C);
            }
            {
                char buf[96];
                char* p = buf;
                p = str_append(p, "EIP: ");
                p = hex_append(p, r->eip);
                p = str_append(p, " CS: ");
                p = hex_append(p, r->cs);
                p = str_append(p, "\n");
                write_serial_try(buf, (int)(p - buf));
            }

            print("\n  EIP=", 0x0C); p_int(r->eip, 0x0C);
            print("  CS=",  0x0C); p_int(r->cs, 0x0C);
            print("\n", 0x0C);
            panic_finish();
        }
    }
}
