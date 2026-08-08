#include "../include/idt.h"
#include "../include/io.h"
#include "../include/task.h"
#include "../include/utils.h"
#include "../include/serial.h"

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
extern void idt_flush(uint32_t);

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

void idt_init() {
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
        // IRQ11 (RTL8139 NIC), IRQ12 (mouse)
        // 0xD8 = 1101_1000: unmask bits 0,1,2,5 (IRQ0 timer, IRQ1 kb, IRQ2
        // cascade, IRQ5 SB16); mask the rest. (0xE8 previously masked IRQ5
        // and the cascade, silently killing SB16 + mouse on legacy-PIC boots.)
        outb(0x21, 0xD8);
        outb(0xA1, 0xEF ^ (1 << 3)); // slave: unmask IRQ11 (bit 3 = 0)
    }

    // CPU Exceptions. Every real fault vector gets a dedicated stub so the
    // frame is never misaligned by isr_default's dummy error code (see the
    // ISR_WITH_ERR/ISR_NO_ERR macros in interrupt_entry.asm): a CPU-pushed
    // error code shifted registers_t by 4 bytes, so an unregistered fault
    // (e.g. #DF/#TS/#SS) silently triple-faulted instead of panicking with
    // a real int_no. All are DPL=0 interrupt gates so Ring 3 cannot invoke
    // them directly.
    idt_set_gate(0,  (uint32_t)isr0,  0x08, 0x8E);  // #DE Division by Zero
    idt_set_gate(1,  (uint32_t)isr1,  0x08, 0xEE);  // #DB Debug/single-step (DPL=3 for GDB)
    idt_set_gate(2,  (uint32_t)isr2,  0x08, 0x8E);  // NMI
    idt_set_gate(3,  (uint32_t)isr3,  0x08, 0xEE);  // #BP Breakpoint (DPL=3 for GDB)
    idt_set_gate(4,  (uint32_t)isr4,  0x08, 0xEE);  // #OF Overflow (DPL=3, INTO from Ring 3)
    idt_set_gate(5,  (uint32_t)isr5,  0x08, 0x8E);  // #BR Bound Range Exceeded
    idt_set_gate(6,  (uint32_t)isr6,  0x08, 0x8E);  // #UD Invalid Opcode
    idt_set_gate(7,  (uint32_t)isr7,  0x08, 0x8E);  // #NM Device Not Available
    idt_set_gate(8,  (uint32_t)isr8,  0x08, 0x8E);  // #DF Double Fault
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
        uint32_t eip = r->eip, cs = r->cs;
        write_serial_string("\n[PANIC] DOUBLE FAULT (int 8) at EIP=");
        write_serial_hex(eip);
        write_serial_string(" CS=");
        write_serial_hex(cs);
        write_serial_string(" — kernel stack corruption, halting\n");
        print("\n[KERNEL PANIC] Double Fault — kernel stack corrupted\n", 0x0C);
        for(;;) __asm__("hlt");
    }

    if (r->int_no == 14) {
        uint32_t faulting_address;
        __asm__ __volatile__("mov %%cr2, %0" : "=r"(faulting_address));
        
        uint32_t cr3_val;
        __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3_val));
        
        uint32_t* pd = (uint32_t*)(uintptr_t)(cr3_val & 0xFFFFF000);
        uint32_t pd_idx = faulting_address >> 22;
        
        if (pd[pd_idx] & PAGE_PRESENT) {
            uint32_t* pt = (uint32_t*)(uintptr_t)(pd[pd_idx] & 0xFFFFF000);
            uint32_t pt_idx = (faulting_address >> 12) & 0x3FF;
            
            if (pt[pt_idx] & PAGE_PRESENT) {
                uint32_t entry = pt[pt_idx];
                if (entry & PAGE_COW) {
                    uint32_t old_paddr = entry & 0xFFFFF000;
                    
                    // Sole ownership optimization
                    if (frame_ref_count[old_paddr / 4096] == 1) {
                        uint32_t flags = entry & 0xFFF;
                        flags |= PAGE_RW;
                        flags &= ~PAGE_COW;
                        pt[pt_idx] = old_paddr | flags;
                        __asm__ __volatile__("invlpg (%0)" : : "r"(faulting_address));
                        
                        write_serial_string("[COW] Promoted sole-owned page to writable at ");
                        write_serial_hex(faulting_address);
                        write_serial_string("\n");
                        return; // Resume execution
                    }
                    
                    // Duplicate page
                    uint32_t new_paddr = frame_alloc();
                    if (new_paddr == 0) {
                        write_serial_string("[COW] OOM during COW fault at ");
                        write_serial_hex(faulting_address);
                        write_serial_string(" — falling through to kill task\n");
                        // Falls through to the unhandled-exception path below,
                        // which calls task_exit() for Ring 3 or panics for Ring 0.
                    } else {
                        memcpy((void*)(uintptr_t)new_paddr, (void*)(uintptr_t)old_paddr, 4096);
                        
                        uint32_t flags = entry & 0xFFF;
                        flags |= PAGE_RW;
                        flags &= ~PAGE_COW;
                        pt[pt_idx] = new_paddr | flags;
                        
                        frame_free(old_paddr);
                        __asm__ __volatile__("invlpg (%0)" : : "r"(faulting_address));
                        
                        write_serial_string("[COW] Duplicated page at ");
                        write_serial_hex(faulting_address);
                        write_serial_string(" (Old: ");
                        write_serial_hex(old_paddr);
                        write_serial_string(" -> New: ");
                        write_serial_hex(new_paddr);
                        write_serial_string(")\n");
                        return; // Resume execution
                    }
                }
            }
        }
        // --- Demand Paging for mmap() regions ---
        // Reserved mmap ranges live at 0x40000000..0x80000000 with no frames;
        // lazily map a zeroed frame per page on first access.
        if ((r->cs & 3) == 3 && faulting_address >= MMAP_BASE && faulting_address < MMAP_END) {
            extern int task_mmap_handle_fault(uint32_t addr, uint32_t cr3);
            if (task_mmap_handle_fault(faulting_address, cr3_val)) {
                return; // Resume execution, instruction will restart
            }
        }
        // --- NEW: Demand Paging for Ring 3 Heap ---
        if ((r->cs & 3) == 3 && faulting_address >= 0x08000000 && faulting_address < 0x20000000) {
            extern int get_current_task(void);
            extern uint32_t task_get_heap_ptr(int tid);
            int tid = get_current_task();
            uint32_t max_heap = task_get_heap_ptr(tid);
            
            if (faulting_address < max_heap) {
                // Address is within the reserved heap space! Map it.
                uint32_t phys = frame_alloc();
                if (phys) {
                    vmm_map_page(cr3_val, faulting_address, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
                    // Clear the page to 0 (important for security & determinism)
                    memset((void*)(faulting_address & 0xFFFFF000), 0, 4096);
                    
                    write_serial_string("[VMM] Demand Paged addr ");
                    write_serial_hex(faulting_address);
                    write_serial_string(" for TID ");
                    write_serial_hex(tid);
                    write_serial_string("\n");
                    return; // Resume execution, instruction will restart
                } else {
                    write_serial_string("[VMM] OUT OF MEMORY during Demand Paging!\n");
                }
            }
        }
    }

    if (interrupt_handlers[r->int_no] != 0) {
        isr_t handler = interrupt_handlers[r->int_no];
        handler(r);
    } else {
        // Unhandled exception
        write_serial_string("\n[EXCEPTION] int_no=");
        write_serial_hex(r->int_no);
        write_serial_string(" CS=");
        write_serial_hex(r->cs);
        write_serial('\n');
        
        uint32_t cs = r->cs;
        if ((cs & 3) == 3) {
            // Ring 3 crash - clean up windows, then kill the task
            write_serial_string("[CRASH] Ring 3 crash, killing task\n");
            extern int get_current_task(void);
            extern void wm_cleanup_task(int tid);
            int crashed_tid = get_current_task();
            write_serial_string("[CRASH] Task ID: ");
            write_serial_hex(crashed_tid);
            write_serial('\n');
            if (r->int_no == 14) {
                uint32_t cr2;
                __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
                write_serial_string("PF addr: ");
                write_serial_hex(cr2);
                write_serial_string(" err=");
                write_serial_hex(r->err_code);
                write_serial_string("\n");
            }
            write_serial_string("EIP: ");
            write_serial_hex(r->eip);
            write_serial_string(" ESP: ");
            write_serial_hex(r->useresp);
            write_serial_string(" EBP: ");
            write_serial_hex(r->ebp);
            write_serial_string("\n");
            write_serial_string("EAX: ");
            write_serial_hex(r->eax);
            write_serial_string(" EBX: ");
            write_serial_hex(r->ebx);
            write_serial_string(" ECX: ");
            write_serial_hex(r->ecx);
            write_serial_string(" EDX: ");
            write_serial_hex(r->edx);
            write_serial_string("\n");
            uint32_t cr3_val;
            __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3_val));
            write_serial_string("CR3: ");
            write_serial_hex(cr3_val);
            write_serial_string("\n");
            // The new task_exit_with_code() handles full cleanup (WM + VMM)
            // and records a SIGSEGV-style exit status for the parent.
            extern void task_exit_with_code(int code);
            task_exit_with_code(128 + SIGSEGV);
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
                write_serial_string("PF addr: ");
                write_serial_hex(cr2);
                write_serial_string("\n");
                print(" (Page Fault at ", 0x0C);
                p_int(cr2, 0x0C);
                print(")", 0x0C);
            }
            write_serial_string("EIP: ");
            write_serial_hex(r->eip);
            write_serial_string(" CS: ");
            write_serial_hex(r->cs);
            write_serial_string("\n");

            print("\n  EIP=", 0x0C); p_int(r->eip, 0x0C);
            print("  CS=",  0x0C); p_int(r->cs, 0x0C);
            print("\n", 0x0C);
            for(;;) __asm__("hlt");
        }
    }
}
