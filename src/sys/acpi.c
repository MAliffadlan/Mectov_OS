#include "../include/acpi.h"
#include "../include/utils.h"
#include "../include/serial.h"
#include "../include/vmm.h"
#include "../include/io.h"    // outw for the S5 PM1a/b control writes

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

// ---- FADT / S5 poweroff state (v38.45) ----
static int      fadt_found = 0;
static uint32_t fadt_pm1a_cnt = 0;
static uint32_t fadt_pm1b_cnt = 0;
static int      s5_typa = 0;   // \_S5 package values (default 0 when absent)
static int      s5_typb = 0;

int acpi_s5_ready(void) { return fadt_found; }

// Scan the DSDT AML for  Name(_S5_, Package(2) { a, b })  in its simplest
// encoded form: 08 '_S5_' 12 pkglen 02 0A a 0A b. Virtually every real
// firmware (SeaBIOS/QEMU included) emits exactly this for the sleep states.
static void parse_s5_from_dsdt(uint32_t dsdt_addr) {
    if (dsdt_addr == 0 || dsdt_addr >= ACPI_PHYS_MAX) return;
    acpi_header_t* d = (acpi_header_t*)dsdt_addr;
    if (!string_starts_with(d->signature, "DSDT")) return;
    if (!validate_table(d, 256 * 1024)) return;
    if (d->length <= sizeof(acpi_header_t)) return;

    uint8_t* aml = (uint8_t*)dsdt_addr + sizeof(acpi_header_t);
    uint32_t len = d->length - sizeof(acpi_header_t);
    for (uint32_t i = 0; i + 12 <= len; i++) {
        if (aml[i] == 0x08 && aml[i+1] == '_' && aml[i+2] == 'S' &&
            aml[i+3] == '5' && aml[i+4] == '_' &&
            aml[i+5] == 0x12 && aml[i+7] == 0x02 &&
            aml[i+8] == 0x0A && aml[i+10] == 0x0A) {
            s5_typa = aml[i+9];
            s5_typb = aml[i+11];
            write_serial_string("[ACPI] \\_S5 SLP_TYPa=");
            write_serial_hex((uint32_t)s5_typa);
            write_serial_string(" SLP_TYPb=");
            write_serial_hex((uint32_t)s5_typb);
            write_serial_string("\n");
            return;
        }
    }
    write_serial_string("[ACPI] no \\_S5 package in DSDT (using SLP_TYP=0)\n");
}

static void acpi_process_fadt(fadt_t* fadt) {
    // A truncated FADT (length < 72) cannot even carry pm1b_cnt_blk.
    if (fadt->header.length < 72) {
        write_serial_string("[ACPI] FADT too short, ignoring\n");
        return;
    }
    fadt_found = 1;
    fadt_pm1a_cnt = fadt->pm1a_cnt_blk;
    fadt_pm1b_cnt = fadt->pm1b_cnt_blk;
    write_serial_string("[ACPI] FADT: PM1a_CNT=0x");
    write_serial_hex(fadt_pm1a_cnt);
    write_serial_string(" PM1b_CNT=0x");
    write_serial_hex(fadt_pm1b_cnt);
    write_serial_string("\n");
    parse_s5_from_dsdt(fadt->dsdt);
}

void acpi_poweroff(void) {
    if (!fadt_found) {
        write_serial_string("[ACPI] poweroff: no FADT, caller must fall back\n");
        return;
    }
    write_serial_string("[ACPI] S5 poweroff (PM1a write)\n");
    // SLP_TYP lives at bits 12:10, SLP_EN at bit 13 of the PM1 control reg.
    if (fadt_pm1a_cnt) outw(fadt_pm1a_cnt, (uint16_t)((s5_typa << 10) | 0x2000));
    if (fadt_pm1b_cnt) outw(fadt_pm1b_cnt, (uint16_t)((s5_typb << 10) | 0x2000));
    // Some firmware needs a moment + retry; if we are still here it did not
    // take (e.g. SMM-guarded). Return so the caller's legacy fallback runs.
    for (int i = 0; i < 100; i++) {
        if (fadt_pm1a_cnt) outw(fadt_pm1a_cnt, (uint16_t)((s5_typa << 10) | 0x2000));
        __asm__ __volatile__("pause");
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

        // v38.45: scan EVERY entry — the MADT (SMP) and the FADT (poweroff)
        // live in the same table list and their relative order varies by
        // firmware, so an early return after the MADT could miss the FADT.
        uint32_t len = xsdt->length;
        int entries = (len - sizeof(acpi_header_t)) / 8;
        uint64_t* xptrs = (uint64_t*)((uint8_t*)xsdt + sizeof(acpi_header_t));
        int madt_done = 0, fadt_done = 0;
        for (int i = 0; i < entries; i++) {
            uint64_t ptr = xptrs[i];
            if ((ptr >> 32) != 0 || ptr == 0 || ptr >= ACPI_PHYS_MAX) continue;
            acpi_header_t* header = (acpi_header_t*)(uint32_t)ptr;
            if (!madt_done && string_starts_with(header->signature, "APIC")) {
                if (validate_table(header, 1024 * 1024)) {
                    parse_madt((madt_t*)header);
                    madt_done = 1;
                }
            } else if (!fadt_done && string_starts_with(header->signature, "FACP")) {
                if (validate_table(header, 1024 * 1024)) {
                    acpi_process_fadt((fadt_t*)header);
                    fadt_done = 1;
                }
            }
        }
        if (!madt_done) write_serial_string("[ACPI] MADT not found in XSDT!\n");
        if (!fadt_done) write_serial_string("[ACPI] FADT not found in XSDT\n");
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
    int madt_done = 0, fadt_done = 0;
    for (int i = 0; i < entries; i++) {
        uint32_t ptr = rptrs[i];
        if (ptr == 0 || ptr >= ACPI_PHYS_MAX) continue;
        acpi_header_t* header = (acpi_header_t*)ptr;
        if (!madt_done && string_starts_with(header->signature, "APIC")) {
            if (validate_table(header, 1024 * 1024)) {
                parse_madt((madt_t*)header);
                madt_done = 1;
            }
        } else if (!fadt_done && string_starts_with(header->signature, "FACP")) {
            if (validate_table(header, 1024 * 1024)) {
                acpi_process_fadt((fadt_t*)header);
                fadt_done = 1;
            }
        }
    }
    if (!madt_done) write_serial_string("[ACPI] MADT not found in RSDT!\n");
    if (!fadt_done) write_serial_string("[ACPI] FADT not found in RSDT\n");
}
