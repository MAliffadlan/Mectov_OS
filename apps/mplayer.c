#include "lib/libc.h"

// Define event struct
typedef struct {
    int type;
    int x, y;
    int key;
} gui_event_t;

// Globals
void** __mct_lib_ptr;

// Colors (Catppuccin Mocha)
#define COLOR_BG      0x001E1E2E
#define COLOR_SURFACE 0x00313244
#define COLOR_ACCENT  0x0089B4FA
#define COLOR_TEXT    0x00CDD6F4
#define COLOR_SUBTEXT 0x00A6ADC8
#define COLOR_PLAY    0x00A6E3A1
#define COLOR_STOP    0x00F38BA8

// Simple tone buffer (generated in BSS, sent to SB16)
// 11025 Hz, 1 second = 11025 bytes
#define TONE_RATE  11025
#define TONE_SIZE  11025
static uint8_t tone_buf[TONE_SIZE];

static void generate_tone(int freq) {
    // Generate 8-bit unsigned PCM sine approximation
    // Using a simple triangle wave (no sin() available)
    int period = TONE_RATE / freq;
    if (period < 2) period = 2;
    int half = period / 2;
    for (int i = 0; i < TONE_SIZE; i++) {
        int pos = i % period;
        if (pos < half) {
            tone_buf[i] = 128 + (pos * 127 / half);
        } else {
            tone_buf[i] = 255 - ((pos - half) * 127 / half);
        }
    }
}

static void draw_visualizer(int wid, int x, int y, int w, int h) {
    static int bars[20] = {0};
    for (int i = 0; i < 20; i++) {
        if (bars[i] == 0) bars[i] = (rand() % (h - 5)) + 5;
        else bars[i] += (rand() % 9) - 4;
        if (bars[i] < 3) bars[i] = 3;
        if (bars[i] > h) bars[i] = h;
        int bw = w / 20 - 2;
        sys_draw_rect(wid, x + i * (bw + 2), y + h - bars[i], bw, bars[i], COLOR_ACCENT);
    }
}

static void draw_player(int wid, int is_playing) {
    // 1. Background
    sys_draw_rect(wid, 0, 0, 320, 220, COLOR_BG);
    
    // 2. Header
    sys_draw_rect(wid, 0, 0, 320, 40, COLOR_SURFACE);
    sys_draw_text(wid, 20, 12, "Mectov Music", COLOR_TEXT);
    
    // 3. Album Art Placeholder
    sys_draw_rect(wid, 20, 60, 80, 80, COLOR_SURFACE);
    sys_draw_rect(wid, 35, 75, 50, 50, COLOR_ACCENT);
    
    // 4. Song Info
    sys_draw_text(wid, 110, 65, "Midnight City", COLOR_TEXT);
    sys_draw_text(wid, 110, 85, "M83 (Cover)", COLOR_SUBTEXT);
    sys_draw_text(wid, 110, 110, "11025 Hz Tone", 0x006C7086);

    // 5. Visualizer Area
    sys_draw_rect(wid, 20, 160, 280, 40, 0x0011111B);
    if (is_playing) {
        draw_visualizer(wid, 25, 165, 270, 30);
    } else {
        sys_draw_rect(wid, 25, 179, 270, 2, COLOR_SUBTEXT);
    }
    
    // 6. Controls
    sys_draw_rect(wid, 110, 125, 40, 24, is_playing ? 0x0045475A : COLOR_PLAY);
    sys_draw_text(wid, 122, 130, ">", is_playing ? COLOR_SUBTEXT : COLOR_BG);
    
    sys_draw_rect(wid, 160, 125, 40, 24, is_playing ? COLOR_STOP : 0x0045475A);
    sys_draw_text(wid, 172, 130, "[]", is_playing ? COLOR_BG : COLOR_SUBTEXT);

    sys_update_window(wid);
}

void _start() {
    __mct_lib_ptr = mct_load_library("apps/libc.mct");
    if (!__mct_lib_ptr) sys_exit();
    
    // Pre-generate a 440 Hz triangle wave tone
    generate_tone(440);
    
    int wid = sys_create_window(150, 150, 320, 220, "Mectov Media Player");
    if (wid < 0) sys_exit();

    int is_playing = 0;
    uint32_t last_vis = 0;
    
    draw_player(wid, is_playing);

    while (1) {
        gui_event_t ev;
        int has_event = sys_get_event(wid, &ev);
        if (has_event > 0) {
            if (ev.type == 1) { // Paint
                draw_player(wid, is_playing);
            } else if (ev.type == 2) { // Key
                if (ev.key == 'q' || ev.key == 27) break;
            } else if (ev.type == 3) { // Click
                // Play button
                if (ev.x >= 110 && ev.x <= 150 && ev.y >= 125 && ev.y <= 149) {
                    if (!is_playing) {
                        // Play generated tone via SB16 DMA
                        sys_play_wav(tone_buf, TONE_SIZE, TONE_RATE);
                        // sys_play_sound(440, 100);
                        is_playing = 1;
                    }
                }
                // Stop button
                else if (ev.x >= 160 && ev.x <= 200 && ev.y >= 125 && ev.y <= 149) {
                    if (is_playing) {
                        sys_stop_wav();
                        is_playing = 0;
                    }
                }
                draw_player(wid, is_playing);
            }
        }
        
        if (is_playing) {
            uint32_t t = sys_get_ticks();
            if (t > last_vis + 50) {
                draw_player(wid, is_playing);
                last_vis = t;
            }
        }
        
        sys_yield();
    }

    sys_exit();
}
