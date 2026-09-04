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

// Read the PIT channel-0 counter (a 16-bit countdown reloading to 1193 at the
// BSP's 1 kHz rate). The PIT is a global I/O device, so any CPU may read it.
static uint32_t pit_read(void) {
    outb(0x43, 0x00);            // latch channel 0
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint32_t)((hi << 8) | lo);
}

// Shared LAPIC timer rate (counts per 1 ms), measured ONCE on the BSP before
// the APs wake (see lapic_timer_calibrate below). All cores share the same
// bus clock, so one measurement is valid for every LAPIC.
uint32_t lapic_timer_per_ms = 0;

// Measure this core's LAPIC timer rate against the PIT over a 50 ms window.
// Must run on the BSP while the APs are still asleep: the PIT is shared
// hardware, so if three APs calibrate at once they corrupt each other's
// readings and end up with timers firing at wild rates — a timer storm on
// the APs that starves the BSP and makes the whole system flaky.
uint32_t lapic_timer_calibrate(void) {
    if (smp_lapic_addr == 0) return 0;

    // Masked one-shot with a huge count; we measure how far it counts down in
    // a 50 ms window of PIT time.
    lapic_write(LAPIC_LVT_TIMER, 32 | 0x10000);   // vector 32, masked
    lapic_write(LAPIC_TIMER_DIV, 0xB);            // divide by 1
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    // Busy-wait ~50 ms of PIT time: the 16-bit counter counts 0..1193 then
    // wraps, so accumulate each (downward) step, adding a full period on wrap.
    uint32_t c = pit_read();
    int32_t acc = 0;
    while (acc < 59659) {
        uint32_t c2 = pit_read();
        if (c2 != c) {
            if (c2 < c) acc += (int32_t)(c - c2);
            else        acc += (int32_t)(c + (1194 - c2));   // wrapped
            c = c2;
        }
        __asm__ __volatile__("pause");
    }

    uint32_t lapic_elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CUR);
    uint32_t per_ms = lapic_elapsed / 50;         // counts per 1 ms
    if (per_ms == 0 || per_ms > 0x7FFFFF) per_ms = 1000000;  // sanity fallback

    lapic_timer_per_ms = per_ms;
    return per_ms;
}

// Program THIS core's local APIC timer at ~1 kHz using the shared calibrated
// rate. IRQ0 (PIT) is routed to the BSP only, so without this the Application
// Processors would never receive a timer interrupt — tasks parked on their
// runqueues would starve forever. Called from ap_main() before interrupts are
// enabled; the LAPIC timer drives irq_handler -> schedule() on the AP.
void lapic_timer_init(void) {
    if (smp_lapic_addr == 0) return;
    if (lapic_timer_per_ms == 0) lapic_timer_calibrate();  // safety net
    lapic_write(LAPIC_LVT_TIMER, 32 | (1u << 17)); // periodic, vector 32
    lapic_write(LAPIC_TIMER_INIT, lapic_timer_per_ms);
}

void apic_send_eoi(void) {
    if (smp_lapic_addr) {
        lapic_write(LAPIC_EOI, 0);
    }
}

// Directed fixed-delivery IPI (physical destination mode, no shorthand):
// interrupts one specific LAPIC on `vector`. Unlike the NMI delivery used
// by the panic path this is a normal interrupt — the target must have IF=1
// to take it, and its own interrupt gate clears IF on entry. Used by the
// watchdog self-test (watchdog.c) to make one AP drop into a cli spin.
void apic_send_fixed_ipi(uint8_t lapic_id, uint8_t vector) {
    if (!smp_lapic_addr) return;
    volatile uint32_t* icr_high = (volatile uint32_t*)(smp_lapic_addr + LAPIC_ICR_HIGH);
    volatile uint32_t* icr_low  = (volatile uint32_t*)(smp_lapic_addr + LAPIC_ICR_LOW);

    *icr_high = ((uint32_t)lapic_id << 24);   // physical destination
    *icr_low  = vector;                        // fixed delivery (000), no shorthand

    // Wait for delivery to complete.
    while (*icr_low & (1u << 12)) {
        __asm__ __volatile__("pause");
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

    // Route IRQ14 (IDE primary, BMIDE DMA) to APIC INT 46 and IRQ15 (IDE
    // secondary) to INT 47. Without these the BMIDE controller's completion
    // interrupt never reaches a LAPIC and the IRQ stays latched.
    uint64_t entry_ide0 = 46 | (0 << 8) | (0 << 11) | (0 << 13) | (0 << 15) | (0 << 16);
    entry_ide0 |= ((uint64_t)smp_bsp_lapic_id) << 56;
    ioapic_set_entry(14, entry_ide0);
    uint64_t entry_ide1 = 47 | (0 << 8) | (0 << 11) | (0 << 13) | (0 << 15) | (0 << 16);
    entry_ide1 |= ((uint64_t)smp_bsp_lapic_id) << 56;
    ioapic_set_entry(15, entry_ide1);

    // Route Timer to GSI dynamically parsed from MADT (smp_pit_gsi). The PIT
    // tick stays BSP-only: Application Processors get their own periodic
    // tick from the local APIC timer (see lapic_timer_init() in ap_main), so
    // the per-CPU scheduler runs on every core.
    uint64_t entry_pit = 32 | (0 << 8) | (0 << 11) | (0 << 13) | (0 << 15) | (0 << 16);
    entry_pit |= ((uint64_t)smp_bsp_lapic_id) << 56;
    ioapic_set_entry(smp_pit_gsi, entry_pit);
    
    // Mask the rest
    for (uint32_t i = 0; i <= max_intr; i++) {
        if (i != smp_pit_gsi && i != 1 && i != 5 && i != 11 && i != 12 &&
            i != 14 && i != 15) {
            ioapic_set_entry(i, 0x10000); // Set mask bit
        }
    }
}
