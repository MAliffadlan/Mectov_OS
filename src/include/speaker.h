#ifndef SPEAKER_H
#define SPEAKER_H

extern int abort_ex;

void play_sound(unsigned int n);
void nosound();
void delay(int ms);
void beep();
void nada(int f, int d);

// Non-blocking tone: start the pitch now, and let the timer tick switch the
// speaker off once `duration_ms` has passed. This is the shape the
// `SYS_PLAY_SOUND` callers want -- snake/flappy/volume all fire sound effects
// from inside a game loop and then carry on, so the blocking `nada()` above
// would freeze Snake for the whole 500 ms of its game-over note.
void speaker_tone(int freq, int duration_ms);

// Cancel only the non-blocking deadline. The legacy blocking nada()/beep()
// paths call this before taking over the PIT, without holding the lock across
// their busy wait or changing the gate themselves.
void speaker_cancel(void);

// Called once per BSP timer tick. Silence the speaker when the tone's deadline
// has passed. Cheap and self-gating: it does nothing when no tone is pending.
void speaker_tick(void);

#endif
