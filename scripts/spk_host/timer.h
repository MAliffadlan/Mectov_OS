// scripts/spk_host/timer.h — controllable host clock for speaker.c.
//
// get_ticks() is the same public clock accessor the kernel driver uses. Its
// host implementation can invoke a scripted IRQ before returning, allowing a
// deterministic task/IRQ interleaving without changing the shipped driver.
#ifndef SPK_HOST_TIMER_H
#define SPK_HOST_TIMER_H

#include "types.h"

extern volatile uint32_t spk_host_ticks;
extern void (*spk_host_before_tick_read)(void);
uint32_t get_ticks(void);

#endif
