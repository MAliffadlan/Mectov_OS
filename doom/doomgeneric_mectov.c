/*
 * doomgeneric_mectov.c - DOOM Platform Layer for Mectov OS
 * Implements the 5 required doomgeneric functions.
 */
#include "doomgeneric.h"
#include "doomkeys.h"

/* Kernel font table (linked in from src/drivers/font8x16.c) */
#include "../src/include/font8x16.h"

/* Kernel API */
extern uint32_t* fb_addr;
extern uint32_t* back_buffer;
extern uint32_t  fb_width;
extern uint32_t  fb_height;
extern uint32_t  fb_pitch;
extern uint32_t  bb_pitch;

extern volatile uint32_t timer_ticks;  // 1000 Hz tick counter
extern uint8_t   k_get_scancode(void);
extern void      write_serial_string(const char *s);
extern void      write_serial_hex(uint32_t);

/* DOOM renders at 320x200 internally (or DOOMGENERIC_RESX x DOOMGENERIC_RESY) */
/* We need to scale it up to fit our framebuffer */

int doom_running = 0;
volatile int doom_fullscreen = 0; /* Global flag to disable cursor/desktop */
static int doom_initialized = 0;

/* Key buffer for passing keyboard events to DOOM */
#define KEY_QUEUE_SIZE 256
static struct {
    int pressed;
    unsigned char key;
} key_queue[KEY_QUEUE_SIZE];
static int key_queue_head = 0;
static int key_queue_tail = 0;

/* Track which scancodes are currently held (by base scancode 0x00-0x7F) */
static uint8_t key_held[128];

static void key_queue_push(int pressed, unsigned char key) {
    int next = (key_queue_head + 1) % KEY_QUEUE_SIZE;
    if (next != key_queue_tail) {
        key_queue[key_queue_head].pressed = pressed;
        key_queue[key_queue_head].key = key;
        key_queue_head = next;
    }
}

/* Map PS/2 scancode to DOOM key code
 * IMPORTANT: Must use DOOM's specific action key codes, not generic codes!
 * KEY_FIRE (0xa3), KEY_USE (0xa2), KEY_UPARROW (0xae), etc. */
static unsigned char scancode_to_doomkey(uint8_t sc) {
    uint8_t code = sc & 0x7F;  /* Strip release bit */
    
    switch (code) {
        /* Arrow keys — movement/turning */
        case 0x48: return KEY_UPARROW;      /* Up arrow = Forward */
        case 0x50: return KEY_DOWNARROW;     /* Down arrow = Backward */
        case 0x4B: return KEY_LEFTARROW;     /* Left arrow = Turn left */
        case 0x4D: return KEY_RIGHTARROW;    /* Right arrow = Turn right */
        
        /* Action keys — use DOOM's specific key codes! */
        case 0x1D: return KEY_FIRE;          /* Left Ctrl = FIRE */
        case 0x38: return KEY_RALT;          /* Alt = Strafe modifier */
        case 0x39: return KEY_USE;           /* Space = USE/Open door */
        case 0x2A: return KEY_RSHIFT;        /* Left Shift = Run */
        case 0x36: return KEY_RSHIFT;        /* Right Shift = Run */
        
        /* WASD — modern FPS controls */
        case 0x11: return KEY_UPARROW;       /* W = Forward */
        case 0x1F: return KEY_DOWNARROW;     /* S = Backward */
        case 0x1E: return KEY_STRAFE_L;      /* A = Strafe left */
        case 0x20: return KEY_STRAFE_R;      /* D = Strafe right */
        
        /* Extra fire/use on convenient keys */
        case 0x12: return KEY_FIRE;          /* E = Fire (alternative) */
        case 0x21: return KEY_USE;           /* F = Use (alternative) */
        
        /* Menu / system keys */
        case 0x01: return KEY_ESCAPE;        /* ESC = Menu */
        case 0x1C: return KEY_ENTER;         /* Enter */
        case 0x0F: return KEY_TAB;           /* Tab = Automap */
        case 0x0E: return KEY_BACKSPACE;     /* Backspace */
        
        /* Number keys 1-9 for weapon selection */
        case 0x02: return '1';
        case 0x03: return '2';
        case 0x04: return '3';
        case 0x05: return '4';
        case 0x06: return '5';
        case 0x07: return '6';
        case 0x08: return '7';
        case 0x09: return '8';
        case 0x0A: return '9';
        
        /* F keys */
        case 0x3B: return KEY_F1;
        case 0x3C: return KEY_F2;
        case 0x3D: return KEY_F3;
        case 0x3E: return KEY_F4;
        case 0x3F: return KEY_F5;
        case 0x40: return KEY_F6;
        case 0x41: return KEY_F7;
        case 0x42: return KEY_F8;
        case 0x43: return KEY_F9;
        case 0x44: return KEY_F10;
        case 0x57: return KEY_F11;
        case 0x58: return KEY_F12;
        
        /* Misc keys */
        case 0x0C: return KEY_MINUS;         /* - = Shrink screen */
        case 0x0D: return KEY_EQUALS;        /* = = Grow screen */
        case 0x19: return 'p';               /* P */
        case 0x32: return 'm';               /* M */
        case 0x15: return 'y';               /* Y = Confirm */
        case 0x31: return 'n';               /* N = Deny */
        case 0x14: return 't';               /* T = Chat */
        
        default: return 0;
    }
}

