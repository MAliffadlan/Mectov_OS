#include "../include/timer.h"
#include "../include/vga.h"
#include "../include/io.h"

uint32_t timer_ticks = 0;

static void timer_handler(registers_t* regs) {
    timer_ticks++;
    
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
