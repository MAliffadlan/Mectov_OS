#include "../include/acpi.h"
#include "../include/utils.h"
#include "../include/serial.h"
#include "../include/vmm.h"

uint32_t smp_bsp_lapic_id = 0;
uint32_t smp_cpu_count = 0;
uint8_t smp_lapic_ids[MAX_CORES];
uint32_t smp_lapic_addr = 0;
uint32_t smp_ioapic_addr = 0;
uint32_t smp_pit_gsi = 2; // Default to 2, will override if MADT entry found

static int checksum(const char* addr, int len) {
    uint8_t sum = 0;
    for (int i = 0; i < len; i++) {
        sum += addr[i];
    }
    return sum == 0;
}

static int string_starts_with(const char* str, const char* prefix) {
    while (*prefix) {
        if (*str++ != *prefix++) return 0;
    }
    return 1;
}

static uint16_t read_phys_u16(uintptr_t addr) {
    uint16_t value;
    __asm__ __volatile__("movw (%1), %0" : "=r"(value) : "r"(addr) : "memory");
    return value;
}

// Physical range that is safely identity-mapped by paging_init() (0..256MB).
// ACPI tables always live in low RAM, so any pointer above this is bogus.
#define ACPI_PHYS_MAX (256u * 1024u * 1024u)

static rsdp_t* find_rsdp(void) {
    // 1. Search in EBDA first 1KB
    uint32_t ebda = ((uint32_t)read_phys_u16(0x40E)) << 4;
    if (ebda >= 0x400 && ebda < ACPI_PHYS_MAX) {
        for (uint32_t i = ebda; i < ebda + 1024; i += 16) {
            if (string_starts_with((const char*)i, "RSD PTR ") && checksum((char*)i, 20)) {
                return (rsdp_t*)i;
            }
        }
    }
    
    // Search Main BIOS Area
    for (uint32_t i = 0x000E0000; i < 0x00100000; i += 16) {
        if (string_starts_with((const char*)i, "RSD PTR ") && checksum((char*)i, 20)) {
            return (rsdp_t*)i;
        }
    }
    return NULL;
}

static void parse_madt(madt_t* madt) {
    smp_lapic_addr = madt->local_apic_address;
    write_serial_string("[ACPI] Found MADT. Local APIC at ");
    write_serial_hex(smp_lapic_addr);
    write_serial_string("\n");
    
    uint8_t* ptr = madt->entries;
    uint8_t* end = (uint8_t*)madt + madt->header.length;
    
    smp_cpu_count = 0;
    
    while (ptr < end) {
        madt_entry_header_t* header = (madt_entry_header_t*)ptr;
        if (header->length < sizeof(madt_entry_header_t)) {
            break;
        }
        if (ptr + header->length > end) {
            break;
        }
        
        if (header->type == 0) { // Processor Local APIC
            madt_local_apic_t* lapic = (madt_local_apic_t*)ptr;
            if (lapic->flags & 1) { // Processor Enabled
                if (smp_cpu_count < MAX_CORES) {
                    smp_lapic_ids[smp_cpu_count++] = lapic->apic_id;
                    write_serial_string("[ACPI] Found CPU Core (APIC ID ");
                    write_serial_hex(lapic->apic_id);
                    write_serial_string(")\n");
                }
            }
        } else if (header->type == 1) { // I/O APIC
            madt_io_apic_t* ioapic = (madt_io_apic_t*)ptr;
            smp_ioapic_addr = ioapic->io_apic_address;
            write_serial_string("[ACPI] Found I/O APIC at ");
            write_serial_hex(smp_ioapic_addr);
            write_serial_string("\n");
        } else if (header->type == 2) { // Interrupt Source Override
            madt_iso_t* iso = (madt_iso_t*)ptr;
            if (iso->source == 0) { // IRQ 0 (PIT)
                smp_pit_gsi = iso->global_system_interrupt;
                write_serial_string("[ACPI] PIT (IRQ0) overridden to GSI ");
                write_serial_hex(smp_pit_gsi);
                write_serial_string("\n");
            }
        }
        
        ptr += header->length;
    }
}

