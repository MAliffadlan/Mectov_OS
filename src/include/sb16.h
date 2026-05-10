#ifndef SB16_H
#define SB16_H

#include "types.h"

void sb16_init(void);
void sb16_start_playback(uint16_t sample_rate);
void sb16_set_audio_buffer(uint8_t* pcm_data, uint32_t length);

#endif
