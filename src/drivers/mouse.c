#include "../include/mouse.h"
#include "../include/io.h"
#include "../include/idt.h"
#include "../include/vga.h"
#include "../include/keyboard.h"   // ps2_drain()
#include "../include/entropy.h"   // entropy_add() — kernel CSPRNG feed

volatile int mouse_x = 400, mouse_y = 300;
volatile uint8_t mouse_btn = 0;
volatile int mouse_updated = 0;
volatile int8_t mouse_scroll = 0;  // scroll wheel delta (positive = up, negative = down)

// v38.103: raw relative motion since the last mouse_take_delta(). Written from
// IRQ context, read by the desktop loop for the window that captured the
// mouse; the absolute cursor below is clamped and useless for aiming.
volatile int mouse_raw_dx = 0, mouse_raw_dy = 0;

// v38.149: every pointer packet the guest actually received, cumulatively —
// both paths (PS/2 IRQ12 and xHCI HID) count, because the question it answers
// is "did the host deliver anything at all".
//
// The window layer already counts events delivered to a game window
// (look_s/key_s in q3_vm.c's perf line). The two numbers differ in exactly one
// situation and it is the one the player's 12:46 session was in: the game held
// the mouse capture (comp_s ~27/s) and received ZERO events for 375 s, with
// look_s=0 in 103 of 108 windows. If this counter is also still, the loss is
// upstream of the guest (a QEMU window without input focus, or a frontend that
// never grabbed the pointer so it stopped at the window edge) — host side, and
// no amount of guest code can fix it. If this counter is MOVING while look_s is
// 0, the packets arrive and the fault is in the guest's own drain path.
static volatile unsigned mouse_pkt_total;
unsigned mouse_pkt_count(void) { return mouse_pkt_total; }

// Returns the accumulated relative motion (screen space: +x right, +y down)
// and clears it. 1 = there was motion, 0 = idle.
int mouse_take_delta(int *dx, int *dy) {
    /* v38.150: this was cli/read/clear/sti, which is atomic only on a
     * uniprocessor. The desktop loop and the game task drain from DIFFERENT
     * cores, so two overlapping drains both read the same counts before
     * either cleared them — measured as exactly 4x motion on 4 cores
     * (0.20 deg/px for a 0.05 setting) and exactly 1x on 1 core, which is
     * why every harness (single-core-ish timing) looked smooth while a real
     * hand did not. xchg reads and clears in one indivisible step, on every
     * core at once; the IRQ-side adds below are lock-prefixed for the same
     * reason, so a count can neither be delivered twice nor lost. */
    int rx = __sync_lock_test_and_set(&mouse_raw_dx, 0);
    int ry = __sync_lock_test_and_set(&mouse_raw_dy, 0);
    if (dx) *dx = rx;
    if (dy) *dy = ry;
    return (rx != 0 || ry != 0);
}

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
    // Mix the raw PS/2 byte + IRQ timing into the entropy pool (v38.52).
    entropy_add(data);
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

                /* v38.150: lock-prefixed add — the drain side xchg's these from
                 * another core, so a plain += can interleave with it and
                 * resurrect (double-deliver) or drop counts. */
                __sync_fetch_and_add(&mouse_raw_dx, dx);
                __sync_fetch_and_add(&mouse_raw_dy, -dy);   // PS/2 +y is up; screen space is +y down
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
            mouse_pkt_total++;   // v38.149: see mouse_pkt_count()
            break;
    }
}

// ---- IRQ12 handler ----
static void mouse_handler(registers_t* regs) {
    (void)regs;
    ps2_drain(); // routes by the AUX status bit, not by which IRQ fired
    // Pointer motion should reach the compositor immediately, not at the
    // next 10 ms tick (see take_resched).
    extern void request_resched(void);
    request_resched();
}

void init_mouse() {
    // The whole protocol runs with interrupts disabled: IRQ1/IRQ12 fire
    // ps2_drain(), which can steal the controller's replies from under our
    // direct inb(0x60) reads. Most notably the command-byte response to
    // `outb(0x64, 0x20)` — usually 0x41-0x4F — gets queued as a bogus
    // keyboard "scancode" that later input consumers read as a real keypress
    // (it once dismissed the lock screen instantly at boot). The poll loops
    // make this a few hundred microseconds at most.
    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));

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

    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));

    // IRQ12 = interrupt vector 44 (0x2C)
    register_interrupt_handler(44, mouse_handler);
}

// USB HID boot-protocol pointer report -> the shared cursor state (see
// mouse.h). Keeps the exact clamp/scroll semantics of the PS/2 path: buttons
// bit0=left bit1=right bit2=middle (same layout as the HID button byte),
// wheel sign as in the report (+ = up, matching mouse_scroll). The PS/2
// overflow-drop rule does not apply — HID deltas are already sign-extended.
void mouse_hid_report(uint8_t btn, int8_t dx, int8_t dy, int8_t wheel) {
    entropy_add((uint32_t)(uint8_t)dx);
    entropy_add((uint32_t)(uint8_t)dy);
    mouse_btn = btn & 0x07;
    /* v38.150: same SMP rule as the PS/2 path above — the drain xchg's from
     * another core. */
    __sync_fetch_and_add(&mouse_raw_dx, dx);
    __sync_fetch_and_add(&mouse_raw_dy, dy);   // HID +y is already screen space (down)
    mouse_x += dx;
    mouse_y += dy;
    if (mouse_x < 0)               mouse_x = 0;
    if (mouse_x >= (int)fb_width)  mouse_x = (int)fb_width  - 1;
    if (mouse_y < 0)               mouse_y = 0;
    if (mouse_y >= (int)fb_height) mouse_y = (int)fb_height - 1;
    if (wheel) mouse_scroll = wheel;
    mouse_updated = 1;
    mouse_pkt_total++;   // v38.149: see mouse_pkt_count()
}

