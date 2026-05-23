#ifndef MOUSE_H
#define MOUSE_H

#include "types.h"

extern volatile int mouse_x, mouse_y;
extern volatile uint8_t mouse_btn;    // bit0=left, bit1=right, bit2=middle
extern volatile int mouse_updated;    // flag: new packet arrived
extern volatile int8_t mouse_scroll;  // scroll wheel delta (>0 = up, <0 = down)

void init_mouse();

#endif
