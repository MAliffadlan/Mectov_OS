// panic.c — multi-core register dump on kernel panic + `panic=reboot`.
//
// The old panic paths each printed a single EIP/CS line from the faulting
// core and halted. That told you WHERE one core crashed, but nothing about
// the other three — a deadlock or a 4-core race looks identical. This module
// makes every panic dump all cores:
//
//   1. panic_finish() (the shared tail of every panic path) calls
//      panic_dump_all_cores().
//   2. The BSP snapshots itself, then sends an NMI IPI to all other cores
//      (delivery mode NMI — never masked by IF, so even a core parked while
//      holding a spinlock records its registers).
//   3. Each core's NMI handler (registered on vector 2) writes its registers
//      into a per-core slot and then parks for good.
//   4. The BSP waits (bounded) for the slots, prints one line per core, then
//      reboots via the 8042 when booted with `panic=reboot` (CI-friendly:
//      QEMU sees a reset instead of a hung guest) or halts.
//
// The panic path may already hold any lock (the faulting core could have
// crashed inside spin_lock), so NOTHING here takes a lock: only try-writes
// (write_serial_try) and raw memory stores to the per-core table.

#include "../include/panic.h"
#include "../include/serial.h"
#include "../include/acpi.h"
#include "../include/apic.h"
#include "../include/io.h"
#include "../include/mem.h"   // memset

static panic_cpu_state_t panic_states[PANIC_MAX_CORES];
static volatile int panic_dumping = 0;
static int panic_reboot_mode = 0;
static int panic_self_test_mode = 0;

// ---- Boot command line ----
// cmdline is a NUL-terminated string of space-separated words.
static int has_word(const char* cmd, const char* word) {
    if (!cmd || !word) return 0;
    int wlen = 0;
    while (word[wlen]) wlen++;
    const char* p = cmd;
    while (*p) {
        // skip spaces
        while (*p == ' ') p++;
        if (!*p) break;
        // compare word
        int i = 0;
        while (p[i] && p[i] != ' ' && word[i] && p[i] == word[i]) i++;
        if (!word[i] && (p[i] == ' ' || p[i] == '\0')) return 1;
        // advance to next space
        while (*p && *p != ' ') p++;
    }
    return 0;
}

void panic_parse_cmdline(const char* cmd) {
    if (!cmd) return;
    if (has_word(cmd, "panic=reboot")) panic_reboot_mode = 1;
    if (has_word(cmd, "panic_self_test")) panic_self_test_mode = 1;
}

int panic_reboot_enabled(void) { return panic_reboot_mode; }

// ---- NMI handler ----
// Registered on vector 2 (idt.c). During a panic dump it records this core's
// registers and parks forever; when no dump is in progress it returns so the
// normal NMI path (GDB, machine-check) is unaffected.
void panic_nmi_handler(registers_t* r) {
    if (!panic_dumping) return;

    int cid = smp_lapic_addr ? (apic_get_id() & 15) : 0;
    if (cid < 0 || cid >= PANIC_MAX_CORES) cid = 0;

    panic_states[cid].valid  = 1;
    panic_states[cid].eip    = r->eip;
    panic_states[cid].cs     = r->cs;
    panic_states[cid].esp    = r->useresp ? r->useresp : r->esp;
    panic_states[cid].eflags = r->eflags;
    panic_states[cid].eax    = r->eax;
    panic_states[cid].ebx    = r->ebx;
    panic_states[cid].ecx    = r->ecx;
    panic_states[cid].edx    = r->edx;
    panic_states[cid].esi    = r->esi;
    panic_states[cid].edi    = r->edi;
    panic_states[cid].ebp    = r->ebp;
    uint32_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    panic_states[cid].cr3 = cr3;

    // Park: the system is dead; resuming this core could clobber more state.
    for (;;) __asm__ __volatile__("hlt");
}

// ---- Self snapshot (the core that called panic_finish) ----
// The caller's stack frame is the best approximation we have: EIP comes from
// the return address of panic_finish's caller.
static uint32_t panic_return_eip(void) {
    return (uint32_t)__builtin_return_address(0);
}

// ---- NMI IPI to all OTHER cores ----
// LAPIC ICR: delivery mode 100b (NMI) in bits 8-10, destination shorthand
// 11b (All-Excluding-Self) in bits 18-19. No vector field needed for NMI.
#define LAPIC_ICR_LOW  0x300
#define LAPIC_ICR_HIGH 0x310

static void panic_ipi_all_but_self(void) {
    if (!smp_lapic_addr) return;  // no APIC (single-core fallback)
    volatile uint32_t* icr_high = (volatile uint32_t*)(smp_lapic_addr + LAPIC_ICR_HIGH);
    volatile uint32_t* icr_low  = (volatile uint32_t*)(smp_lapic_addr + LAPIC_ICR_LOW);

    *icr_high = 0;                          // physical mode, no destination field
    *icr_low  = (3u << 18) | (4u << 8);     // all-excluding-self, NMI

    // Wait for delivery to complete.
    while (*icr_low & (1u << 12)) {
        __asm__ __volatile__("pause");
    }
}

// ---- Print helpers (try-write only, no locks) ----
static char* hex_append(char* p, uint32_t v) {
    *p++ = '0'; *p++ = 'x';
    for (int i = 28; i >= 0; i -= 4) {
        int n = (int)((v >> i) & 0xF);
        *p++ = (char)(n < 10 ? '0' + n : 'A' + (n - 10));
    }
    return p;
}
static char* str_append(char* p, const char* s) {
    while (*s) *p++ = *s++;
    return p;
}

