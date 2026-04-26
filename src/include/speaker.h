#ifndef SPEAKER_H
#define SPEAKER_H

extern int abort_ex;

void play_sound(unsigned int n);
void nosound();
void delay(int ms);
void beep();
void nada(int f, int d);

#endif
