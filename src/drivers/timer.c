#include "../include/timer.h"
#include "../include/vga.h"
#include "../include/io.h"
#include "../include/apic.h"
#include "../include/serial.h"   // write_serial() (locked — multi-CPU safe)

// volatile: written by the IRQ0 handler, read by wait loops in thread context.
// Without it the compiler may hoist the load out of a polling loop and spin on
// a stale value forever. (syscall.c already declared it extern volatile.)
volatile uint32_t timer_ticks = 0;

// Which CPU am I? IRQ0 is broadcast to every core (see ioapic_init), but only
// the BSP owns the global tick counter / GUI heartbeat — the APs only need the
// interrupt to drive schedule().
static inline int get_cid(void) {
    extern uint32_t smp_lapic_addr;
    return smp_lapic_addr ? (apic_get_id() & 15) : 0;
}

static void timer_handler(registers_t* regs) {
    (void)regs;
    // BSP only: the wall clock, heartbeat and GUI updates must not run four
    // times per tick just because IRQ0 is now broadcast to every core.
    if (get_cid() != 0) return;
    timer_ticks++;
    
    // Heartbeat: send '.' every 1000 ticks (1 second). write_serial() takes
    // the serial lock, so the dot can never split a log line from another CPU.
    if ((timer_ticks % 1000) == 0) {
        write_serial('.');
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
