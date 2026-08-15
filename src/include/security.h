#ifndef SECURITY_H
#define SECURITY_H

// Set by lock_screen() (shell `lock` builtin, Start menu "Lock"); the kernel
// main loop polls it and runs the login gate without resetting the session.
extern volatile int pending_lock;
void lock_screen();

#endif
