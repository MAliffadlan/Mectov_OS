#ifndef SB16_H
#define SB16_H

#include "types.h"

// Audio format flags
#define SB16_FMT_8BIT       0x00
#define SB16_FMT_16BIT      0x01
#define SB16_FMT_MONO       0x00
#define SB16_FMT_STEREO     0x02
#define SB16_FMT_SIGNED     0x04
#define SB16_FMT_UNSIGNED   0x00

// Playback states
#define SB16_STATE_STOPPED  0
#define SB16_STATE_PLAYING  1
#define SB16_STATE_PAUSED   2

// Audio format structure
typedef struct {
    uint8_t bits;           // 8 or 16
    uint8_t channels;       // 1 (mono) or 2 (stereo)
    uint8_t is_signed;      // 0 = unsigned, 1 = signed
    uint16_t sample_rate;   // Hz (e.g., 22050, 44100)
} sb16_format_t;

// Mixer controls
typedef struct {
    uint8_t master;         // 0-100
    uint8_t bass;           // 0-100
    uint8_t treble;         // 0-100
    uint8_t pcm;            // 0-100
} sb16_mixer_t;

// Core functions
void sb16_init(void);
void sb16_start_playback(uint16_t sample_rate);
void sb16_start_playback_ex(sb16_format_t* format);
void sb16_stop_playback(void);
void sb16_pause_playback(void);
void sb16_resume_playback(void);

// Buffer management
void sb16_set_audio_buffer(uint8_t* pcm_data, uint32_t length);
uint8_t* sb16_swap_audio_buffer(uint8_t* pcm_data, uint32_t length);

// Volume and mixer
void sb16_set_volume(uint8_t vol);      // 0-100
uint8_t sb16_get_volume(void);
void sb16_set_mixer(sb16_mixer_t* mixer);
void sb16_get_mixer(sb16_mixer_t* mixer);

// Status
int sb16_is_available(void);
int sb16_get_state(void);               // Returns SB16_STATE_*
uint32_t sb16_get_position(void);       // Returns current playback position

#endif
