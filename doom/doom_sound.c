// ============================================================
// DOOM Sound Module — bridges DOOM's sound system to Mectov OS SB16
// ============================================================

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "m_argv.h"

// Stubs for libsamplerate (referenced by i_sound.c FEATURE_SOUND)
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

// Mectov OS SB16 driver
extern void sb16_set_audio_buffer(unsigned char* pcm, unsigned int len);
extern void sb16_start_playback(unsigned short rate);

extern void write_serial_string(const char*);
extern void write_serial_hex(unsigned int);

// ---- Mixer Configuration ----
#define MIX_CHANNELS   8
#define MIX_RATE       11025
#define MIX_BUFFER_LEN 4096   // Samples per mix pass

// ---- Channel state ----
typedef struct {
    unsigned char* data;      // PCM data pointer (8-bit unsigned, DMX format)
    int length;               // Total samples
    int pos;                  // Current playback position
    int vol;                  // Volume (0-127)
    int sep;                  // Stereo separation (unused for mono, kept for API)
    int playing;
} mix_channel_t;

static mix_channel_t channels[MIX_CHANNELS];
static unsigned char mix_output[MIX_BUFFER_LEN];
static int sound_initialized = 0;

// ---- DMX Sound Lump Format ----
// Bytes 0-1: format (should be 0x0003)
// Bytes 2-3: sample rate
// Bytes 4-7: number of samples
// Bytes 8+:  raw 8-bit unsigned PCM data

static snddevice_t sound_devices[] = { SNDDEVICE_SB };

// ---- Sound Module Functions ----

static boolean DG_SND_Init(boolean use_sfx_prefix) {
    (void)use_sfx_prefix;
    write_serial_string("[DOOM_SND] Init sound module\n");
    
    for (int i = 0; i < MIX_CHANNELS; i++) {
        channels[i].playing = 0;
        channels[i].data = 0;
    }
    
    // Fill silence
    for (int i = 0; i < MIX_BUFFER_LEN; i++) mix_output[i] = 128;
    
    // Start SB16 continuous playback with our mix buffer
    sb16_set_audio_buffer(mix_output, MIX_BUFFER_LEN);
    sb16_start_playback(MIX_RATE);
    
    sound_initialized = 1;
    write_serial_string("[DOOM_SND] Sound module ready.\n");
    return true;
}

static void DG_SND_Shutdown(void) {
    sound_initialized = 0;
    write_serial_string("[DOOM_SND] Shutdown\n");
}

static int DG_SND_GetSfxLumpNum(sfxinfo_t *sfx) {
    char buf[12] = "DS";
    // Copy sfx name after "DS" prefix
    for (int i = 0; i < 6 && sfx->name[i]; i++) {
        buf[2 + i] = sfx->name[i];
        buf[3 + i] = '\0';
    }
    
    int lump = W_CheckNumForName(buf);
    return lump;
}

static int DG_SND_StartSound(sfxinfo_t *sfx, int channel, int vol, int sep) {
    if (!sound_initialized) return -1;
    if (channel < 0 || channel >= MIX_CHANNELS) return -1;
    
    int lump = sfx->lumpnum;
    if (lump < 0) return -1;
    
    unsigned char* lumpdata = (unsigned char*)W_CacheLumpNum(lump, 8); // PU_STATIC = 8 in zone tags
    if (!lumpdata) return -1;
    
    // Parse DMX header
    int format = lumpdata[0] | (lumpdata[1] << 8);
    // int rate   = lumpdata[2] | (lumpdata[3] << 8);
    int length = lumpdata[4] | (lumpdata[5] << 8) | (lumpdata[6] << 16) | (lumpdata[7] << 24);
    
    if (format != 3 || length <= 32) {
        return -1; // Not a valid DMX sound
    }
    
    // Skip the 16-byte padding at start and end of DMX data
    channels[channel].data    = lumpdata + 24;  // Skip header (8) + padding (16)
    channels[channel].length  = length - 32;     // Remove start+end padding
    channels[channel].pos     = 0;
    channels[channel].vol     = vol;
    channels[channel].sep     = sep;
    channels[channel].playing = 1;
    
    return channel;
}

static void DG_SND_StopSound(int channel) {
    if (channel >= 0 && channel < MIX_CHANNELS) {
        channels[channel].playing = 0;
    }
}

static boolean DG_SND_IsPlaying(int channel) {
    if (channel >= 0 && channel < MIX_CHANNELS) {
        return channels[channel].playing;
    }
    return false;
}

static void DG_SND_UpdateParams(int channel, int vol, int sep) {
    if (channel >= 0 && channel < MIX_CHANNELS) {
        channels[channel].vol = vol;
        channels[channel].sep = sep;
    }
}

static void DG_SND_Update(void) {
    if (!sound_initialized) return;
    
    // Mix all active channels into mix_output
    for (int i = 0; i < MIX_BUFFER_LEN; i++) {
        int mixed = 0;
        int active = 0;
        
        for (int ch = 0; ch < MIX_CHANNELS; ch++) {
            if (!channels[ch].playing) continue;
            
            int sample = (int)channels[ch].data[channels[ch].pos] - 128; // Convert to signed
            sample = (sample * channels[ch].vol) / 127; // Apply volume
            mixed += sample;
            active++;
            
            channels[ch].pos++;
            if (channels[ch].pos >= channels[ch].length) {
                channels[ch].playing = 0; // Done
            }
        }
        
        // Clamp and convert back to unsigned
        if (mixed > 127) mixed = 127;
        if (mixed < -128) mixed = -128;
        mix_output[i] = (unsigned char)(mixed + 128);
    }
    
    // Feed updated buffer to SB16
    sb16_set_audio_buffer(mix_output, MIX_BUFFER_LEN);
}

static void DG_SND_CacheSounds(sfxinfo_t *sounds, int num_sounds) {
    (void)sounds;
    (void)num_sounds;
}

// ---- Exported Sound Module ----
sound_module_t DG_sound_module = {
    sound_devices,
    sizeof(sound_devices) / sizeof(sound_devices[0]),
    DG_SND_Init,
    DG_SND_Shutdown,
    DG_SND_GetSfxLumpNum,
    DG_SND_Update,
    DG_SND_UpdateParams,
    DG_SND_StartSound,
    DG_SND_StopSound,
    DG_SND_IsPlaying,
    DG_SND_CacheSounds,
};

// ---- Music Module (Stub) ----
// DOOM expects DG_music_module but we don't support MUS/MIDI yet.
static snddevice_t music_devices[] = { SNDDEVICE_SB };
static boolean DG_MUS_Init(void) { return false; }
static void DG_MUS_Shutdown(void) {}
static void DG_MUS_SetVol(int v) { (void)v; }
static void DG_MUS_Pause(void) {}
static void DG_MUS_Resume(void) {}
static void* DG_MUS_Register(void* d, int l) { (void)d; (void)l; return 0; }
static void DG_MUS_Unregister(void* h) { (void)h; }
static void DG_MUS_Play(void* h, boolean l) { (void)h; (void)l; }
static void DG_MUS_Stop(void) {}
static boolean DG_MUS_IsPlaying(void) { return false; }

music_module_t DG_music_module = {
    music_devices,
    sizeof(music_devices) / sizeof(music_devices[0]),
    DG_MUS_Init,
    DG_MUS_Shutdown,
    DG_MUS_SetVol,
    DG_MUS_Pause,
    DG_MUS_Resume,
    DG_MUS_Register,
    DG_MUS_Unregister,
    DG_MUS_Play,
    DG_MUS_Stop,
    DG_MUS_IsPlaying,
    NULL, // Poll
};
