#include "../include/apic.h"
#include "../include/acpi.h"
#include "../include/serial.h"
#include "../include/io.h"
#include "../include/mem.h"

// Write to LAPIC register
static void lapic_write(uint32_t reg, uint32_t value) {
    volatile uint32_t* ptr = (volatile uint32_t*)(smp_lapic_addr + reg);
    *ptr = value;
}

// Read from LAPIC register
static uint32_t lapic_read(uint32_t reg) {
    volatile uint32_t* ptr = (volatile uint32_t*)(smp_lapic_addr + reg);
    return *ptr;
}

void apic_init(void) {
    if (smp_lapic_addr == 0) return;

    // Ensure LAPIC page is mapped (usually 0xFEE00000)
    page_map(smp_lapic_addr, smp_lapic_addr, PAGE_PRESENT | PAGE_RW);
    __asm__ __volatile__("invlpg (%0)" : : "r"(smp_lapic_addr));

    // Enable LAPIC (Spurious Interrupt Vector Register)
    // Set bit 8 to enable, map spurious interrupt to 0xFF
    lapic_write(LAPIC_SIVR, lapic_read(LAPIC_SIVR) | 0x1FF);

    // Set Task Priority Register to 0 (accept all interrupts)
    lapic_write(LAPIC_TPR, 0);

    // Acknowledge any pending interrupts
    lapic_write(LAPIC_EOI, 0);

    write_serial_string("[APIC] Local APIC initialized for CPU ");
    write_serial_hex(apic_get_id());
    write_serial_string("\n");
}

void apic_send_eoi(void) {
    if (smp_lapic_addr) {
        lapic_write(LAPIC_EOI, 0);
    }
}

uint32_t apic_get_id(void) {
    if (smp_lapic_addr == 0) return 0;
    return lapic_read(LAPIC_ID) >> 24;
}

// Write to IOAPIC register
static void ioapic_write(uint8_t reg, uint32_t value) {
    volatile uint32_t* ptr_reg = (volatile uint32_t*)(smp_ioapic_addr + IOAPIC_ID);
    // actually offset 0x10 is data
    
    // IOWIN is at 0x10 offset from IOAPIC base
    volatile uint32_t* ptr_data = (volatile uint32_t*)(smp_ioapic_addr + 0x10);
    
    *ptr_reg = reg;
    *ptr_data = value;
}

// Read from IOAPIC register
static uint32_t ioapic_read(uint8_t reg) {
    volatile uint32_t* ptr_reg = (volatile uint32_t*)(smp_ioapic_addr + IOAPIC_ID);
    volatile uint32_t* ptr_data = (volatile uint32_t*)(smp_ioapic_addr + 0x10);
    
    *ptr_reg = reg;
    return *ptr_data;
}

void ioapic_set_entry(uint8_t index, uint64_t data) {
    uint8_t reg = IOAPIC_REDTBL + (index * 2);
    ioapic_write(reg, (uint32_t)data);
    ioapic_write(reg + 1, (uint32_t)(data >> 32));
}

void ioapic_init(void) {
    if (smp_ioapic_addr == 0) return;

    // Ensure IOAPIC page is mapped
    page_map(smp_ioapic_addr, smp_ioapic_addr, PAGE_PRESENT | PAGE_RW);
    __asm__ __volatile__("invlpg (%0)" : : "r"(smp_ioapic_addr));

    uint32_t ver = ioapic_read(IOAPIC_VER);
    uint32_t max_intr = (ver >> 16) & 0xFF;

    write_serial_string("[IOAPIC] Max interrupts: ");
    write_serial_hex(max_intr);
    write_serial_string("\n");

    // Route IRQ1 (Keyboard) to APIC INT 33
    // Entry format: [Vector (8)] | [Delivery Mode (3)] | [Dest Mode (1)] | [Status (1)] | [Polarity (1)] | [Remote IRR (1)] | [Trigger Mode (1)] | [Mask (1)]
    // Upper 32 bits: [Destination (8)]
    uint64_t entry_kb = 33 | (0 << 8) | (0 << 11) | (0 << 13) | (0 << 15) | (0 << 16);
    entry_kb |= ((uint64_t)smp_bsp_lapic_id) << 56;
    ioapic_set_entry(1, entry_kb);

    // Route IRQ12 (Mouse) to APIC INT 44
    uint64_t entry_mouse = 44 | (0 << 8) | (0 << 11) | (0 << 13) | (0 << 15) | (0 << 16);
    entry_mouse |= ((uint64_t)smp_bsp_lapic_id) << 56;
    ioapic_set_entry(12, entry_mouse);

    // Route IRQ5 (SB16 sound) to APIC INT 37
    uint64_t entry_sb16 = 37 | (0 << 8) | (0 << 11) | (0 << 13) | (0 << 15) | (0 << 16);
    entry_sb16 |= ((uint64_t)smp_bsp_lapic_id) << 56;
    ioapic_set_entry(5, entry_sb16);

    // Route IRQ11 (RTL8139 NIC) to APIC INT 43
    uint64_t entry_net = 43 | (0 << 8) | (0 << 11) | (0 << 13) | (0 << 15) | (0 << 16);
    entry_net |= ((uint64_t)smp_bsp_lapic_id) << 56;
    ioapic_set_entry(11, entry_net);

    // Route Timer to GSI dynamically parsed from MADT (smp_pit_gsi)
    uint64_t entry_pit = 32 | (0 << 8) | (0 << 11) | (0 << 13) | (0 << 15) | (0 << 16);
    entry_pit |= ((uint64_t)smp_bsp_lapic_id) << 56;
    ioapic_set_entry(smp_pit_gsi, entry_pit);
    
    // Mask the rest
    for (uint32_t i = 0; i <= max_intr; i++) {
        if (i != smp_pit_gsi && i != 1 && i != 5 && i != 11 && i != 12) {
            ioapic_set_entry(i, 0x10000); // Set mask bit
        }
    }
}
