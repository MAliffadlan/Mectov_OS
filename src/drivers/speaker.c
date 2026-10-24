#include "../include/speaker.h"
#include "../include/io.h"
#include "../include/keyboard.h"
#include "../include/spinlock.h"
#include "../include/timer.h"

int abort_ex = 0;

void play_sound(unsigned int n) { unsigned int d = 1193180/n; outb(0x43,0xb6); outb(0x42,(unsigned char)d); outb(0x42,(unsigned char)(d>>8)); unsigned char t = inb(0x61); if(t!=(t|3)) outb(0x61,t|3); }
void nosound() { outb(0x61, inb(0x61) & 0xFC); }

// Busy-wait for ~ms, aborting early on ESC.
//
// This used to read port 0x60 directly. That is the shared 8042 output buffer:
// consuming it here could steal a mouse byte before the owning driver saw it,
// knocking the PS/2 packet state machine out of phase. Go through ps2_drain()
// so each byte still reaches the device that owns it, and pick up ESC from the
// flag it sets. int 0x80 is now a preemptible trap gate, not an IF=0 section.
void delay(int ms) {
    for (volatile int i = 0; i < ms; i++) {
        ps2_drain();
        if (keyboard_take_esc()) { abort_ex = 1; nosound(); }
        if (abort_ex) return;
        for (volatile int j = 0; j < 4000; j++) __asm__ __volatile__ ("pause");
    }
}

// The blocking startup/chime path takes ownership of the PIT, but does not
// keep tone_lock during its busy wait. Cancel only the old non-blocking
// deadline: it must not fire in the middle of (or after) this legacy note.
void beep() { abort_ex = 0; speaker_cancel(); play_sound(1000); delay(100); nosound(); }
void nada(int f, int d) { abort_ex = 0; speaker_cancel(); if (f > 0) play_sound(f); delay(d); nosound(); }

// ---- Non-blocking tone + its timer-tick deadline ----
//
// Two contexts touch this: `speaker_tone` from a syscall (task context, may be
// preempted, possibly on any core) and `speaker_tick` from the BSP's timer IRQ.
// Aligned scalars alone are NOT enough: a tick between programming a new pitch
// and publishing its deadline would retire the new pitch using the old note's
// deadline. Serialize the complete port + state transaction, following sb16's
// task/IRQ discipline: irqsave from task context, plain spin_lock in IRQ.
// The lock is a leaf and is never held while sleeping or waiting for a note.
static spinlock_t tone_lock = SPINLOCK_INIT;
static uint32_t tone_off_ms = 0;   // 0 = nothing pending
static int tone_active = 0;        // accessed only while tone_lock is held

void speaker_cancel(void) {
    uint32_t nf = spin_lock_irqsave(&tone_lock);
    tone_active = 0;
    tone_off_ms = 0;
    spin_unlock_irqrestore(&tone_lock, nf);
}

void speaker_tone(int freq, int duration_ms) {
    uint32_t nf = spin_lock_irqsave(&tone_lock);
    abort_ex = 0;
    if (freq > 0) play_sound(freq);

    if (duration_ms <= 0) {           // a zero-length note is just silence
        nosound();
        tone_active = 0;
        tone_off_ms = 0;
    } else {
        // get_ticks() counts MILLISECONDS (v38.80), so this is a plain ms deadline.
        tone_off_ms = get_ticks() + (uint32_t)duration_ms;
        tone_active = 1;
    }
    spin_unlock_irqrestore(&tone_lock, nf);
}

void speaker_tick(void) {
    // Read the state only while holding the lock: even a fast-path deadline
    // snapshot can be stale by the time the lock is acquired, and must never
    // authorize silencing a newer pitch.
    spin_lock(&tone_lock);
    if (tone_active) {
        // Signed difference, so the wrap is handled in BOTH directions — a
        // bare `get_ticks() >= tone_off_ms` fails at each end, measured by
        // scripts/spk_deadline_test.py (--driver against that form): a wrapped
        // deadline reads as past and cuts the note one tick in; a wrapped
        // counter under an unwrapped deadline reads as ~49 days in the future.
        // Both require arming within duration_ms + one tick of the wrap, so
        // they turn up about once per 49.7 days of uptime and need a host gate.
        if ((int32_t)(get_ticks() - tone_off_ms) >= 0) {
            nosound();
            tone_active = 0;
            tone_off_ms = 0;
        }
    }
    spin_unlock(&tone_lock);
}
