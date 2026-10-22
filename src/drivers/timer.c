#include "../include/timer.h"
#include "../include/vga.h"
#include "../include/io.h"
#include "../include/apic.h"
#include "../include/acpi.h"     // acpi_get_hpet_base()
#include "../include/mem.h"      // PAGE_PRESENT/PAGE_RW (the HPET MMIO map)
#include "../include/rtc.h"
#include "../include/serial.h"   // write_serial() (locked — multi-CPU safe)
#include "../include/entropy.h"   // entropy_add() — kernel CSPRNG feed
#include "../include/watchdog.h"  // per-core heartbeat + hard-lockup detector

// ---- HPET (High Precision Event Timer) ----
// Modern high-resolution clock source (10-25 MHz counter vs PIT's 1.19 MHz).
// Used here as a precise monotonic clock + busy-wait, NOT as the scheduler
// tick source: the PIT/HZ=100 tick that drives timer_ticks, the watchdog
// cadence, AP tick insurance and the per-core LAPIC timers is carefully
// tuned and should not be re-based onto HPET routing. HPET's value is
// fine-grained elapsed time for drivers/apps (calibration, delays, profiles).
//
// Register map (Intel HPET spec):
//   +0x000 GCAP_ID : bit15 legacy_route_cap, bits32..63 counter period (fs)
//   +0x010 GEN_CONF: bit0 ENABLE_CNF, bit1 LEG_RT_CNF
//   +0x020 GEN_STAT
//   +0x0F0 MAIN_COUNTER (64-bit)
//   +0x100 timer0 conf/cap, +0x108 timer0 comparator, +0x120 timer0 IRQ route
static uint32_t      hpet_base = 0;
static uint32_t      hpet_fs_per_clk = 0;  // counter period in femtoseconds (32-bit, < 4e9)
static uint32_t      hpet_counter_lo_base = 0;
static uint32_t      hpet_counter_hi_base = 0;
static int hpet_ready = 0;

static inline uint32_t hpet_reg(uint32_t off) {
    return *(volatile uint32_t*)(uintptr_t)(hpet_base + off);
}
static inline void hpet_reg_set(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(uintptr_t)(hpet_base + off) = v;
}
// Read the 64-bit main counter into two 32-bit halves, with rollover
// detection (the real hardware may split the read across two cycles).
static inline void hpet_counter_read(uint32_t* lo, uint32_t* hi) {
    uint32_t h1, l, h2;
    do {
        h1 = hpet_reg(0x0F4);
        l  = hpet_reg(0x0F0);
        h2 = hpet_reg(0x0F4);
    } while (h1 != h2);
    *lo = l; *hi = h2;
}

int hpet_present(void) { return hpet_ready; }

// 32x32 → 64-bit unsigned multiply via x86 `mull` (EDX:EAX = EAX * rm).
static inline uint64_t mul32_32(uint32_t a, uint32_t b) {
    uint32_t lo, hi;
    __asm__("mull %3" : "=a"(lo), "=d"(hi) : "a"(a), "rm"(b));
    return (uint64_t)hi << 32 | lo;
}

// 64-bit / 32-bit unsigned division via x86 `div` (EDX:EAX / rm). Returns
// quotient; remainder is discarded. Does NOT check for `div #DE`: the caller
// must ensure `hi < d` (i.e. the quotient fits in 32 bits).
//
// Operand numbering: 3 operands total — %0 = EAX (lo), %1 = EDX (hi), %2 = d.
// (Earlier this used %3, which resolves past the operand list to garbage and
// silently corrupted every 64-bit HPET conversion — caught by host-side unit
// test on mul32_32/div64_32.)
static inline uint32_t div64_32(uint32_t hi, uint32_t lo, uint32_t d) {
    __asm__("div %2" : "+a"(lo), "+d"(hi) : "r"(d));
    (void)hi;
    return lo;
}

