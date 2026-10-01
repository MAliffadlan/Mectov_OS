/* M9 GUI-1: PS/2 mouse (IRQ12) -> cursor position for the desktop.
 *
 * Mirrors the 32-bit driver (src/drivers/mouse.c): enable the aux port, set
 * the compaq status byte's IRQ12 bit, F6 defaults, F4 reporting, then decode
 * 3-byte packets. Two deliberate simplifications, both documented:
 *   - no IntelliMouse wheel negotiation (F3 200/100/80 + F2) — a desktop
 *     bring-up needs X/Y/buttons, and the boot-time handshake stays short;
 *   - the init sequence runs with interrupts disabled, like the 32-bit one,
 *     because IRQ1/IRQ12 handlers steal 8042 replies from under our direct
 *     inb(0x60) reads (that bug once dismissed the 32-bit lock screen).
 *
 * Routing: the so-called "keyboard controller" has ONE output buffer shared
 * by both devices, so a byte can be picked up by either IRQ. ps2_drain64()
 * therefore runs from both handlers and dispatches on the status byte's AUX
 * bit (0x20) instead of trusting which IRQ fired — the same rule the 32-bit
 * ps2_drain() uses, and the reason a mouse never types garbage on serial.
 *
 * BSP-only, like the M7 keyboard: PIC IRQ12 routes to the BSP and AP LINTs
 * are masked. The IRQ writer is therefore single; the desktop reads x/y from
 * the IRQ path (move hook) and from its own init, so no lock is needed.
 */
#include "cpu64.h"

static volatile int m_x = 400, m_y = 300; /* 32-bit kernel's boot position */
static volatile int m_btn = 0;
static u8 m_pkt[3];
static int m_cycle = 0;
static int m_seen = 0; /* packets decoded (first-motion marker for tests) */
static void (*m_move)(int dx, int dy);
static void (*m_btn_hook)(int x, int y, int btn, int changed);
static int m_btn_prev; /* button mask of the previous packet */

static inline u8 inb64(u16 port) {
    u8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outb64(u16 port, u8 v) {
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}

static void wait_in(void) {
    u32 t = 100000;
    while (t-- && !(inb64(0x64) & 1)) { }
}
static void wait_out(void) {
    u32 t = 100000;
    while (t-- && (inb64(0x64) & 2)) { }
}
static void mouse_write(u8 val) {
    wait_out();
    outb64(0x64, 0xD4);
    wait_out();
    outb64(0x60, val);
}
static u8 mouse_read(void) {
    wait_in();
    return inb64(0x60);
}

/* Decode one PS/2 byte into the shared cursor state. Byte 0's bit 3 is the
 * alignment marker, so a dropped byte mid-stream cannot shift the packet
 * forever. Bits 6/7 are X/Y overflow: the deltas are meaningless then, so
 * keep the buttons and drop the movement (mirrors src/drivers/mouse.c). */
void mouse64_feed(u8 data) {
    switch (m_cycle) {
    case 0:
        m_pkt[0] = data;
        if (!(data & 0x08)) return; /* not aligned yet: stay on byte 0 */
        m_cycle = 1;
        return;
    case 1:
        m_pkt[1] = data;
        m_cycle = 2;
        return;
    default:
        m_pkt[2] = data;
        m_cycle = 0;
        break;
    }

    m_btn = m_pkt[0] & 0x07;
    int dx = 0, dy = 0;
    if (!(m_pkt[0] & 0xC0)) {
        dx = (int)(signed char)m_pkt[1];
        dy = (int)(signed char)m_pkt[2];
        m_x += dx;
        m_y -= dy; /* PS/2 Y grows upward */
        if (m_x < 0) m_x = 0;
        if (m_y < 0) m_y = 0;
        if (gfx_width() && m_x > gfx_width() - 1) m_x = gfx_width() - 1;
        if (gfx_height() && m_y > gfx_height() - 1) m_y = gfx_height() - 1;
    }
    if (!m_seen) {
        m_seen = 1;
        s_printf("[K64] mouse: first packet (%u, %u)\n", (u64)m_x, (u64)m_y);
    }
    /* M14: a button edge is reported BEFORE the move hook, so a press that
     * starts a drag is already recorded when the movement of the same packet
     * is processed — one decision, one repaint. */
    if (m_btn != m_btn_prev) {
        int changed = m_btn ^ m_btn_prev;
        m_btn_prev = m_btn;
        if (m_btn_hook) m_btn_hook(m_x, m_y, m_btn, changed);
    }
    if (m_move && (dx || dy)) m_move(dx, dy);
}

/* Drain the 8042 output buffer, routing each byte to the device that sent it
 * (status bit 5 = AUX). Called from the IRQ1 and IRQ12 handlers, IF=0. */
void ps2_drain64(void) {
    int guard = 64; /* a stuck status bit must not spin forever */
    while ((inb64(0x64) & 1) && guard-- > 0) {
        u8 status = inb64(0x64);
        u8 data = inb64(0x60);
        if (status & 0x20) mouse64_feed(data);
        else kbd_push(data);
    }
}

int mouse64_x(void) { return m_x; }
int mouse64_y(void) { return m_y; }
int mouse64_buttons(void) { return m_btn; }
void mouse64_set_move_hook(void (*fn)(int dx, int dy)) { m_move = fn; }
void mouse64_set_button_hook(void (*fn)(int x, int y, int btn, int changed)) {
    m_btn_hook = fn;
}

void mouse64_init(void) {
    u64 flags;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags));
    while (inb64(0x64) & 1) (void)inb64(0x60); /* drain firmware leftovers */

    wait_out();
    outb64(0x64, 0xA8); /* enable the auxiliary (mouse) port */

    wait_out();
    outb64(0x64, 0x20); /* read the compaq status byte */
    wait_in();
    u8 status = (u8)((inb64(0x60) | 0x02) & ~0x20); /* IRQ12 on, clock on */
    wait_out();
    outb64(0x64, 0x60);
    wait_out();
    outb64(0x60, status);

    mouse_write(0xF6); /* set defaults */
    mouse_read();      /* ACK */
    mouse_write(0xF4); /* enable data reporting */
    mouse_read();      /* ACK */

    __asm__ __volatile__("push %0; popfq" : : "r"(flags));
    s_puts("[K64] mouse: PS/2 aux on (IRQ12, 3-byte packets)\n");
}
