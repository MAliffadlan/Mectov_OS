#include "../include/idt.h"
#include "../include/io.h"
#include "../include/task.h"
#include "../include/utils.h"

idt_entry_t idt[256];
idt_ptr_t   idt_ptr;
isr_t       interrupt_handlers[256];

// Assembly stubs (from interrupt_entry.asm)
extern void isr0();
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

void idt_init() {
    idt_ptr.limit = sizeof(idt_entry_t) * 256 - 1;
    idt_ptr.base  = (uint32_t)&idt;
    memset(&idt, 0, sizeof(idt_entry_t) * 256);
    for (int i = 0; i < 256; i++) {
        interrupt_handlers[i] = 0;
        idt_set_gate(i, (uint32_t)isr_default, 0x08, 0x8E); // DPL=0 interrupt gate
    }

    // PIC: remap IRQ0-7 → INT 32-39, IRQ8-15 → INT 40-47
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    // Unmask: IRQ0 (timer), IRQ1 (keyboard), IRQ2 (cascade), IRQ12 (mouse)
    outb(0x21, 0xF8);
    outb(0xA1, 0xEF);

    // CPU Exceptions
    idt_set_gate(0,  (uint32_t)isr0,  0x08, 0x8E);  // Division by Zero
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E);  // General Protection Fault
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E);  // Page Fault

    // Hardware IRQs
    idt_set_gate(32, (uint32_t)irq0,  0x08, 0x8E);  // Timer
    idt_set_gate(33, (uint32_t)irq1,  0x08, 0x8E);  // Keyboard
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);  // Mouse

    // Syscall gate: int 0x80, DPL=3 (Ring 3 can call this)
    idt_set_gate(0x80, (uint32_t)isr128, 0x08, 0xEE);

    idt_flush((uint32_t)&idt_ptr);
}

void register_interrupt_handler(uint8_t n, isr_t handler) {
    interrupt_handlers[n] = handler;
}

// IRQ handler — called from irq_common_stub
// Returns (possibly new) ESP for context switching
uint32_t irq_handler(uint32_t esp) {
    registers_t *r = (registers_t*)esp;
    
    // Send EOI to PIC
    if (r->int_no >= 40) outb(0xA0, 0x20);
    outb(0x20, 0x20);
    
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

void isr_handler(registers_t *r) {
    if (interrupt_handlers[r->int_no] != 0) {
        isr_t handler = interrupt_handlers[r->int_no];
        handler(r);
    } else {
        // Unhandled exception — print info and halt
        print("\n[CRASH] Unhandled Exception: ", 0x0C);
        p_int(r->int_no, 0x0C);
        
        if (r->int_no == 13) {
            print(" (General Protection Fault, err=", 0x0C);
            p_int(r->err_code, 0x0C);
            print(")", 0x0C);
        }
        if (r->int_no == 14) {
            uint32_t cr2;
            __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
            print(" (Page Fault at ", 0x0C);
            p_int(cr2, 0x0C);
            print(")", 0x0C);
        }
        print("\n  EIP=", 0x0C); p_int(r->eip, 0x0C);
        print("  CS=",  0x0C); p_int(r->cs, 0x0C);
        print("\n", 0x0C);
        for(;;) __asm__("hlt");
    }
}
