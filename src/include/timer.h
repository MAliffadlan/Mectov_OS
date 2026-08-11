#ifndef TIMER_H
#define TIMER_H

#include "types.h"
#include "idt.h"

void init_timer(uint32_t frequency);
uint32_t get_ticks();
uint32_t timer_get_us();
void timer_calibrate_ticks_per_sec(void);
void timer_update_rate_if_second(void);
extern volatile uint32_t ticks_per_sec;

#endif
