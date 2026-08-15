// security.c — Lock-screen request flag.
//
// lock_screen() just raises a flag: the kernel main loop (kernel.c) notices
// it, runs the graphical login gate (gui_login) OVER the live desktop
// WITHOUT resetting the session, and restores everything on unlock. The old
// text-mode stub (print "System Locked. Please enter password." into the
// console) never verified anything — is_locked was set and never read, so it
// has been replaced by this flag. The `lock` shell builtin and the Start
// menu "Lock" item both funnel through here.
#include "../include/security.h"

volatile int pending_lock = 0;

void lock_screen() {
    pending_lock = 1;
}
