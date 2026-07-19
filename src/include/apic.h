#ifndef APIC_H
#define APIC_H

#include "types.h"

// LAPIC Registers (Offsets from smp_lapic_addr)
#define LAPIC_ID            0x020
#define LAPIC_VER           0x030
#define LAPIC_TPR           0x080
#define LAPIC_EOI           0x0B0
#define LAPIC_LDR           0x0D0
#define LAPIC_DFR           0x0E0
#define LAPIC_SIVR          0x0F0
#define LAPIC_ISR           0x100
#define LAPIC_TMR           0x180
#define LAPIC_IRR           0x200
#define LAPIC_ESR           0x280
#define LAPIC_ICR_LOW       0x300
#define LAPIC_ICR_HIGH      0x310
#define LAPIC_LVT_TIMER     0x320
#define LAPIC_LVT_THERMAL   0x330
#define LAPIC_LVT_PERF      0x340
#define LAPIC_LVT_LINT0     0x350
#define LAPIC_LVT_LINT1     0x360
#define LAPIC_LVT_ERR       0x370
#define LAPIC_TIMER_INIT    0x380
#define LAPIC_TIMER_CUR     0x390
#define LAPIC_TIMER_DIV     0x3E0

// IOAPIC Registers
#define IOAPIC_ID           0x00
#define IOAPIC_VER          0x01
#define IOAPIC_ARB          0x02
#define IOAPIC_REDTBL       0x10

void apic_init(void);
void apic_send_eoi(void);
uint32_t apic_get_id(void);
void ioapic_init(void);
void ioapic_set_entry(uint8_t index, uint64_t data);

#endif
