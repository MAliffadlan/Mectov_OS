#include "../include/keyboard.h"
#include "../include/io.h"
#include "../include/idt.h"
#include "../include/vga.h"

int shift_p = 0, caps_a = 0;
int keyboard_ctrl_held = 0;

// Scancode Buffer (Gudang Antrean)
#define KBD_BUFFER_SIZE 256
static uint8_t kbd_buffer[KBD_BUFFER_SIZE];
static uint32_t kbd_head = 0;
static uint32_t kbd_tail = 0;

static void keyboard_handler(registers_t* regs) {
    (void)regs;
    uint8_t scancode = inb(0x60);
    
    // Update modifier state
    if (scancode == 0x2A || scancode == 0x36) shift_p = 1; 
    else if (scancode == 0xAA || scancode == 0xB6) shift_p = 0; 
    else if (scancode == 0x3A) caps_a = !caps_a;
    else if (scancode == 0x1D) keyboard_ctrl_held = 1;  // Left Ctrl press
    else if (scancode == 0x9D) keyboard_ctrl_held = 0;  // Left Ctrl release

    // Push to buffer
    uint32_t next = (kbd_head + 1) % KBD_BUFFER_SIZE;
    if (next != kbd_tail) {
        kbd_buffer[kbd_head] = scancode;
        kbd_head = next;
    }
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
    static unsigned char m_n[] = { 0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0 };
    static unsigned char m_s[] = { 0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '\"', '~', 0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0 };
    if (s < 58) {
        char c = m_n[s], cs = m_s[s]; int isl = (c >= 'a' && c <= 'z');
        if (shift_p) return (isl && caps_a) ? c : cs;
        else return (isl && caps_a) ? cs : c;
    }
    return 0;
}