// Validate an ACPI table header at a physical address: signature, length and
// checksum. Returns 0 if the table is unusable. The length field comes from
// the header itself, so we bound it to something sane before checksumming.
static int validate_table(acpi_header_t* header, uint32_t max_len) {
    if ((uintptr_t)header < 0x1000 || (uintptr_t)header >= ACPI_PHYS_MAX) {
        return 0;
    }
    uint32_t len = header->length;
    if (len < sizeof(acpi_header_t) || len > max_len) {
        return 0;
    }
    if ((uintptr_t)header + len > ACPI_PHYS_MAX) {
        return 0;
    }
    return checksum((char*)header, (int)len);
}

void acpi_init(void) {
    rsdp_t* rsdp = find_rsdp();
    if (!rsdp) {
        write_serial_string("[ACPI] RSDP not found!\n");
        return;
    }
    
    write_serial_string("[ACPI] Found RSDP at ");
    write_serial_hex((uint32_t)rsdp);
    write_serial_string("\n");

    // ACPI 2.0+ (rev >= 2) prefers the 64-bit XSDT. Fall back to RSDT for
    // legacy firmware. On 32-bit we can only use tables below 4GB, which is
    // always the case for ACPI tables in practice.
    uint64_t xsdt_addr64 = 0;
    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
        xsdt_addr64 = rsdp->xsdt_address;
    }

    if (xsdt_addr64 != 0 && (xsdt_addr64 >> 32) == 0 && (uint32_t)xsdt_addr64 < ACPI_PHYS_MAX) {
        uint32_t addr = (uint32_t)xsdt_addr64;
        write_serial_string("[ACPI] XSDT Addr: ");
        write_serial_hex(addr);
        write_serial_string("\n");

        acpi_header_t* xsdt = (acpi_header_t*)addr;
        if (!validate_table(xsdt, 1024 * 1024)) {
            write_serial_string("[ACPI] XSDT checksum failed!\n");
            return;
        }
        write_serial_string("[ACPI] XSDT sig/len OK\n");

        // XSDT entries are 64-bit pointers (table data follows the 36-byte
        // header; pointer arithmetic is 64-bit-safe here).
        uint32_t len = xsdt->length;
        int entries = (len - sizeof(acpi_header_t)) / 8;
        uint64_t* xptrs = (uint64_t*)((uint8_t*)xsdt + sizeof(acpi_header_t));
        for (int i = 0; i < entries; i++) {
            uint64_t ptr = xptrs[i];
            if ((ptr >> 32) != 0 || ptr == 0 || ptr >= ACPI_PHYS_MAX) continue;
            acpi_header_t* header = (acpi_header_t*)(uint32_t)ptr;
            if (string_starts_with(header->signature, "APIC")) {
                if (validate_table(header, 1024 * 1024)) {
                    parse_madt((madt_t*)header);
                    return;
                }
            }
        }
        write_serial_string("[ACPI] MADT not found in XSDT!\n");
        return;
    }
    
    // ---- Legacy RSDT path (ACPI 1.0) ----
    uint32_t addr = rsdp->rsdt_address;
    write_serial_string("[ACPI] RSDT Addr: ");
    write_serial_hex(addr);
    write_serial_string("\n");

    acpi_header_t* rsdt = (acpi_header_t*)addr;
    if (!validate_table(rsdt, 1024 * 1024)) {
        write_serial_string("[ACPI] RSDT checksum failed!\n");
        return;
    }
    write_serial_string("[ACPI] RSDT sig/len OK\n");

    uint32_t len = rsdt->length;
    int entries = (len - sizeof(acpi_header_t)) / 4;
    uint32_t* rptrs = (uint32_t*)((uint8_t*)rsdt + sizeof(acpi_header_t));
    for (int i = 0; i < entries; i++) {
        uint32_t ptr = rptrs[i];
        if (ptr == 0 || ptr >= ACPI_PHYS_MAX) continue;
        acpi_header_t* header = (acpi_header_t*)ptr;
        if (string_starts_with(header->signature, "APIC")) {
            if (validate_table(header, 1024 * 1024)) {
                parse_madt((madt_t*)header);
                return;
            }
        }
    }
    write_serial_string("[ACPI] MADT not found in RSDT!\n");
}
