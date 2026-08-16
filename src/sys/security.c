// security.c — Lock-screen request flag + idle auto-lock timer.
//
// lock_screen() just raises a flag: the kernel main loop (kernel.c) notices
// it, runs the graphical login gate (gui_login) OVER the live desktop
// WITHOUT resetting the session, and restores everything on unlock. The old
// text-mode stub (print "System Locked. Please enter password." into the
// console) never verified anything — is_locked was set and never read, so it
// has been replaced by this flag. The `lock` shell builtin and the Start
// menu "Lock" item both funnel through here.
//
// v38.51 adds the idle auto-lock: `locktimeout <seconds>` arms a countdown
// that raises the same flag once keyboard+mouse have been quiet long enough.
#include "../include/security.h"

extern uint32_t get_ticks();

volatile int pending_lock = 0;
volatile uint32_t auto_lock_secs = 0;
volatile uint32_t last_input_tick = 0;

void lock_screen() {
    pending_lock = 1;
}

// `locktimeout <seconds>`: 0 disables. Restart the countdown from now so the
// new timeout applies immediately instead of inheriting an old idle span.
void security_set_auto_lock(uint32_t secs) {
    auto_lock_secs = secs;
    last_input_tick = get_ticks();
}

void security_note_input(void) {
    last_input_tick = get_ticks();
}

void security_reset_idle(void) {
    last_input_tick = get_ticks();
}

// Called at 1 s granularity from the kernel main loop. Fires the lock when
// the quiet span passes the timeout, then re-anchors the clock so the lock
// does not fire again immediately after the user unlocks.
void security_auto_lock_tick(uint32_t now) {
    if (auto_lock_secs == 0) return;
    if (now - last_input_tick >= auto_lock_secs * 1000u) {
        pending_lock = 1;
        last_input_tick = now;
    }
}
