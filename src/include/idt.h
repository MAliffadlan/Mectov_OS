#ifndef IDT_H
#define IDT_H

#include "types.h"

// Struktur entry IDT (8 byte)
struct idt_entry_struct {
    uint16_t base_lo;             // Alamat handler (bawah)
    uint16_t sel;                 // Kernel segment selector
    uint8_t  always0;             // Selalu 0
    uint8_t  flags;               // Flags (P, DPL, Type)
    uint16_t base_hi;             // Alamat handler (atas)
} __attribute__((packed));

typedef struct idt_entry_struct idt_entry_t;

// Struktur pointer IDT (6 byte)
struct idt_ptr_struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

typedef struct idt_ptr_struct idt_ptr_t;

// Context CPU (Save State)
struct registers {
    uint32_t ds;                                     // Data segment selector
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; // PUSHAD
    uint32_t int_no, err_code;                       // Interrupt number and error code
    uint32_t eip, cs, eflags, useresp, ss;           // Pushed by CPU automatically
};

typedef struct registers registers_t;

// Handler C untuk semua interrupt
typedef void (*isr_t)(registers_t*);

void idt_init();
void register_interrupt_handler(uint8_t n, isr_t handler);

#endif