void hpet_init(void) {
    hpet_base = acpi_get_hpet_base();
    if (hpet_base == 0) {
        write_serial_string("[HPET] no HPET table, using PIT\n");
        return;
    }

    // The HPET MMIO page is NOT in the RAM identity map (real platforms put
    // the HPET at 0xFED00000, inside the PCI MMIO window that page_map owns
    // and acpi.c's old 256 MB sanity cap wrongly rejected). Map it
    // identity-style kernel-only (the APIC does exactly this), then verify
    // before the first register read — a page_map that refuses the region
    // leaves base unmapped, and dereferencing THAT is a #PF.
    {
        extern void page_map(uint32_t vaddr, uint32_t paddr, uint32_t flags);
        extern void write_serial_hex(uint32_t);
        page_map(hpet_base, hpet_base, PAGE_PRESENT | PAGE_RW);
        __asm__ __volatile__("invlpg (%0)" : : "r"(hpet_base) : "memory");
        volatile uint32_t* probe = (volatile uint32_t*)(uintptr_t)(hpet_base + 0x004);
        uint32_t per = *probe;   // GCAP_ID bits 32..63 (the period field)
        if (per == 0xFFFFFFFFu) {
            // All-ones on a first read usually means the page did not land
            // (bad mapping) — treat as absent rather than a nonsense period.
            write_serial_string("[HPET] MMIO probe failed, disabling\n");
            hpet_base = 0;
            return;
        }
        write_serial_string("[HPET] MMIO mapped at ");
        write_serial_hex(hpet_base);
        write_serial_string("\n");
    }

    // GCAP_ID bits 32..63 = counter period in femtoseconds (10^-15 s).
    // A period of 0 or an absurd value means the counter is unusable.
    // Read the upper 32 bits of the 64-bit register at offset 0.
    uint32_t gcap_hi = hpet_reg(0x004);   // bits 32..63 of GCAP_ID
    if (gcap_hi == 0 || gcap_hi > 1000000000u) {
        write_serial_string("[HPET] bad counter period, disabling\n");
        hpet_base = 0;
        return;
    }
    hpet_fs_per_clk = gcap_hi;

    // Enable the main counter. If the firmware says legacy routing is
    // supported, also enable legacy route so timer0 can share the PIC IRQ
    // path (harmless when we never program a comparator).
    uint32_t conf = hpet_reg(0x010);
    if (hpet_reg(0x000) & (1u << 15)) conf |= 2u;   // LEG_RT_CNF
    conf |= 1u;                                      // ENABLE_CNF
    hpet_reg_set(0x010, conf);

    hpet_counter_read(&hpet_counter_lo_base, &hpet_counter_hi_base);
    hpet_ready = 1;
    write_serial_string("[HPET] enabled at ");
    write_serial_hex(hpet_base);
    write_serial_string(" period=");
    write_serial_hex(hpet_fs_per_clk);
    write_serial_string(" fs\n");
}

// Monotonic microseconds since hpet_init's counter snapshot. Falls back to
// timer_get_us() (PIT) when no HPET is present.
// Formula: us = (delta_clk * period_fs) / 10^9
uint32_t hpet_get_us(void) {
    if (!hpet_ready) return timer_get_us();

    uint32_t lo, hi;
    hpet_counter_read(&lo, &hi);

    uint32_t dlo = lo - hpet_counter_lo_base;
    uint32_t dhi = hi - hpet_counter_hi_base;
    if (lo < hpet_counter_lo_base) dhi--;  // borrow
    // Now dhi:dlo = counter delta (64-bit, but typically < 2^30 for ~sec-scale).

    // Instead of full 64x64 multiply and 64/32 div (which needs libgcc),
    // fold the high part: each counter wraparound is 2^32 / hpet_fs_per_clk
    // femtoseconds... Actually, simplify: if the delta fits 32 bits (which it
    // does for the first ~170 seconds at 25 MHz on a fresh boot), fall
    // through to the fast path. For the very rare overflow we punt to PIT.
    if (dhi != 0) return timer_get_us();

    // us = dlo * fs_per_clk / 10^9  — use div64_32 for the 64-bit product.
    uint64_t prod = mul32_32(dlo, hpet_fs_per_clk);
    return div64_32((uint32_t)(prod >> 32), (uint32_t)prod, 1000000000u);
}

