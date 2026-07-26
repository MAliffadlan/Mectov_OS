#include "../include/speaker.h"
#include "../include/io.h"
#include "../include/keyboard.h"

int abort_ex = 0;

void play_sound(unsigned int n) { unsigned int d = 1193180/n; outb(0x43,0xb6); outb(0x42,(unsigned char)d); outb(0x42,(unsigned char)(d>>8)); unsigned char t = inb(0x61); if(t!=(t|3)) outb(0x61,t|3); }
void nosound() { outb(0x61, inb(0x61) & 0xFC); }

// Busy-wait for ~ms, aborting early on ESC.
//
// This used to read port 0x60 directly. That is the shared 8042 output buffer:
// when this runs from the Ring 3 shell (int 0x80 => IF=0, so no IRQ drains it)
// it was the only reader, and every mouse byte it swallowed knocked the PS/2
// packet state machine out of phase. Go through ps2_drain() so each byte still
// reaches the device that owns it, and pick up ESC from the flag it sets.
void delay(int ms) {
    for (volatile int i = 0; i < ms; i++) {
        ps2_drain();
        if (keyboard_take_esc()) { abort_ex = 1; nosound(); }
        if (abort_ex) return;
        for (volatile int j = 0; j < 4000; j++) __asm__ __volatile__ ("pause");
    }
}

// abort_ex is cleared on entry: nothing else in the tree ever resets it, so
// leaving it latched would let a single ESC silence the speaker permanently.
// Aborting mid-note still works via delay()'s early return above.
void beep() { abort_ex = 0; play_sound(1000); delay(100); nosound(); }
void nada(int f, int d) { abort_ex = 0; if (f > 0) play_sound(f); delay(d); nosound(); }
