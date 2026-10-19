#ifndef SB16_H
#define SB16_H

#include "types.h"

void sb16_init(void);
void sb16_start_playback(uint16_t sample_rate);
void sb16_stop_playback(void);
void sb16_set_audio_buffer(uint8_t* pcm_data, uint32_t length);
// Atomically publish a new PCM source and return the previous one. The caller
// owns the returned buffer and may free it: the swap happens under sb16_lock,
// which the completion IRQ also holds while it reads from that pointer, so
// once this returns no IRQ can still be using the old buffer.
uint8_t* sb16_swap_audio_buffer(uint8_t* pcm_data, uint32_t length);
void sb16_set_volume(uint8_t vol);      // 0-100
uint8_t sb16_get_volume(void);
int sb16_is_available(void);

#endif
