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
extern void isr4();
extern void isr13();
extern void isr14();
extern void isr128();
extern void isr_default();
extern void irq0();
extern void irq1();
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
        // Unmask: IRQ0 (timer), IRQ1 (keyboard), IRQ2 (cascade), IRQ12 (mouse)
        outb(0x21, 0xF8);
        outb(0xA1, 0xEF);
    }

    // CPU Exceptions
    idt_set_gate(0,  (uint32_t)isr0,  0x08, 0x8E);  // Division by Zero
    idt_set_gate(4,  (uint32_t)isr4,  0x08, 0xEE);  // Overflow (DPL=3, INTO from Ring 3)
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E);  // General Protection Fault
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E);  // Page Fault

    // Hardware IRQs
    idt_set_gate(32, (uint32_t)irq0,  0x08, 0x8E);  // Timer
    idt_set_gate(33, (uint32_t)irq1,  0x08, 0x8E);  // Keyboard
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);  // Mouse

    // Syscall gate: int 0x80, DPL=3 (Ring 3 can call this)
    idt_set_gate(0x80, (uint32_t)isr128, 0x08, 0xEE);

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
            // The new task_exit() handles full cleanup (WM + VMM)
            task_exit();
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
