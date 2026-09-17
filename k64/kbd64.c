/* M7.2 PS/2 keyboard (set 1): IRQ1 -> ASCII ring buffer.
 *
 * BSP-only by construction (PIC IRQ1 routes to the BSP; AP LINTs are
 * masked, no IOAPIC in M7): the IRQ writer is single, readers (any CPU via
 * SYS_GETCHAR) only move the tail — safe SPSC with byte indices, no lock.
 * Shift + CapsLock + Backspace handled; Enter yields '\n'. No LED, no
 * keymap switching (US layout baked in).
 */
#include "cpu64.h"

#define KBD_BUF 256

static volatile u8 kbd_buf[KBD_BUF];
static volatile u8 kbd_head = 0; /* writer (IRQ) */
static volatile u8 kbd_tail = 0; /* readers (syscall) */
static int kbd_shift = 0;
static int kbd_caps = 0;

/* Set-1 make codes -> ASCII (0 = non-printing). Shift layer second. */
static const char kbd_map[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0,
};
static const char kbd_map_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0,
};

static inline u8 kbd_inb(u16 port) {
    u8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* Called from the IRQ1 stub (vec 33) with the raw scancode byte. */
void kbd_push(u8 sc) {
    if (sc == 0x2A || sc == 0x36) { kbd_shift = 1; return; }       /* shift */
    if (sc == 0xAA || sc == 0xB6) { kbd_shift = 0; return; }
    if (sc == 0x3A) { kbd_caps = !kbd_caps; return; }              /* caps */
    if (sc & 0x80) return;                                        /* release */
    if (sc >= 128) return;
    char c = kbd_shift ? kbd_map_shift[sc] : kbd_map[sc];
    /* CapsLock flips letters only (shift already applied above). */
    if (kbd_caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c = (char)(c ^ 0x20);
    if (!c) return;
    u8 h = kbd_head;
    u8 n = (u8)(h + 1);
    if (n == kbd_tail) return; /* full: drop oldest? No — drop newest. */
    kbd_buf[h] = (u8)c;
    kbd_head = n;
}

/* Drain any stale bytes (boot firmwaree leftovers). */
void kbd_init(void) {
    while (kbd_inb(0x64) & 1) (void)kbd_inb(0x60);
}

/* Nonblocking Ring-3 read: byte or -1. */
int kbd_try_get(void) {
    u8 t = kbd_tail;
    if (t == kbd_head) return -1;
    int c = kbd_buf[t];
    kbd_tail = (u8)(t + 1);
    return c;
}
