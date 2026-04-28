#include "../include/timer.h"
#include "../include/vga.h"
#include "../include/io.h"

uint32_t timer_ticks = 0;

// Direct serial write (no function call overhead, no blocking risk)
static inline void serial_char(char c) {
    // Quick poll (limited retries to avoid blocking)
    for (int i = 0; i < 10000; i++) {
        if (inb(0x3F8 + 5) & 0x20) break;
    }
    outb(0x3F8, c);
}

static void timer_handler(registers_t* regs) {
    (void)regs;
    timer_ticks++;
    
    // Heartbeat: send '.' every 1000 ticks (1 second)
    if ((timer_ticks % 1000) == 0) {
        serial_char('.');
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
    
    // Disable interrupts to ensure atomic read of ticks + hardware counter
    __asm__ __volatile__("cli");
    
    // Latch counter 0 (Command 0x00)
    outb(0x43, 0x00);
    uint8_t low = inb(0x40);
    uint8_t high = inb(0x40);
    count = (high << 8) | low;
    
    ticks = timer_ticks;
    
    __asm__ __volatile__("sti");
    
    // PIT runs at 1193180 Hz. At 1000 Hz, divisor is 1193.
    // Counter counts down from 1193 to 0.
    // Elapsed ticks = 1193 - count.
    // Microseconds elapsed = (1193 - count) * 1000 / 1193.
    uint32_t elapsed_us = ((1193 - count) * 1000) / 1193;
    
    return (ticks * 1000) + elapsed_us;
}
