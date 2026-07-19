#ifndef ACPI_H
#define ACPI_H

#include "types.h"

// Root System Description Pointer
typedef struct {
    char signature[8];
    uint8_t checksum;
    char oemid[6];
    uint8_t revision;
    uint32_t rsdt_address;
} __attribute__((packed)) rsdp_t;

// Standard ACPI Table Header
typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oemid[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_header_t;

// Root System Description Table
typedef struct {
    acpi_header_t header;
    uint32_t pointers[]; // variable length
} __attribute__((packed)) rsdt_t;

// MADT (Multiple APIC Description Table)
typedef struct {
    acpi_header_t header;
    uint32_t local_apic_address;
    uint32_t flags;
    uint8_t entries[]; // variable length
} __attribute__((packed)) madt_t;

// MADT Entry Header
typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) madt_entry_header_t;

// Type 0: Processor Local APIC
typedef struct {
    madt_entry_header_t header;
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed)) madt_local_apic_t;

// Type 1: I/O APIC
typedef struct {
    madt_entry_header_t header;
    uint8_t io_apic_id;
    uint8_t reserved;
    uint32_t io_apic_address;
    uint32_t global_system_interrupt_base;
} __attribute__((packed)) madt_io_apic_t;

// Type 2: Interrupt Source Override
typedef struct {
    madt_entry_header_t header;
    uint8_t bus;
    uint8_t source;
    uint32_t global_system_interrupt;
    uint16_t flags;
} __attribute__((packed)) madt_iso_t;

#define MAX_CORES 16

extern uint32_t smp_bsp_lapic_id;
extern uint32_t smp_cpu_count;
extern uint8_t smp_lapic_ids[MAX_CORES];
extern uint32_t smp_lapic_addr;
extern uint32_t smp_ioapic_addr;
extern uint32_t smp_pit_gsi;

void acpi_init(void);

#endif
