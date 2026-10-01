/* M2 64-bit IDT + PIC + PIT.
 *
 * 16-byte gates (offset_lo/mid/hi + IST). Vectors 0-47 get stubs from
 * k64/entry64.asm; #DF (8) runs on IST1 (no HW task gate in long mode).
 * PIC remapped to 32/40, everything masked except IRQ0; PIT at 100Hz.
 * SMP + IOAPIC + LAPIC timer replace all of this in M6.
 */
#include "cpu64.h"

struct __attribute__((packed)) idt_ent64 {
    u16 off_lo;
    u16 sel;
    u8 ist;
    u8 attr;
    u16 off_mid;
    u32 off_hi;
    u32 zero;
};
_Static_assert(sizeof(struct idt_ent64) == 16, "idt gate must be 16 bytes");

struct __attribute__((packed)) idt_ptr64 {
    u16 limit;
    u64 base;
};

#define IDT_PRESENT_INT 0x8E  /* P=1 DPL=0 interrupt gate */
#define IDT_SYSCALL_GATE 0xEE /* P=1 DPL=3 interrupt gate (Ring-3 int $0x80) */

static struct idt_ent64 idt[256];
void idt64_load(void); /* defined below; AP entry point too */
/* 48 stubs defined in k64/entry64.asm + M4/M6/M7 direct entries. */
extern void *isr64_table[];
extern void isr64_128(void);
extern void isr64_96(void);
extern void isr64_97(void);
extern void isr64_255(void);
extern void isr64_80(void);

static inline void outb(u16 port, u8 v) {
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline void io_wait(void) {
    outb(0x80, 0);
}

static void set_gate(int n, void *addr, u8 ist, u8 attr) {
    u64 a = (u64)addr;
    idt[n].off_lo = (u16)(a & 0xFFFF);
    idt[n].sel = 0x08;
    idt[n].ist = ist;
    idt[n].attr = attr;
    idt[n].off_mid = (u16)((a >> 16) & 0xFFFF);
    idt[n].off_hi = (u32)(a >> 32);
    idt[n].zero = 0;
}

void idt64_init(void) {
    __asm__ __volatile__("cli");
    for (int i = 0; i < 256; i++) {
        idt[i].off_lo = 0;
        idt[i].sel = 0;
        idt[i].ist = 0;
        idt[i].attr = 0;   /* not present */
        idt[i].off_mid = 0;
        idt[i].off_hi = 0;
        idt[i].zero = 0;
    }
    for (int i = 0; i < 48; i++) {
        u8 ist = (i == 8) ? 1 : 0; /* double fault on IST1 stack */
        set_gate(i, isr64_table[i], ist, IDT_PRESENT_INT);
    }
    /* M4 syscall entry: callable from CPL3, no IST (uses TSS.rsp0). */
    set_gate(128, isr64_128, 0, IDT_SYSCALL_GATE);
    /* M6: fixed IPI vectors + LAPIC spurious (SIVR vector 0xFF). */
    set_gate(VEC_IPI_TEST, isr64_96, 0, IDT_PRESENT_INT);
    set_gate(VEC_IPI_TLB, isr64_97, 0, IDT_PRESENT_INT);
    set_gate(VEC_SPURIOUS, isr64_255, 0, IDT_PRESENT_INT);
    /* M7: AP LAPIC timer (DPL0, BSP keeps the PIT on vector 32). */
    set_gate(VEC_AP_TIMER, isr64_80, 0, IDT_PRESENT_INT);
    idt64_load();
}

/* M6: an AP calls this to load the shared IDT (gates installed once by
 * the BSP above). IF=0 throughout AP bring-up. */
void idt64_load(void) {
    struct idt_ptr64 p;
    p.limit = (u16)(sizeof(idt) - 1);
    p.base = (u64)idt;
    __asm__ __volatile__("lidt (%0)" :: "r"(&p) : "memory");
}

void pic_remap_mask_timer_kbd(void) {
    /* ICW1: init + expect ICW4. */
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    /* ICW2: vector offsets 32 (master) / 40 (slave). */
    outb(0x21, 32); io_wait();
    outb(0xA1, 40); io_wait();
    /* ICW3: slave on IRQ2. */
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    /* ICW4: 8086 mode. */
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    /* Mask everything except IRQ0 (PIT), IRQ1 (M7.2 keyboard) and IRQ2 —
     * the cascade line, which M9's mouse needs: IRQ12 arrives through it.
     * Master 0xF8, slave 0xEF (IRQ12 only). */
    outb(0x21, 0xF8);
    outb(0xA1, 0xEF);
}

void pit_init_hz(u32 hz) {
    if (hz == 0) hz = 100;
    u32 div = 1193182u / hz;
    if (div == 0) div = 1;
    if (div > 0xFFFF) div = 0xFFFF;
    outb(0x43, 0x36); /* ch0, lo/hi, mode 3 (square wave) */
    outb(0x40, (u8)(div & 0xFF));
    outb(0x40, (u8)((div >> 8) & 0xFF));
}