/* Poll keyboard — tracks key state to filter typematic auto-repeat.
 * Only queues ACTUAL state changes (press→release or release→press).
 * PS/2 keyboards send repeated press scancodes while a key is held;
 * without filtering, these flood the queue and cause "stuck keys". */
static void doom_poll_keyboard(void) {
    /* Drain the ENTIRE keyboard buffer */
    while (1) {
        uint8_t sc = k_get_scancode();
        if (sc == 0) break;
        
        uint8_t base = sc & 0x7F;
        int pressed = !(sc & 0x80);
        
        /* Filter: skip if state hasn't changed (typematic repeat) */
        if (pressed && key_held[base]) continue;  /* Already held — skip repeat */
        if (!pressed && !key_held[base]) continue; /* Already released — skip */
        
        /* Update state */
        key_held[base] = pressed ? 1 : 0;
        
        unsigned char dkey = scancode_to_doomkey(sc);
        if (dkey == 0) continue;
        
        key_queue_push(pressed, dkey);
    }
}

/* ===== DG_Init ===== */
void DG_Init(void) {
    doom_running = 1;
    write_serial_string("[DOOM] DG_Init: Mectov OS platform initialized\n");
}

/* ===== Doom heartbeat overlay =====
 * Frame counter + uptime drawn directly to the front framebuffer after each
 * blit. Lets the user distinguish a frozen QEMU window (display/audio main
 * loop stalled while the guest keeps running) from a dead OS: if the counter
 * advances, the game loop is alive; if it is static while the serial log
 * still grows, the window is stale.
 */
static uint32_t doom_frame_counter = 0;

static uint32_t pow10(int e) {
    uint32_t r = 1;
    for (int i = 0; i < e; i++) r *= 10;
    return r;
}

static void doom_draw_heartbeat(void) {
    if (!fb_addr) return;

    uint32_t uptime = timer_ticks / 1000;
    uint32_t fc = doom_frame_counter;

    /* Build "Fdddddd Uddddd" (frame, 6 digits; uptime, 5 digits ~27h) */
    char buf[16];
    int n = 0;
    buf[n++] = 'F';
    for (int d = 5; d >= 0; d--) { buf[n++] = (char)('0' + (fc / pow10(d)) % 10); }
    buf[n++] = ' ';
    buf[n++] = 'U';
    for (int d = 4; d >= 0; d--) { buf[n++] = (char)('0' + (uptime / pow10(d)) % 10); }

    int chars = n;
    int bw = chars * 8 + 8;  /* box width  */
    int bx = (int)fb_width - bw - 6;
    int by = 4;
    /* Raw write with no clipping — refuse tiny framebuffers to keep bx >= 0 */
    if (bx < 0 || (int)fb_height < 24) return;
    uint32_t stride = fb_pitch / 4;
    uint32_t *fb = fb_addr;

    /* Dark box + hairline border */
    for (int y = 0; y < 20; y++) {
        for (int x = 0; x < bw; x++) {
            uint32_t col = (y < 1 || y >= 19 || x < 1 || x >= bw - 1) ? 0x00333333 : 0x00000000;
            fb[(by + y) * stride + (bx + x)] = col;
        }
    }

    /* Green digits */
    for (int i = 0; i < chars; i++) {
        unsigned char c = (unsigned char)buf[i];
        int gx = bx + 4 + i * 8;
        for (int j = 0; j < 16; j++) {
            unsigned char row = font8x16_data[c][j];
            for (int k = 0; k < 8; k++) {
                if (row & (0x80 >> k))
                    fb[(by + 2 + j) * stride + (gx + k)] = 0x0000FF00;
            }
        }
    }

    /* Blinking status dot (toggles every 8 frames) */
    int on = (fc / 8) & 1;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            uint32_t col = on ? 0x00FF4400 : 0x00443300;
            fb[(by + 8 + y) * stride + (bx + bw - 6 + x)] = col;
        }
}

/* ===== DG_DrawFrame ===== */
/* Serial heartbeat: emits one line every DOOM_TICK_EVERY frames so the
 * serial log can tell a frozen game loop apart from a frozen QEMU window.
 * If [DOOM] tick lines stop while kernel [LOAD] lines continue, the doom
 * task is stuck; if they keep going while the screen looks frozen, it is
 * the QEMU display/audio side. */
#define DOOM_TICK_EVERY 600   /* ~17 s at 35 fps */
static uint32_t doom_tick_serial_counter = 0;

