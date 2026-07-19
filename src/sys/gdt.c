#include "../include/gdt.h"
#include "../include/utils.h"
#include "../include/apic.h"
#include "../include/serial.h"

gdt_entry_t gdt_entries[16][6];
gdt_ptr_t   gdt_ptr[16];

// TSS structure
typedef struct {
    uint32_t prev_tss;
    uint32_t esp0;  // Kernel stack pointer
    uint32_t ss0;   // Kernel stack segment
    uint32_t esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed)) tss_entry_t;

tss_entry_t tss[16];
uint32_t user_stack[1024]; // 4KB user stack

extern void gdt_flush(uint32_t);

static inline int get_cid() { 
    extern uint32_t smp_lapic_addr; 
    return smp_lapic_addr ? (apic_get_id() & 15) : 0; 
}

static void gdt_set_gate(int cid, int32_t num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt_entries[cid][num].base_low    = (base & 0xFFFF);
    gdt_entries[cid][num].base_middle = (base >> 16) & 0xFF;
    gdt_entries[cid][num].base_high   = (base >> 24) & 0xFF;
    gdt_entries[cid][num].limit_low   = (limit & 0xFFFF);
    gdt_entries[cid][num].granularity = (limit >> 16) & 0x0F;
    gdt_entries[cid][num].granularity |= gran & 0xF0;
    gdt_entries[cid][num].access      = access;
}

void init_gdt() {
    int cid = get_cid();
    gdt_ptr[cid].limit = (sizeof(gdt_entry_t) * 6) - 1;
    gdt_ptr[cid].base  = (uint32_t)&gdt_entries[cid];

    gdt_set_gate(cid, 0, 0, 0, 0, 0);                // 0x00: Null segment
    gdt_set_gate(cid, 1, 0, 0xFFFFFFFF, 0x9A, 0xCF); // 0x08: Kernel Code (Ring 0)
    gdt_set_gate(cid, 2, 0, 0xFFFFFFFF, 0x92, 0xCF); // 0x10: Kernel Data (Ring 0)
    gdt_set_gate(cid, 3, 0, 0xFFFFFFFF, 0xFA, 0xCF); // 0x18: User Code   (Ring 3)
    gdt_set_gate(cid, 4, 0, 0xFFFFFFFF, 0xF2, 0xCF); // 0x20: User Data   (Ring 3)

    // TSS entry (GDT slot 5 = selector 0x28)
    uint32_t tss_base = (uint32_t)&tss[cid];
    uint32_t tss_limit = sizeof(tss_entry_t) - 1;
    memset(&tss[cid], 0, sizeof(tss_entry_t));
    tss[cid].ss0 = 0x10;  // Kernel data segment
    tss[cid].esp0 = 0;    // Will be set before switching to user mode
    tss[cid].iomap_base = sizeof(tss_entry_t);
    gdt_set_gate(cid, 5, tss_base, tss_limit, 0x89, 0x00); // TSS descriptor

    gdt_flush((uint32_t)&gdt_ptr[cid]);

    // Load TSS (must use RPL=0 selector for LTR, which is 0x28)
    __asm__ volatile("mov $0x28, %%ax; ltr %%ax" ::: "ax");
    
    write_serial_string("[GDT] Initialized for CPU ");
    write_serial_hex(cid);
    write_serial_string("\n");
}

void tss_set_kernel_stack(uint32_t stack) {
    int cid = get_cid();
    tss[cid].esp0 = stack;
}