void panic_dump_all_cores(void) {
    if (panic_dumping) return;   // re-entrant (e.g. a fault inside the dump)
    panic_dumping = 1;

    int self = smp_lapic_addr ? (apic_get_id() & 15) : 0;
    if (self < 0 || self >= PANIC_MAX_CORES) self = 0;

    // Snapshot self first (EIP from our caller's return address).
    panic_states[self].valid = 1;
    panic_states[self].eip = panic_return_eip();
    panic_states[self].cs = 0x08;   // kernel code segment
    uint32_t esp, eflags, cr3;
    __asm__ __volatile__("mov %%esp, %0" : "=r"(esp));
    __asm__ __volatile__("pushfl; pop %0" : "=r"(eflags));
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    panic_states[self].esp = esp;
    panic_states[self].eflags = eflags;
    panic_states[self].cr3 = cr3;
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp;
    __asm__ __volatile__("mov %%eax, %0" : "=r"(eax));
    __asm__ __volatile__("mov %%ebx, %0" : "=r"(ebx));
    __asm__ __volatile__("mov %%ecx, %0" : "=r"(ecx));
    __asm__ __volatile__("mov %%edx, %0" : "=r"(edx));
    __asm__ __volatile__("mov %%esi, %0" : "=r"(esi));
    __asm__ __volatile__("mov %%edi, %0" : "=r"(edi));
    __asm__ __volatile__("mov %%ebp, %0" : "=r"(ebp));
    panic_states[self].eax = eax;
    panic_states[self].ebx = ebx;
    panic_states[self].ecx = ecx;
    panic_states[self].edx = edx;
    panic_states[self].esi = esi;
    panic_states[self].edi = edi;
    panic_states[self].ebp = ebp;

    // Ask every other core for its registers. They park inside the NMI
    // handler; we never expect them back.
    panic_ipi_all_but_self();

    // Wait (bounded) for the other cores' snapshots to land. They run their
    // NMI handler on their own stack — a few thousand iterations is plenty
    // even under TCG; the bound keeps a dead AP from hanging the panic.
    int waited = 0;
    while (waited < 10000000) {
        int all_in = 1;
        for (int c = 0; c < PANIC_MAX_CORES; c++) {
            if (c == self) continue;
            if (smp_lapic_addr && c < (int)smp_cpu_count && !panic_states[c].valid) {
                all_in = 0;
                break;
            }
        }
        if (all_in) break;
        waited++;
        __asm__ __volatile__("pause");
    }

    // ---- Print one line per core ----
    for (int c = 0; c < PANIC_MAX_CORES; c++) {
        if (!panic_states[c].valid) continue;
        char buf[200];
        char* p = buf;
        p = str_append(p, "[PANIC] CPU ");
        *p++ = (char)('0' + c);
        p = str_append(p, " EIP=");
        p = hex_append(p, panic_states[c].eip);
        p = str_append(p, " CS=");
        p = hex_append(p, panic_states[c].cs);
        p = str_append(p, " ESP=");
        p = hex_append(p, panic_states[c].esp);
        p = str_append(p, " EFL=");
        p = hex_append(p, panic_states[c].eflags);
        p = str_append(p, " EAX=");
        p = hex_append(p, panic_states[c].eax);
        p = str_append(p, " EBX=");
        p = hex_append(p, panic_states[c].ebx);
        p = str_append(p, " ECX=");
        p = hex_append(p, panic_states[c].ecx);
        p = str_append(p, " EDX=");
        p = hex_append(p, panic_states[c].edx);
        p = str_append(p, " CR3=");
        p = hex_append(p, panic_states[c].cr3);
        p = str_append(p, "\n");
        write_serial_try(buf, (int)(p - buf));
    }
}

// ---- Unified panic tail ----
void panic_finish(void) {
    panic_dump_all_cores();

    if (panic_reboot_mode) {
        const char* msg = "[PANIC] panic=reboot — resetting system\n";
        int mlen = 0; while (msg[mlen]) mlen++;
        write_serial_try(msg, mlen);
        // 8042 keyboard-controller reset: the classic x86 reboot. Works under
        // QEMU (guest reset → QEMU exits with -no-reboot, or boots again).
        outb(0x64, 0xFE);
        // Belt and braces: port 0x92 fast A20-style reset.
        outb(0x92, 0x01);
    } else {
        const char* msg = "[PANIC] halted\n";
        int mlen = 0; while (msg[mlen]) mlen++;
        write_serial_try(msg, mlen);
    }
    for (;;) __asm__ __volatile__("hlt");
}

// ---- CI self-test ----
// Booted with `panic_self_test`: fire a deliberate kernel panic from the BSP
// main loop once the system is fully up (all APs running), so the whole
// dump-all-cores + reboot path is exercised end to end. QEMU is launched
// with -no-reboot by scripts/panic_test.py, so the reset exits QEMU cleanly.
void panic_self_test_tick(void) {
    if (!panic_self_test_mode) return;
    panic_self_test_mode = 0;   // fire once
    const char* msg = "[PANIC] SELF TEST — deliberate panic (panic_self_test)\n";
    int mlen = 0; while (msg[mlen]) mlen++;
    write_serial_try(msg, mlen);
    panic_finish();
}
