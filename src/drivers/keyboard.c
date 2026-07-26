#include "../include/keyboard.h"
#include "../include/io.h"
#include "../include/idt.h"
#include "../include/vga.h"
#include "../include/mouse.h"   // mouse_feed_byte()

int shift_p = 0, caps_a = 0;
int keyboard_ctrl_held = 0;
int keyboard_alt_held = 0;

// Scancode Buffer (Gudang Antrean)
#define KBD_BUFFER_SIZE 2048
static uint8_t kbd_buffer[KBD_BUFFER_SIZE];
static volatile uint32_t kbd_head = 0;
static volatile uint32_t kbd_tail = 0;

// Set on ESC press, cleared by keyboard_take_esc(). Lets code that needs to
// notice ESC while busy (speaker.c's delay loop) do so without reading port
// 0x60 behind ps2_drain()'s back. The scancode still reaches kbd_buffer below,
// so normal ESC handling is unaffected.
static volatile int esc_pressed = 0;

int keyboard_take_esc(void) {
    if (!esc_pressed) return 0;
    esc_pressed = 0;
    return 1;
}

static void keyboard_feed_byte(uint8_t scancode) {
    // Update modifier state
    if (scancode == 0x01) esc_pressed = 1;              // ESC press
    else if (scancode == 0x2A || scancode == 0x36) shift_p = 1;
    else if (scancode == 0xAA || scancode == 0xB6) shift_p = 0;
    else if (scancode == 0x3A) caps_a = !caps_a;
    else if (scancode == 0x1D) keyboard_ctrl_held = 1;  // Left Ctrl press
    else if (scancode == 0x9D) keyboard_ctrl_held = 0;  // Left Ctrl release
    else if (scancode == 0x38) keyboard_alt_held = 1;   // Left Alt press
    else if (scancode == 0xB8) keyboard_alt_held = 0;   // Left Alt release

    // Push to buffer
    uint32_t next = (kbd_head + 1) % KBD_BUFFER_SIZE;
    if (next != kbd_tail) {
        kbd_buffer[kbd_head] = scancode;
        kbd_head = next;
    }
}

// Drain the 8042 output buffer, routing every byte to the device it came from.
//
// The controller has ONE output buffer shared by the keyboard and the mouse.
// Status bit 0 says a byte is waiting; status bit 5 says that byte came from the
// AUX (mouse) port. Both IRQ1 and IRQ12 call this, because whichever fires first
// may find the other device's byte sitting in the buffer.
//
// Reading without testing bit 5 is what caused stuck modifiers and cursor jumps:
// a mouse byte of 0x38 latched keyboard_alt_held with no matching 0xB8 release,
// 0x1D turned every later keypress into a Ctrl chord, and the mouse packet state
// machine lost a byte and went out of phase.
//
// The drain must be a loop: the PS/2 IRQ is edge-triggered, so leaving a byte
// behind loses the edge and the device stops reporting until something else
// happens to read the port.
void ps2_drain(void) {
    // Hard cap — never spin on hardware without a bound inside an ISR.
    for (int guard = 0; guard < 64; guard++) {
        uint8_t status = inb(0x64);
        if (!(status & 0x01)) break;      // output buffer empty, done
        uint8_t data = inb(0x60);
        if (status & 0x20) mouse_feed_byte(data);
        else               keyboard_feed_byte(data);
    }
}

static void keyboard_handler(registers_t* regs) {
    (void)regs;
    ps2_drain();
}

void init_keyboard() {
    register_interrupt_handler(33, keyboard_handler);
}

// Ambil scancode dari antrean (Non-blocking)
uint8_t k_get_scancode() {
    if (kbd_head == kbd_tail) return 0;
    uint8_t scancode = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUFFER_SIZE;
    return scancode;
}

char scancode_to_char(uint8_t s) {
    static unsigned char m_n[] = { 0, 0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0 };
    static unsigned char m_s[] = { 0, 0x1B, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '\"', '~', 0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0 };
    if (s < 58) {
        char c = m_n[s], cs = m_s[s]; int isl = (c >= 'a' && c <= 'z');
        if (shift_p) return (isl && caps_a) ? c : cs;
        else return (isl && caps_a) ? cs : c;
    }
    return 0;
}
