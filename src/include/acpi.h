#ifndef ACPI_H
#define ACPI_H

#include "types.h"

// Root System Description Pointer (ACPI 1.0 + 2.0 extension)
typedef struct {
    char signature[8];
    uint8_t checksum;
    char oemid[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;         // ACPI 2.0+: total RSDP length (36)
    uint64_t xsdt_address;   // ACPI 2.0+: 64-bit XSDT pointer
    uint8_t extended_checksum;
    uint8_t reserved[3];
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

// FADT (Fixed ACPI Description Table) — only the prefix this kernel uses:
// the PM1a/b control-block IO ports and the DSDT pointer (for the \_S5
// sleeping values). Header length is validated to cover through pm1b_cnt.
typedef struct {
    acpi_header_t header;      // 0
    uint32_t firmware_ctrl;    // 36
    uint32_t dsdt;             // 40
    uint8_t  int_model;        // 44
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;          // 46
    uint32_t smi_cmd;          // 48
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;     // 56
    uint32_t pm1b_evt_blk;     // 60
    uint32_t pm1a_cnt_blk;     // 64  <- SLP_TYP/SLP_EN go here to power off
    uint32_t pm1b_cnt_blk;     // 68
} __attribute__((packed)) fadt_t;

#define MAX_CORES 16

extern uint32_t smp_bsp_lapic_id;
extern uint32_t smp_cpu_count;
extern uint8_t smp_lapic_ids[MAX_CORES];
extern uint32_t smp_lapic_addr;
extern uint32_t smp_ioapic_addr;
extern uint32_t smp_pit_gsi;

void acpi_init(void);

// S5 soft-off (v38.45): write the \_S5 sleeping type + SLP_EN into the
// FADT's PM1a/b control blocks. Never returns on success (the machine is
// off); returns only when no usable FADT was found, so the caller can fall
// back to legacy port pokes. Safe to call from anywhere with IO access.
void acpi_poweroff(void);
// 1 when acpi_init located a valid FADT (diagnostics / tests).
int  acpi_s5_ready(void);

#endif