void DG_DrawFrame(void) {
    if (!DG_ScreenBuffer || !fb_addr) return;
    
    /* Poll keyboard every frame */
    doom_poll_keyboard();

    if (++doom_tick_serial_counter >= DOOM_TICK_EVERY) {
        doom_tick_serial_counter = 0;
        write_serial_string("[DOOM] tick f=");
        write_serial_hex(doom_frame_counter);
        write_serial_string(" t=");
        write_serial_hex(timer_ticks / 1000);
        write_serial_string("\n");
    }
    
    /* FULLSCREEN NATIVE 1:1 BLIT (1024x768) */
    uint32_t src_w = DOOMGENERIC_RESX;
    uint32_t src_h = DOOMGENERIC_RESY;
    
    /* Bypass back_buffer during fullscreen to avoid being blocked by swap_buffers() */
    uint32_t *dst = fb_addr; 
    uint32_t dst_pitch_px = fb_pitch / 4;
    
    for (uint32_t y = 0; y < src_h; y++) {
        uint32_t *src_row = (uint32_t*)DG_ScreenBuffer + (y * src_w);
        uint32_t *dst_row = dst + (y * dst_pitch_px);
        /* Fast blit via memcpy */
        memcpy(dst_row, src_row, src_w * 4);
    }

    /* Heartbeat overlay on top of the frame */
    doom_frame_counter++;
    doom_draw_heartbeat();
}

/* ===== DG_SleepMs ===== */
void DG_SleepMs(uint32_t ms) {
    uint32_t target = timer_ticks + ms;
    while (timer_ticks < target) {
        /* Poll keyboard while waiting */
        doom_poll_keyboard();
        __asm__ volatile("hlt");
    }
}

/* ===== DG_GetTicksMs ===== */
uint32_t DG_GetTicksMs(void) {
    return timer_ticks;
}

/* ===== DG_GetKey ===== */
int DG_GetKey(int *pressed, unsigned char *key) {
    /* Also poll latest keys */
    doom_poll_keyboard();
    
    if (key_queue_tail == key_queue_head) return 0;
    
    *pressed = key_queue[key_queue_tail].pressed;
    *key = key_queue[key_queue_tail].key;
    key_queue_tail = (key_queue_tail + 1) % KEY_QUEUE_SIZE;
    return 1;
}

/* ===== DG_SetWindowTitle ===== */
void DG_SetWindowTitle(const char *title) {
    write_serial_string("[DOOM] Title: ");
    write_serial_string(title);
    write_serial_string("\n");
}

/* ===== Entry point called from shell/desktop ===== */
/* Default 0 = DOOM runs silent. The shell sets this to 1 when the user
 * types `doom -sound`. SB16 DMA + IRQ activity while DOOM streams audio is
 * what froze the display on some hosts (guest stays alive, QEMU window
 * stalls), so sound is opt-in; without it the SB16 module is never even
 * registered, which also removes the harmless 'd_ not found' music spam.
 */
int doom_sound_enabled = 0;

void doom_start(void) {
    /* The shell sets doom_sound_enabled right before calling us. Snapshot it
     * into a local, then self-clear so the flag can never leak into the next
     * launch if doom_start is reached through any other path (the resume path
     * skips doomgeneric_Create, so a stale flag would be silently ignored
     * there anyway — and wrongly applied if it ever ran Create). */
    int snd = doom_sound_enabled;
    doom_sound_enabled = 0;
    write_serial_string("[DOOM] Starting DOOM on Mectov OS...\n");
    
    /* Hide desktop cursor during DOOM */
    doom_fullscreen = 1;
    
    /* CRITICAL: Enable interrupts! We're called from a syscall handler
     * where interrupts are disabled. Without this, timer_ticks won't
     * advance and keyboard won't work. */
    __asm__ volatile("sti");
    
    /* Fake argc/argv for DOOM — point to WAD file. 5 slots so the
     * optional -nosound arg + terminator never write past the array. */
    char *argv[5] = { "doom", "-iwad", "doom1.wad", NULL, NULL };
    int argc = 3;
    if (!snd) argv[argc++] = "-nosound";   /* silent by default */
    argv[argc] = NULL;
    
    /* Initialize DOOM only once to prevent crashes on relaunch */
    if (!doom_initialized) {
        doom_initialized = 1;
        doomgeneric_Create(argc, argv);
    } else {
        /* Resume from where we left off (e.g. Menu) */
        doom_running = 1;
    }
    
    write_serial_string("[DOOM] Entering game loop...\n");
    
    /* Main loop */
    while (doom_running) {
        doomgeneric_Tick();
    }
    
    write_serial_string("[DOOM] DOOM exited to Mectov OS\n");
    doom_fullscreen = 0; /* Restore desktop rendering */
    
    extern int cursor_saved_x;
    cursor_saved_x = -1;
    
    extern void vga_force_sync(void);
    vga_force_sync();
    
    extern void full_redraw(void);
    full_redraw();
}
