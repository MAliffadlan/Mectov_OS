#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "types.h"

extern int shift_p, caps_a;
extern int keyboard_ctrl_held;
extern int keyboard_alt_held;

void init_keyboard();
// Drains the shared 8042 output buffer and routes each byte to the keyboard or
// the mouse based on the AUX status bit. Called by BOTH IRQ1 and IRQ12 — never
// read port 0x60 directly from an IRQ handler.
void ps2_drain(void);
// Returns 1 once per ESC press, then clears. For code that must notice ESC
// while busy without reading port 0x60 itself.
int keyboard_take_esc(void);
uint8_t k_get_scancode();
char scancode_to_char(uint8_t s);

#endif
