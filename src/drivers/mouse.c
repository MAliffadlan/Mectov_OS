#include "../include/mouse.h"
#include "../include/io.h"
#include "../include/idt.h"
#include "../include/vga.h"
#include "../include/keyboard.h"   // ps2_drain()

volatile int mouse_x = 400, mouse_y = 300;
volatile uint8_t mouse_btn = 0;
volatile int mouse_updated = 0;
volatile int8_t mouse_scroll = 0;  // scroll wheel delta (positive = up, negative = down)

static uint8_t mouse_cycle = 0;
static int8_t  mouse_bytes[4];     // 4 bytes for IntelliMouse
static uint8_t mouse_has_wheel = 0; // 1 = IntelliMouse mode active

// ---- PS/2 controller helpers ----
static void mouse_wait_in()  { uint32_t t=100000; while(t-- && !(inb(0x64)&1)); }
static void mouse_wait_out() { uint32_t t=100000; while(t-- &&  (inb(0x64)&2)); }

static void mouse_write(uint8_t val) {
    mouse_wait_out(); outb(0x64, 0xD4);
    mouse_wait_out(); outb(0x60, val);
}
static uint8_t mouse_read() { mouse_wait_in(); return inb(0x60); }

// Helper: set sample rate
static void mouse_set_sample_rate(uint8_t rate) {
    mouse_write(0xF3); mouse_read(); // ACK
    mouse_write(rate); mouse_read(); // ACK
}

// ---- PS/2 packet state machine ----
// Fed one byte at a time by ps2_drain() (keyboard.c). The byte may arrive via
// IRQ12 or via IRQ1 — the 8042 has a single output buffer shared by both
// devices, so whichever IRQ runs first can pick up the other device's byte.
void mouse_feed_byte(uint8_t data) {
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
            if (mouse_has_wheel) {
                mouse_cycle++;
                break; // wait for 4th byte
            }
            // Fall through for 3-byte mode (no wheel)
            goto process_packet;
        case 3:
            mouse_bytes[3] = (int8_t)data;
            // Fall through to process
        process_packet:
            mouse_cycle = 0;

            // Update buttons
            mouse_btn = mouse_bytes[0] & 0x07; // bits 0-2: left, right, middle

            // Bits 6/7 of byte 0 are the X/Y overflow flags. When either is set
            // the deltas in bytes 1/2 are meaningless, so keep the button state
            // but drop the movement instead of teleporting the cursor.
            if (!((uint8_t)mouse_bytes[0] & 0xC0)) {
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
            }

            // Update scroll wheel (4th byte, only in IntelliMouse mode)
            if (mouse_has_wheel) {
                int8_t sz = mouse_bytes[3];
                // Only the low 4 bits are the Z-axis delta (signed nibble)
                // But in basic IntelliMouse it's a full signed byte
                if (sz != 0) {
                    mouse_scroll = sz; // negative = scroll down, positive = scroll up
                }
            }

            mouse_updated = 1;
            break;
    }
}

// ---- IRQ12 handler ----
static void mouse_handler(registers_t* regs) {
    (void)regs;
    ps2_drain(); // routes by the AUX status bit, not by which IRQ fired
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

    // Set defaults
    mouse_write(0xF6); mouse_read();  // ACK

    // ---- Enable IntelliMouse scroll wheel ----
    // Magic sequence: set sample rate 200, 100, 80
    mouse_set_sample_rate(200);
    mouse_set_sample_rate(100);
    mouse_set_sample_rate(80);

    // Read device ID to check if IntelliMouse mode activated
    mouse_write(0xF2); mouse_read(); // ACK
    uint8_t dev_id = mouse_read();
    if (dev_id == 3) {
        mouse_has_wheel = 1;  // IntelliMouse mode! 4-byte packets
    }

    // Enable data reporting
    mouse_write(0xF4); mouse_read();  // ACK

    // IRQ12 = interrupt vector 44 (0x2C)
    register_interrupt_handler(44, mouse_handler);
}
