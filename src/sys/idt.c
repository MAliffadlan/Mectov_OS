#include "../include/idt.h"
#include "../include/io.h"
#include "../include/task.h"
#include "../include/utils.h"

idt_entry_t idt[256];
idt_ptr_t   idt_ptr;
isr_t       interrupt_handlers[256];
extern void isr0();
extern void irq0();
extern void irq1();
extern void irq12();
extern void idt_flush(uint32_t);
extern void isr_default();

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
    memset(&idt, 0, sizeof(idt_entry_t) * 256); // ZERO OUT IDT!
    for (int i = 0; i < 256; i++) {
        interrupt_handlers[i] = 0;
        idt_set_gate(i, (uint32_t)isr_default, 0x08, 0x8E);
    }

    // PIC: remap IRQ0-7 → INT 32-39, IRQ8-15 → INT 40-47
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    // Master PIC: 0xF8 = 1111 1000
    //   unmask IRQ0 (timer=bit0), IRQ1 (kbd=bit1), IRQ2 (cascade to slave=bit2)
    // Slave PIC: 0xEF = 1110 1111
    //   unmask IRQ12 (mouse = IRQ4 on slave = bit4)
    outb(0x21, 0xF8);
    outb(0xA1, 0xEF);

    idt_set_gate(32, (uint32_t)irq0,  0x08, 0x8E);
    idt_set_gate(33, (uint32_t)irq1,  0x08, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);

    idt_flush((uint32_t)&idt_ptr);
}

void register_interrupt_handler(uint8_t n, isr_t handler) {
    interrupt_handlers[n] = handler;
}

uint32_t irq_handler(uint32_t esp) {
    registers_t *r = (registers_t*)esp;
    if (r->int_no >= 40) outb(0xA0, 0x20);
    outb(0x20, 0x20);
    if (interrupt_handlers[r->int_no] != 0) {
        isr_t handler = interrupt_handlers[r->int_no];
        handler(r);
    }
    
    // Call scheduler if it's the timer interrupt
    if (r->int_no == 32) {
        return schedule(esp);
    }
    return esp;
}

void isr_handler(registers_t *r) {
    if (interrupt_handlers[r->int_no] != 0) {
        isr_t handler = interrupt_handlers[r->int_no];
        handler(r);
    } else {
        for(;;) __asm__("hlt");
    }
}
