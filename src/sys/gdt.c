#include "../include/gdt.h"
#include "../include/utils.h"
#include "../include/apic.h"
#include "../include/serial.h"

gdt_entry_t gdt_entries[16][8];
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

// ---- #DF task gate (32-bit IST substitute) ----
// A kernel stack overflow usually faults while the CPU is mid-push (a call,
// or the exception frame itself) — the stack pointer is already INSIDE the
// unmapped guard page, so the CPU cannot push the #PF/#DF frame there and
// triple-faults (silent reboot) before any handler can run. x86-32 has no
// hardware IST; the classic fix is a TASK GATE on vector 8: the CPU
// hardware-switches to this dedicated TSS (own stack + own CR3 + own EIP),
// never touching the corrupt stack, and runs df_task_handler which prints a
// clean panic and halts. The gate is only entered on a real double fault.
static uint8_t df_stack[4096] __attribute__((aligned(16)));
tss_entry_t df_tss;

static inline int get_cid() { 
    extern uint32_t smp_lapic_addr; 
    return smp_lapic_addr ? (apic_get_id() & 15) : 0; 
}

static void df_hex(char** pp, uint32_t v) {
    char* p = *pp;
    *p++ = '0'; *p++ = 'x';
    for (int i = 28; i >= 0; i -= 4) {
        int n = (int)((v >> i) & 0xF);
        *p++ = (char)(n < 10 ? '0' + n : 'A' + (n - 10));
    }
    *pp = p;
}

// Runs on the dedicated df_tss (own stack + CR3) after a hardware task
// switch, so a corrupt kernel stack cannot prevent it. The CPU saved the
// faulted task's state into the CURRENT TSS (the one TR pointed at, i.e.
// tss[cid]) before loading df_tss — dump EIP/ESP/CR3 from there so serial
// logs say WHERE the double fault happened, not just that one occurred.
static void df_task_handler(void) {
    int cid = get_cid();
    char buf[192];
    char* p = buf;
    const char* pre = "\n[PANIC] DOUBLE FAULT (task gate) - kernel stack corrupt/overflow - halting";
    while (*pre) *p++ = *pre++;
    *p++ = '\n'; *p++ = ' ';
    *p++ = '['; *p++ = 'D'; *p++ = 'F'; *p++ = ']'; *p++ = ' ';
    *p++ = 'c'; *p++ = 'p'; *p++ = 'u'; *p++ = '='; df_hex(&p, (uint32_t)cid);
    *p++ = ' '; *p++ = 'E'; *p++ = 'I'; *p++ = 'P'; *p++ = '='; df_hex(&p, tss[cid].eip);
    *p++ = ' '; *p++ = 'E'; *p++ = 'S'; *p++ = 'P'; *p++ = '='; df_hex(&p, tss[cid].esp);
    *p++ = ' '; *p++ = 'C'; *p++ = 'R'; *p++ = '3'; *p++ = '='; df_hex(&p, tss[cid].cr3);
    *p++ = '\n';
    write_serial_try(buf, (int)(p - buf));
    extern void panic_finish(void);
    panic_finish();
}

extern void gdt_flush(uint32_t);

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
    gdt_ptr[cid].limit = (sizeof(gdt_entry_t) * 8) - 1;
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

    // #DF task-gate TSS (GDT slot 6 = selector 0x30). Set up once by the BSP
    // (cid == 0); every per-CPU GDT still gets a descriptor pointing at the
    // same shared df_tss so the task gate resolves on all cores. CR3 is NOT
    // set here — init_gdt runs before paging_init, so CR3 is still 0; the
    // kernel calls gdt_set_df_cr3() once paging is live.
    if (cid == 0) {
        memset(&df_tss, 0, sizeof(df_tss));
        df_tss.ss0 = 0x10;
        df_tss.esp0 = (uint32_t)(uintptr_t)&df_stack[4096];
        df_tss.esp = (uint32_t)(uintptr_t)&df_stack[4096];
        df_tss.eip = (uint32_t)(uintptr_t)&df_task_handler;
        df_tss.eflags = 0x2;
        df_tss.cs = 0x08;
        df_tss.ss = 0x10;
        df_tss.ds = 0x10;
        df_tss.es = 0x10;
        df_tss.fs = 0x10;
        df_tss.gs = 0x10;
        df_tss.ldt = 0;
        df_tss.iomap_base = sizeof(tss_entry_t);
    }
    gdt_set_gate(cid, 6, (uint32_t)(uintptr_t)&df_tss, sizeof(df_tss) - 1, 0x89, 0x00);

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

// Set the #DF task's page tables once paging is initialized (init_gdt runs
// before paging_init, so CR3 is still 0 there). The task-gate handler needs a
// valid kernel page directory to run on.
void gdt_set_df_cr3(uint32_t cr3) {
    df_tss.cr3 = cr3;
}
