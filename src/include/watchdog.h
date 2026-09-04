#ifndef WATCHDOG_H
#define WATCHDOG_H

#include "types.h"
#include "idt.h"   // registers_t

// ---- Multi-core hard-lockup watchdog (see watchdog.c) ----
//
// Every core's vector-32 timer interrupt (PIT on the BSP, local APIC timer
// on the APs) bumps a per-core heartbeat counter. A core that dies with
// IF=0 — a spinlock deadlock, an accidental cli + infinite loop — stops
// ticking, because its timer interrupt can never be delivered. The BSP's
// timer IRQ periodically compares every AP's heartbeat against its own
// clock: a heartbeat frozen for WD_TIMEOUT_TICKS of BSP time is a hard
// lockup, and the BSP falls into the existing multi-core panic dump
// (panic.c) — whose NMI-IPI reaches the hung core even with IF=0 and
// snapshots exactly where it was spinning.

// Directed fixed IPI vector used by the wd_self_test boot arg to make one
// AP drop into a cli spin (idt.c wires the gate, interrupt_entry.asm the
// stub; the handler itself lives here).
#define WD_HANG_VECTOR 0x60

// Detector cadence: how many BSP ticks between watchdog_check() calls.
#define WD_CHECK_INTERVAL 100

// Bump THIS core's heartbeat. Called at the top of the vector-32 handler on
// every core, before the BSP-only branch.
void watchdog_tick(int cid);

// Detect stalled cores. Called from the BSP's timer IRQ every 100 ticks;
// no-op on non-BSP cores, when SMP is off, or before every core has been
// seen ticking at least once (never fires during bring-up). On a hit it
// prints a [WATCHDOG] marker and panics (full multi-core dump + reboot
// under `panic=reboot`). Lock-free: only raw stores and try-writes, safe
// to call from IRQ context.
void watchdog_check(void);

// Parse the GRUB command line for `wd_self_test`. Called once early in
// kernel_main next to panic_parse_cmdline().
void watchdog_parse_cmdline(const char* cmd);

// CI self-test: when booted with `wd_self_test`, sends a directed fixed IPI
// that makes the first AP hang with IF=0 (see wd_hang_ipi_handler). The
// BSP's watchdog must then detect the stall, dump all cores (the hung AP
// answers the NMI from inside its spin) and reboot. No-op unless the boot
// arg is present. Called from kernel_main after the timer is live.
void watchdog_self_test_tick(void);

// Runs on the TARGET core of the self-test IPI: cli + spin forever. Never
// returns. Its register snapshot (via the later NMI) is the proof that the
// watchdog can catch a core that no interrupt can reach.
void wd_hang_ipi_handler(registers_t* r);

#endif
