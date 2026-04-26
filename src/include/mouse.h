#ifndef MOUSE_H
#define MOUSE_H

#include "types.h"

extern volatile int mouse_x, mouse_y;
extern volatile uint8_t mouse_btn;    // bit0=left, bit1=right
extern volatile int mouse_updated;    // flag: new packet arrived

void init_mouse();

#endif
