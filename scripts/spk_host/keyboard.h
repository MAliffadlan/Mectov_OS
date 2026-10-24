// scripts/spk_host/keyboard.h — host shim for src/include/keyboard.h.
//
// src/drivers/speaker.c includes it for delay(), which drains the 8042 output
// buffer and picks up ESC. The deadline logic under test never calls delay(),
// and a host process has no keyboard controller, so both entry points are
// inert here. They exist so the shipped file compiles unchanged.
#ifndef SPK_HOST_KEYBOARD_H
#define SPK_HOST_KEYBOARD_H

static inline void ps2_drain(void) {}
static inline int keyboard_take_esc(void) { return 0; }

#endif
