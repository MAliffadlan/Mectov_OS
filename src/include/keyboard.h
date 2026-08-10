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
// Pop the next scancode together with the modifier snapshot (bit0=shift,
// bit1=ctrl, bit2=alt) captured when the byte was fed — see keyboard.c for
// why consumers that resolve a character must use this instead of the live
// modifier flags (the shift-release byte can be fed before a slow consumer
// pops the key, turning "shift-7" into '7').
uint8_t k_get_scancode_ex(uint8_t* mods_out);
char scancode_to_char(uint8_t s);
// Resolve a scancode against a feed-time modifier snapshot (shift = mods&1).
char scancode_to_char_mods(uint8_t s, uint8_t mods);

#endif
