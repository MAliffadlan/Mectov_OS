#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "types.h"

extern int shift_p, caps_a;
extern int keyboard_ctrl_held;
extern int keyboard_alt_held;

void init_keyboard();
uint8_t k_get_scancode();
char scancode_to_char(uint8_t s);

#endif
