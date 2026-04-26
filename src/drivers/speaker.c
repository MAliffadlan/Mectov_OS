#include "../include/speaker.h"
#include "../include/io.h"

int abort_ex = 0;

void play_sound(unsigned int n) { unsigned int d = 1193180/n; outb(0x43,0xb6); outb(0x42,(unsigned char)d); outb(0x42,(unsigned char)(d>>8)); unsigned char t = inb(0x61); if(t!=(t|3)) outb(0x61,t|3); }
void nosound() { outb(0x61, inb(0x61) & 0xFC); }
void delay(int ms) { for (volatile int i = 0; i < ms; i++) { if (inb(0x64) & 1) if (inb(0x60) == 0x01) { abort_ex = 1; nosound(); } if (abort_ex) return; for (volatile int j = 0; j < 4000; j++) __asm__ __volatile__ ("pause"); } }
void beep() { play_sound(1000); delay(100); nosound(); }
void nada(int f, int d) { if (abort_ex) return; if (f > 0) play_sound(f); delay(d); nosound(); }
