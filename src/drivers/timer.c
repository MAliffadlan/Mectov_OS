#include "../include/timer.h"
#include "../include/vga.h"
#include "../include/io.h"
#include "../include/apic.h"
#include "../include/rtc.h"
#include "../include/serial.h"   // write_serial() (locked — multi-CPU safe)
#include "../include/entropy.h"   // entropy_add() — kernel CSPRNG feed
#include "../include/watchdog.h"  // per-core heartbeat + hard-lockup detector

// volatile: written by the IRQ0 handler, read by wait loops in thread context.
// Without it the compiler may hoist the load out of a polling loop and spin on
// a stale value forever. (syscall.c already declared it extern volatile.)
volatile uint32_t timer_ticks = 0;

// Measured PIT tick rate in ticks/second, calibrated against the CMOS RTC.
// The PIT is programmed for 1000 Hz, but under QEMU TCG the emulated timer can
// run several times faster than wall clock, so an 800-tick "0.8 second" window
// actually becomes a fraction of a second of real time — double-clicks and
// similar UI timeouts miss under TCG (and CI, which runs the boot tests with
// TCG). GUIs should scale their time windows by ticks_per_sec / 1000.
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

    // BSP only: the wall clock, heartbeat and GUI updates must not run four
    // times per tick just because IRQ0 is now broadcast to every core.
    if (cid != 0) return;
    timer_ticks++;

    // Feed the kernel entropy pool (v38.52): tick counter + TSC low bits mix
    // in continuously (1000 Hz), reseeding the ChaCha8 DRBG every 8 samples.
    entropy_add(timer_ticks);

    // Heartbeat: send '.' every 1000 ticks (1 second). write_serial() takes
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

    outb(0x43, 0x36); // Square wave mode
    uint8_t l = (uint8_t)(divisor & 0xFF);
    uint8_t h = (uint8_t)((divisor >> 8) & 0xFF);

    outb(0x40, l);
    outb(0x40, h);
}

uint32_t get_ticks() { return timer_ticks; }

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
    
    // PIT runs at 1193180 Hz. At 1000 Hz, divisor is 1193.
    // Counter counts down from 1193 to 0.
    // Elapsed ticks = 1193 - count.
    // Microseconds elapsed = (1193 - count) * 1000 / 1193.
    uint32_t elapsed_us = ((1193 - count) * 1000) / 1193;
    
    return (ticks * 1000) + elapsed_us;
}
