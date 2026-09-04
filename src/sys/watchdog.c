// watchdog.c — multi-core hard-lockup detector (v38.64).
//
// The gap this fills: a core that dies with interrupts disabled hangs the
// whole system silently. A spinlock deadlock or an accidental cli+spin on
// one AP leaves the other cores (and CI) waiting forever with no log line —
// the exact failure mode of the KVM fork-GPF and qemu32,+nx stalls this
// project hit in earlier bring-ups. A timer-based heartbeat cannot detect a
// core whose timer interrupt is masked, so detection uses the ONE thing
// that still works against an IF=0 core: the NMI-IPI machinery already
// built for multi-core panic dumps (v38.22).
//
// Design:
//   1. HEARTBEAT — timer.c's vector-32 handler runs on every core (PIT on
//      the BSP, local APIC timer on the APs). Its first act is
//      watchdog_tick(cid), a plain store to wd_heartbeat[cid]. A healthy
//      core bumps its slot ~1000x/s; a core in cli cannot, because its
//      timer IRQ is masked.
//   2. DETECTION — the BSP's timer IRQ calls watchdog_check() every 100
//      ticks. For each AP it remembers the last heartbeat value it saw and
//      the BSP tick when that value last changed. If a core's heartbeat
//      has been frozen for >= WD_TIMEOUT_TICKS of BSP time it is declared
//      hung. A core is only watched after its heartbeat has been seen at
//      least once, so APs still booting (or a dead LAPIC timer) can never
//      false-positive.
//   3. ACTION — wd_hang_detected() prints a [WATCHDOG] marker and calls
//      panic_finish(): the multi-core dump NMI-IPIs every other core — the
//      hung core answers from inside its cli spin and parks, so the log
//      shows exactly where it was stuck — then reboots (panic=reboot) or
//      halts. With `panic=reboot` + QEMU `-no-reboot`, CI gets a clean
//      exit instead of a hang-until-timeout.
//
// The whole module is lock-free by construction (raw stores, try-writes,
// the same discipline as panic.c): the BSP detector runs in IRQ context,
// where taking a lock held by a hung core would deadlock the dump itself.
//
// Self-test (`wd_self_test` boot arg): the BSP sends a directed fixed IPI
// on WD_HANG_VECTOR to the first AP. The AP's handler (registered on all
// cores through the shared IDT) drops into cli+spin; only the watchdog can
// ever find it again. scripts/watchdog_test.py boots with this arg and
// asserts the [WATCHDOG] marker, all four [PANIC] CPU lines and the
// panic=reboot exit.

#include "../include/watchdog.h"
#include "../include/serial.h"
#include "../include/panic.h"
#include "../include/apic.h"
#include "../include/acpi.h"
#include "../include/mem.h"    // memset

// ---- Tunables ----
// Stall threshold in BSP ticks (PIT 1 kHz). Legitimate IF=0 critical
// sections are sub-millisecond (disk DMA waits, lock hold times); nothing
// in the kernel legitimately holds IF=0 for even a fraction of a second of
// BSP time, so 3 s is a hard lockup with enormous margin.
#define WD_TIMEOUT_TICKS     3000
#define WD_MAX_CORES         16

// ---- Per-core heartbeat (written by every core's timer IRQ) ----
static volatile uint32_t wd_heartbeat[WD_MAX_CORES];

// ---- Detector state (BSP only) ----
static uint32_t wd_last_seen[WD_MAX_CORES];     // last heartbeat value seen
static uint32_t wd_stall_since[WD_MAX_CORES];   // BSP tick the stall began
static int      wd_seen[WD_MAX_CORES];          // 1 = core has ticked at least once
static int      wd_fired = 0;                   // one shot

// ---- Boot cmdline state ----
static int wd_self_test_mode = 0;

void watchdog_tick(int cid) {
    if (cid < 0 || cid >= WD_MAX_CORES) cid = 0;
    wd_heartbeat[cid]++;
}

// ---- Minimal try-write formatting (no locks; IRQ context) ----
static char* wd_str(char* p, const char* s) {
    while (*s) *p++ = *s++;
    return p;
}
static char* wd_dec(char* p, uint32_t v) {
    char t[12];
    int i = 0;
    if (v == 0) { *p++ = '0'; return p; }
    while (v > 0) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) *p++ = t[--i];
    return p;
}
static char* wd_hex(char* p, uint32_t v) {
    *p++ = '0'; *p++ = 'x';
    for (int i = 28; i >= 0; i -= 4) {
        int n = (int)((v >> i) & 0xF);
        *p++ = (char)(n < 10 ? '0' + n : 'A' + (n - 10));
    }
    return p;
}

// ---- Boot command line ----
static int wd_has_word(const char* cmd, const char* word) {
    if (!cmd || !word) return 0;
    int wlen = 0;
    while (word[wlen]) wlen++;
    const char* p = cmd;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        int i = 0;
        while (p[i] && p[i] != ' ' && word[i] && p[i] == word[i]) i++;
        if (!word[i] && (p[i] == ' ' || p[i] == '\0')) return 1;
        while (*p && *p != ' ') p++;
    }
    return 0;
}