// Busy-wait for `ns` nanoseconds using the HPET counter. No-op (returns
// immediately) when HPET is unavailable — callers must guard.
void hpet_nsleep(uint32_t ns) {
    if (!hpet_ready) return;

    uint32_t lo, hi;
    hpet_counter_read(&lo, &hi);
    uint64_t start = (uint64_t)hi << 32 | lo;

    // clks_to_wait = ns * 10^6 / fs_per_clk  (1 ns = 10^6 fs, rounded UP so
    // we never sleep less than requested). Both operands fit 32 bits.
    // Rounding up: (numerator + (fs_per_clk - 1)) / fs_per_clk.
    uint64_t num = mul32_32(ns, 1000000u);
    num += hpet_fs_per_clk - 1;
    uint64_t target_clks = div64_32((uint32_t)(num >> 32), (uint32_t)num,
                                    hpet_fs_per_clk);

    uint64_t finish = start + target_clks;
    // 64-bit wrap-safe busy-wait: loop until counter passes finish.
    for (;;) {
        hpet_counter_read(&lo, &hi);
        uint64_t now = (uint64_t)hi << 32 | lo;
        if ((int64_t)(now - finish) >= 0) break;
        __asm__ __volatile__("pause");
    }
}


// volatile: written by the IRQ0 handler, read by wait loops in thread context.
// Without it the compiler may hoist the load out of a polling loop and spin on
// a stale value forever. (syscall.c already declared it extern volatile.)
volatile uint32_t timer_ticks = 0;

// Measured PIT tick rate in ticks/second, calibrated against the CMOS RTC.
// Nominally TIMER_HZ (100); under QEMU TCG the emulated timer can run
// several times faster than wall clock, so a 0.8-second window measured in
// raw ticks actually becomes a fraction of a second of real time —
// double-clicks and similar UI timeouts miss under TCG (and CI, which runs
// the boot tests with TCG). GUIs should scale their time windows by
// ticks_per_sec / 1000. NOTE: since timer_ticks counts milliseconds,
// ticks_per_sec reads ~1000 even at TIMER_HZ=100 (100 IRQs x 10 ms) — it
// measures wall-clock tracking of the ms counter, not the IRQ rate.
volatile uint32_t ticks_per_sec = 1000;

// Calibrate ticks_per_sec by counting PIT ticks across a full RTC second
// boundary. Called once at boot (BSP, single-threaded, interrupts enabled).
// Falls back to 1000 if the RTC is unavailable or the measurement is bogus.
void timer_calibrate_ticks_per_sec(void) {
    rtc_time_t t = rtc_read_time();
    uint32_t sec = t.second;
    // Wait for the next second boundary so the measurement starts on one.
    uint32_t guard = 0;
    while (rtc_read_time().second == sec && guard++ < 100000) { }
    uint32_t t0 = timer_ticks;
    sec = rtc_read_time().second;
    guard = 0;
    while (rtc_read_time().second == sec && guard++ < 100000) { }
    uint32_t t1 = timer_ticks;
    uint32_t rate = t1 - t0;
    // Sanity: a real 1 s window; tolerate 50 Hz .. 100 kHz so a broken RTC
    // (constant seconds) can never produce a nonsense value.
    if (rate >= 50 && rate <= 100000) {
        ticks_per_sec = rate;
        write_serial_string("[TIMER] ticks_per_sec=");
        write_serial_hex(rate);
        write_serial('\n');
    }
}

