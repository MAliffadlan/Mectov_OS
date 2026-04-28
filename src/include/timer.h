#ifndef TIMER_H
#define TIMER_H

#include "types.h"
#include "idt.h"

void init_timer(uint32_t frequency);
uint32_t get_ticks();
uint32_t timer_get_us();

#endif
