#ifndef SECURITY_H
#define SECURITY_H

#include "types.h"

// Set by lock_screen() (shell `lock` builtin, Start menu "Lock"); the kernel
// main loop polls it and runs the login gate without resetting the session.
extern volatile int pending_lock;

// Auto-lock idle timer (v38.51): when auto_lock_secs > 0, the kernel main
// loop fires pending_lock once no keyboard/mouse input has arrived for that
// long (checked at 1 s granularity next to the clock tick). The `locktimeout
// <seconds>` shell builtin sets it (0 disables); every input event bumps
// last_input_tick, and both manual and auto locks restart the countdown
// after unlocking.
extern volatile uint32_t auto_lock_secs;
extern volatile uint32_t last_input_tick;

void lock_screen();
void security_set_auto_lock(uint32_t secs);   // 0 disables; restarts countdown
void security_note_input(void);               // call on any keyboard/mouse event
void security_reset_idle(void);               // restart the countdown (after unlock)
void security_auto_lock_tick(uint32_t now);   // 1 Hz poll; fires pending_lock

#endif