// Rolling tick-rate calibration, called from the main loop. The QEMU TCG
// virtual clock does not run at a constant multiple of wall time — it can be
// slower at boot and faster once the desktop is busy — so a one-shot boot
// measurement is not enough for GUI timeouts. Re-measuring once per RTC
// second keeps ticks_per_sec tracking the live rate. Cheap: one CMOS read
// per second.
void timer_update_rate_if_second(void) {
    static uint32_t last_sec = 0xFFFFFFFF;
    static uint32_t last_ticks = 0;
    static uint32_t last_check_tick = 0;
    // Gate on tick count BEFORE touching the CMOS: the main loop calls this
    // every iteration, but rtc_read_time() busy-waits on the UIP flag —
    // real port I/O that measurably burns host CPU under TCG when done
    // hundreds of times per second. Two reads per second are plenty for
    // a one-second calibration window (the 500 here is milliseconds).
    if (last_check_tick != 0 && (timer_ticks - last_check_tick) < 500) return;
    last_check_tick = timer_ticks;
    rtc_time_t t = rtc_read_time();
    if (last_sec == 0xFFFFFFFF) {
        last_sec = t.second;
        last_ticks = timer_ticks;
        return;
    }
    int delta = (int)t.second - (int)last_sec;
    if (delta < 0) delta += 60;          // RTC seconds wrap at 60
    if (delta >= 1) {
        if (delta == 1) {                 // only trust exact 1 s windows
            uint32_t rate = timer_ticks - last_ticks;
            if (rate >= 50 && rate <= 100000) ticks_per_sec = rate;
        }
        last_sec = t.second;
        last_ticks = timer_ticks;
    }
}

// Which CPU am I? IRQ0 is broadcast to every core (see ioapic_init), but only
// the BSP owns the global tick counter / GUI heartbeat — the APs only need the
// interrupt to drive schedule().
static inline int get_cid(void) {
    extern uint32_t smp_lapic_addr;
    return smp_lapic_addr ? (apic_get_id() & 15) : 0;
}

static void timer_handler(registers_t* regs) {
    (void)regs;
    int cid = get_cid();

    // v38.64/67 hard-lockup watchdog: every core's vector-32 interrupt (PIT
    // on the BSP, local APIC timer on the APs) bumps its own heartbeat AND
    // tick slots FIRST, before the BSP-only branch. A core stuck with IF=0
    // can never deliver this interrupt, so its heartbeat freezes — that is
    // the signal every other core's detector watches for (see watchdog.c).
    watchdog_tick(cid);

    // v38.67: EVERY core runs the hard-lockup detector, not just the BSP.
    // watchdog_check() self-gates its cadence on this core's own tick count
    // (only doing real work every WD_CHECK_INTERVAL ticks) and watches every
    // peer, BSP included. The mesh is what catches a BSP hang: its own
    // detector dies with it, but the APs keep ticking and declare it HUNG.
    watchdog_check();

    // v38.157: drain the serial transmit ring here, on EVERY core. Writers
    // only append to the ring now (see serial.c); this tick is what moves the
    // bytes to the UART, one non-blocking burst per core per tick. 4 cores x
    // 100 Hz x up to 14 bytes >= 5.6 KB/s of capacity against the ~1.4 KB/s
    // the Q3 diagnostics produce, and no frame ever pays a UART stall again.
    serial_poll();

    // BSP only: the wall clock, heartbeat and GUI updates must not run four
    // times per tick just because IRQ0 is now broadcast to every core.
    if (cid != 0) return;
    // The counter is MILLISECONDS, not IRQ ticks (v38.80): every consumer
    // from Ring 3 timeouts to uptime keeps 1 kHz-era arithmetic unchanged.
    timer_ticks += TIMER_MS_PER_TICK;

    // AP tick insurance (v38.90): KVM's APIC-timer virtualization can lose
    // an AP's LAPIC arming when the AP programs INIT then HLTs immediately
    // (exactly ap_main's shape) — the AP then sleeps with no IRQ source
    // until the watchdog murders the boot (all-HLT NMI dump, no panic).
    // Re-tickle every AP from the BSP's reliable PIT tick with a fixed
    // vector-32 broadcast: each AP runs this same handler (heartbeat +
    // schedule), so a lost LAPIC arming is invisible. Every 2nd tick is
    // plenty (20 ms << 3 s watchdog window) and halves the ICR traffic.
    // Gated past SMP bring-up (no IPI mid-SIPI-handshake) and skipped on
    // single-core/no-LAPIC builds, where the shorthand matches nothing
    // anyway. A genuinely IF=0-hung AP ignores IPIs, so the watchdog still
    // fires correctly for real lockups.
    {
        extern uint32_t smp_cpu_count;
        extern volatile int smp_aps_ready;
        extern void apic_broadcast_fixed(uint8_t vector);
        uint32_t tick_no = timer_ticks / TIMER_MS_PER_TICK;
        if (smp_aps_ready && smp_cpu_count > 1 && (tick_no & 1) == 0)
            apic_broadcast_fixed(32);
    }

    // Feed the kernel entropy pool (v38.52): tick counter + TSC low bits mix
    // in continuously, reseeding the ChaCha8 DRBG every 8 samples (every
    // 80 ms at 100 Hz — still far above any attacker-observable rate).
    entropy_add(timer_ticks);

    // Heartbeat: send '.' every 1000 ms (1 second). write_serial() takes
    // the serial lock, so the dot can never split a log line from another CPU.
    if ((timer_ticks % 1000) == 0) {
        write_serial('.');
    }

    // Per-core load line every 3 s (one locked line, BSP-only): "[LOAD]
    // c0=<pct> c1=<pct> ...". Backed by the Fase 3 scheduler's per-CPU load
    // sampling, so tail -f serial_debug.log shows all four cores at work.
    if ((timer_ticks % 3000) == 0) {
        extern uint32_t task_cpu_load(int);
        extern int task_cpu_count(void);
        int n = task_cpu_count();
        if (n < 1 || n > 4) n = 4;
        write_serial_string("[LOAD] ");
        for (int c = 0; c < n; c++) {
            write_serial_string(c == 0 ? "c0=" : c == 1 ? " c1=" : c == 2 ? " c2=" : " c3=");
            write_serial_hex(task_cpu_load(c));
        }
        /* kbd queue depth high-water mark + drop count: answers whether the
         * desktop's one-pop-per-iteration ever backlogs IRQ-fed bursts. */
        {
            extern void kbd_diag(unsigned *maxdepth, unsigned *drops);
            unsigned kd = 0, kx = 0;
            kbd_diag(&kd, &kx);
            write_serial_string(" kbd=");
            write_serial_hex(kd);
            write_serial_string("/");
            write_serial_hex(kx);
        }
        write_serial_string("\n");
    }
    
    // Drive the 'heartbeat' for UI
    update_marquee();
    update_hud();
}

