#ifndef TIMER_H
#define TIMER_H

#include "types.h"
#include "idt.h"

void init_timer(uint32_t frequency);
uint32_t get_ticks();
uint32_t timer_get_us();
void timer_calibrate_ticks_per_sec(void);
void timer_update_rate_if_second(void);
extern volatile uint32_t ticks_per_sec;

// HPET high-precision clock (probed from ACPI; PIT is the fallback when the
// platform has no HPET). hpet_init() must be called after acpi_init().
void     hpet_init(void);
int      hpet_present(void);
uint32_t hpet_get_us(void);    // monotonic microseconds since init
void     hpet_nsleep(uint32_t ns);  // busy-wait ns (HPET only; no-op if absent)

// System tick rate (v38.80, Linux-HZ style): 100 Hz instead of 1000 Hz.
// timer_ticks counts MILLISECONDS (advanced by TIMER_MS_PER_TICK per IRQ),
// so every ms-based consumer — timeouts, sleeps, uptime, blink rates,
// Ring 3 sys_get_ticks() — keeps working unchanged; only true IRQ-tick
// consumers (watchdog cadence, LAPIC programming, load windows) are
// expressed in ticks and scaled accordingly. 10 ms is also the classic
// Linux HZ=100 timeslice.
#define TIMER_HZ            100
#define TIMER_MS_PER_TICK   (1000 / TIMER_HZ)
extern uint32_t pit_divisor;   // programmed PIT divisor (for timer_get_us)

#endif
