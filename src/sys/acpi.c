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

static rsdp_t* find_rsdp(void) {
    // 1. Search in EBDA first 1KB
    uint32_t ebda = ((uint32_t)read_phys_u16(0x40E)) << 4;
    for (uint32_t i = ebda; i < ebda + 1024; i += 16) {
        if (string_starts_with((const char*)i, "RSD PTR ") && checksum((char*)i, 20)) {
            return (rsdp_t*)i;
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

void acpi_init(void) {
    rsdp_t* rsdp = find_rsdp();
    if (!rsdp) {
        write_serial_string("[ACPI] RSDP not found!\n");
        return;
    }
    
    write_serial_string("[ACPI] Found RSDP at ");
    write_serial_hex((uint32_t)rsdp);
    write_serial_string("\n");
    
    uint32_t addr = rsdp->rsdt_address;
    write_serial_string("[ACPI] RSDT Addr: ");
    write_serial_hex(addr);
    write_serial_string("\n");
    
    // Manually read the memory without casting to struct
    uint32_t* ptr = (uint32_t*)addr;
    uint32_t sig = ptr[0];
    uint32_t len = ptr[1];
    
    write_serial_string("[ACPI] Sig: ");
    write_serial_hex(sig);
    write_serial_string(" Len: ");
    write_serial_hex(len);
    write_serial_string("\n");
    
    rsdt_t* rsdt = (rsdt_t*)addr;
    if (len < sizeof(acpi_header_t) || len > 1024 * 1024) {
        write_serial_string("Invalid len\n");
        return;
    }

    if (!checksum((char*)rsdt, len)) {
        write_serial_string("[ACPI] RSDT checksum failed!\n");
        return;
    }
    
    int entries = (len - sizeof(acpi_header_t)) / 4;
    for (int i = 0; i < entries; i++) {
        acpi_header_t* header = (acpi_header_t*)rsdt->pointers[i];
        if ((uintptr_t)header < addr || (uintptr_t)header + sizeof(acpi_header_t) > addr + len) {
            continue;
        }
        if (string_starts_with(header->signature, "APIC")) {
            parse_madt((madt_t*)header);
            break;
        }
    }
}