void watchdog_parse_cmdline(const char* cmd) {
    if (wd_has_word(cmd, "wd_self_test")) wd_self_test_mode = 1;
}

// ---- The hang the watchdog exists to catch ----
// Runs on the target AP of the self-test IPI. The interrupt gate already
// cleared IF on entry; cli makes the intent explicit (and keeps the spin
// IF=0 even if entry conditions ever change). Only an NMI can interrupt
// this loop, so only the watchdog's NMI-IPI dump can record it.
void wd_hang_ipi_handler(registers_t* r) {
    (void)r;
    __asm__ __volatile__("cli");
    for (;;) __asm__ __volatile__("pause");
}

// ---- Detection fired: marker + full multi-core dump + reboot/halt ----
static void wd_hang_detected(int cpu) {
    if (wd_fired) return;
    wd_fired = 1;

    char buf[128];
    char* p = buf;
    p = wd_str(p, "[WATCHDOG] CPU ");
    *p++ = (char)('0' + cpu);
    p = wd_str(p, " HUNG — no timer heartbeat for ");
    p = wd_dec(p, WD_TIMEOUT_TICKS);
    p = wd_str(p, "+ ticks (hard lockup, IF=0) — dumping all cores\n");
    write_serial_try(buf, (int)(p - buf));

    // panic_finish() dumps every core (NMI-IPI reaches the hung one from
    // inside its cli spin), then reboots under `panic=reboot` (CI: QEMU
    // exits with -no-reboot) or halts. Never returns.
    panic_finish();
}

void watchdog_check(void) {
    // BSP only, and only when an APIC exists (SMP off = nothing to watch).
    if (!smp_lapic_addr) return;
    int bsp = apic_get_id() & 15;
    if (bsp < 0 || bsp >= WD_MAX_CORES) bsp = 0;

    uint32_t n = smp_cpu_count;
    if (n > WD_MAX_CORES) n = WD_MAX_CORES;

    extern volatile uint32_t timer_ticks;
    uint32_t now = timer_ticks;

    for (uint32_t c = 0; c < n; c++) {
        if (c == (uint32_t)bsp) continue;
        uint32_t hb = wd_heartbeat[c];

        if (!wd_seen[c]) {
            // Wait until this core has demonstrably ticked before watching
            // it — an AP still booting (or with a dead LAPIC timer) must
            // never look like a hang.
            if (hb == 0) continue;
            wd_seen[c] = 1;
            wd_last_seen[c] = hb;
            wd_stall_since[c] = now;
            continue;
        }

        if (hb != wd_last_seen[c]) {
            wd_last_seen[c] = hb;          // alive: restart the stall clock
            wd_stall_since[c] = now;
        } else if (now - wd_stall_since[c] >= WD_TIMEOUT_TICKS) {
            wd_hang_detected((int)c);
            return;
        }
    }
}

// ---- CI self-test ----
// Booted with `wd_self_test`: hang the first Application Processor once
// the system is up (all APs awake and ticking, the BSP timer live), so the
// detect → NMI-dump → reboot path is exercised end to end. QEMU is
// launched with -no-reboot by scripts/watchdog_test.py, so the panic=reboot
// reset exits QEMU cleanly.
void watchdog_self_test_tick(void) {
    if (!wd_self_test_mode) return;
    wd_self_test_mode = 0;
    if (wd_fired) return;

    if (!smp_lapic_addr || smp_cpu_count <= 1) {
        write_serial_string("[WATCHDOG] self-test skipped (SMP off)\n");
        return;
    }

    // Target: the first APIC id that is not the BSP.
    uint32_t bsp = smp_bsp_lapic_id & 15;
    uint8_t target = 0xFF;
    uint32_t n = smp_cpu_count;
    if (n > WD_MAX_CORES) n = WD_MAX_CORES;
    for (uint32_t i = 0; i < n; i++) {
        if ((smp_lapic_ids[i] & 15) != bsp) { target = smp_lapic_ids[i]; break; }
    }
    if (target == 0xFF) {
        write_serial_string("[WATCHDOG] self-test skipped (no AP found)\n");
        return;
    }

    char buf[128];
    char* p = buf;
    p = wd_str(p, "[WATCHDOG] SELF TEST — hanging CPU ");
    p = wd_dec(p, target & 15);
    p = wd_str(p, " via IPI vector ");
    p = wd_hex(p, WD_HANG_VECTOR);
    p = wd_str(p, " (wd_self_test)\n");
    write_serial_try(buf, (int)(p - buf));

    // Never returns on the target: it drops into wd_hang_ipi_handler's
    // cli spin; the BSP watchdog detects the stall WD_TIMEOUT_TICKS later.
    extern void apic_send_fixed_ipi(uint8_t lapic_id, uint8_t vector);
    apic_send_fixed_ipi(target, WD_HANG_VECTOR);
}
