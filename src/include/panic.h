#ifndef PANIC_H
#define PANIC_H

#include "types.h"
#include "idt.h"   // registers_t

// ---- Multi-core panic dump (see panic.c) ----
//
// When any core panics (unhandled kernel exception, double fault, stack
// overflow, heap corruption) the BSP sends an NMI IPI to every other core.
// Each core's NMI handler snapshots its registers into a per-core slot in
// this shared table, then parks (the system is dead — resuming could corrupt
// more state). The panicking core waits briefly for all slots, prints one
// register line per core to the serial console, then either reboots
// (boot arg `panic=reboot`, for CI) or halts.
//
// NMI is used because it cannot be masked by IF: even a core that is parked
// in cli/sti state (holding a spinlock) will still record its registers.

#define PANIC_MAX_CORES 16

typedef struct {
    volatile int valid;      // 1 = this slot has been filled
    uint32_t eip, cs, esp, eflags, cr3;
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp;
} panic_cpu_state_t;

// Parse the GRUB command line for `panic=reboot` and `panic_self_test`.
// Called once early in kernel_main (cmdline is in the identity-mapped low
// memory, so it stays readable before and after paging is enabled).
void panic_parse_cmdline(const char* cmd);

// NMI (vector 2) handler: on a panic, snapshots THIS core's registers into
// its slot and parks. When no panic is active it returns immediately so the
// normal NMI path (GDB, machine check) is untouched.
void panic_nmi_handler(registers_t* r);

// Snapshot this core (best effort — EIP from the caller's return address),
// NMI-IPI every other core, wait for their snapshots and print one register
// line per core to the serial console. Safe to call from any context: uses
// only try-writes and never takes a lock (the panicking core may already
// hold one). Called by panic_finish().
void panic_dump_all_cores(void);

// The unified kernel panic tail: dump all cores, then reboot if
// `panic=reboot` was given on the command line (keyboard-controller reset,
// 8042 — works under QEMU), otherwise halt. Never returns.
void panic_finish(void);

// 1 when booted with `panic=reboot` (used by CI to keep QEMU from hanging).
int panic_reboot_enabled(void);

// CI self-test: when booted with `panic_self_test`, triggers a deliberate
// kernel panic once the desktop loop is running (so all APs are up), so the
// panic dump + reboot path is exercised end to end. Called from the main
// loop; no-op unless the boot arg is present.
void panic_self_test_tick(void);

#endif
