#ifndef MOUSE_H
#define MOUSE_H

#include "types.h"

extern volatile int mouse_x, mouse_y;
extern volatile uint8_t mouse_btn;    // bit0=left, bit1=right, bit2=middle
extern volatile int mouse_updated;    // flag: new packet arrived
extern volatile int8_t mouse_scroll;  // scroll wheel delta (>0 = up, <0 = down)

void init_mouse();
// Feeds one AUX byte into the packet state machine. Call only from ps2_drain().
void mouse_feed_byte(uint8_t data);
// Apply one USB HID boot-protocol pointer report (buttons, signed deltas,
// wheel) to the same cursor state the PS/2 path maintains. HID axes are
// already sign-correct: +x = right, +y = DOWN (fb origin is top-left), +wheel
// = scroll up. Called by the xHCI driver (xhci.c) under xhci_lock with IRQs
// off — the single-writer rule the IRQ12 path already follows.
void mouse_hid_report(uint8_t btn, int8_t dx, int8_t dy, int8_t wheel);

#endif