void init_timer(uint32_t frequency) {
    register_interrupt_handler(32, timer_handler);

    // PIT I/O ports
    // 0x43: Command port
    // 0x40: Channel 0 data port
    uint32_t divisor = 1193180 / frequency;
    pit_divisor = divisor;

    outb(0x43, 0x36); // Square wave mode
    uint8_t l = (uint8_t)(divisor & 0xFF);
    uint8_t h = (uint8_t)((divisor >> 8) & 0xFF);

    outb(0x40, l);
    outb(0x40, h);
}

uint32_t get_ticks() { return timer_ticks; }

uint32_t pit_divisor = 1193;   // programmed divisor; init_timer() sets it.

uint32_t timer_get_us() {
    uint32_t ticks;
    uint16_t count;
    uint32_t eflags;
    
    // Disable interrupts to ensure atomic read of ticks + hardware counter,
    // and restore the caller's IF afterwards — never blindly re-enable.
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    
    // Latch counter 0 (Command 0x00)
    outb(0x43, 0x00);
    uint8_t low = inb(0x40);
    uint8_t high = inb(0x40);
    count = (high << 8) | low;
    
    ticks = timer_ticks;
    
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
    
    // PIT input is 1193180 Hz == 1193 * 1000 + 180: dividing by 1193 keeps
    // every intermediate in 32 bits at the cost of 0.015% (same rounding
    // the 1 kHz code always had). ticks counts ms, so ticks*1000 is us.
    if (count > pit_divisor) count = pit_divisor;
    uint32_t elapsed_us = ((pit_divisor - count) * 1000) / 1193;
    
    return (ticks * 1000) + elapsed_us;
}
