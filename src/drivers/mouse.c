#include "../include/mouse.h"
#include "../include/io.h"
#include "../include/idt.h"
#include "../include/vga.h"

volatile int mouse_x = 400, mouse_y = 300;
volatile uint8_t mouse_btn = 0;
volatile int mouse_updated = 0;

static uint8_t mouse_cycle = 0;
static int8_t  mouse_bytes[3];

// ---- PS/2 controller helpers ----
static void mouse_wait_in()  { uint32_t t=100000; while(t-- && !(inb(0x64)&1)); }
static void mouse_wait_out() { uint32_t t=100000; while(t-- &&  (inb(0x64)&2)); }

static void mouse_write(uint8_t val) {
    mouse_wait_out(); outb(0x64, 0xD4);
    mouse_wait_out(); outb(0x60, val);
}
static uint8_t mouse_read() { mouse_wait_in(); return inb(0x60); }

// ---- IRQ12 handler ----
static void mouse_handler(registers_t* regs) {
    (void)regs;
    uint8_t data = inb(0x60);
    switch (mouse_cycle) {
        case 0:
            mouse_bytes[0] = (int8_t)data;
            if (data & 0x08) mouse_cycle++;   // alignment bit must be set
            break;
        case 1:
            mouse_bytes[1] = (int8_t)data;
            mouse_cycle++;
            break;
        case 2:
            mouse_bytes[2] = (int8_t)data;
            mouse_cycle = 0;

            // Update buttons
            mouse_btn = mouse_bytes[0] & 0x03;

            // Update position (Y is inverted in PS/2)
            int dx = mouse_bytes[1];
            int dy = mouse_bytes[2];
            if (mouse_bytes[0] & 0x10) dx |= (int)0xFFFFFF00;
            if (mouse_bytes[0] & 0x20) dy |= (int)0xFFFFFF00;

            mouse_x += dx;
            mouse_y -= dy;

            // Clamp to screen bounds
            if (mouse_x < 0)              mouse_x = 0;
            if (mouse_x >= (int)fb_width) mouse_x = (int)fb_width  - 1;
            if (mouse_y < 0)              mouse_y = 0;
            if (mouse_y >= (int)fb_height)mouse_y = (int)fb_height - 1;

            mouse_updated = 1;
            break;
    }
}

void init_mouse() {
    // Enable auxiliary PS/2 device
    mouse_wait_out(); outb(0x64, 0xA8);

    // Enable IRQ12 (bit 1 of compaq status byte)
    mouse_wait_out(); outb(0x64, 0x20);
    mouse_wait_in();
    uint8_t status = (inb(0x60) | 0x02) & ~0x20;
    mouse_wait_out(); outb(0x64, 0x60);
    mouse_wait_out(); outb(0x60, status);

    // Set defaults + enable data reporting
    mouse_write(0xF6); mouse_read();  // ACK
    mouse_write(0xF4); mouse_read();  // ACK

    // IRQ12 = interrupt vector 44 (0x2C)
    register_interrupt_handler(44, mouse_handler);
}
