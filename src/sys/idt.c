#include "../include/idt.h"
#include "../include/io.h"
#include "../include/utils.h"
#include "../include/vga.h"

idt_entry_t idt[256];
idt_ptr_t   idt_ptr;
isr_t       interrupt_handlers[256];

// Assembly handler declarations
extern void isr0();
extern void irq0();
extern void irq1();
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

    for (int i = 0; i < 256; i++) interrupt_handlers[i] = 0;

    // Remap the PIC (Programmable Interrupt Controller)
    // Hardware interrupts start at 0x20
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0x0);
    outb(0xA1, 0x0);

    // Set Gates (0x08 = Kernel Code Segment)
    idt_set_gate(32, (uint32_t)irq0, 0x08, 0x8E); // Timer
    idt_set_gate(33, (uint32_t)irq1, 0x08, 0x8E); // Keyboard

    idt_flush((uint32_t)&idt_ptr);
}

void register_interrupt_handler(uint8_t n, isr_t handler) {
    interrupt_handlers[n] = handler;
}

// Global dispatcher called from Assembly
void isr_handler(registers_t regs) {
    if (interrupt_handlers[regs.int_no] != 0) {
        isr_t handler = interrupt_handlers[regs.int_no];
        handler(&regs);
    }
}

void irq_handler(registers_t regs) {
    // Send EOI (End of Interrupt) to PIC
    if (regs.int_no >= 40) outb(0xA0, 0x20);
    outb(0x20, 0x20);

    if (interrupt_handlers[regs.int_no] != 0) {
        isr_t handler = interrupt_handlers[regs.int_no];
        handler(&regs);
    }
}
