/*
 * doomgeneric_mectov.c - DOOM Platform Layer for Mectov OS
 * Implements the 5 required doomgeneric functions.
 */
#include "doomgeneric.h"
#include "doomkeys.h"

/* Kernel font table (linked in from src/drivers/font8x16.c) */
#include "../src/include/font8x16.h"

/* Window manager (windowed mode, v38.29): wm_open/wm_close/wm_is_open,
 * WmWin, wm_focused — plus the layout constants TITLEBAR_H/TASKBAR_H_PX. */
#include "../src/include/wm.h"
#include "../src/include/theme.h"

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

/* DOOM renders at 320x200 internally; DG_ScreenBuffer is the platform
 * framebuffer (DOOMGENERIC_RESX x DOOMGENERIC_RESY, 640x400 = 2x). The
 * compositor scales that into our WM window in windowed mode, or it is
 * blitted 1:1 to the physical framebuffer in legacy fullscreen mode. */

volatile int doom_running = 0;
volatile int doom_fullscreen = 0; /* Global flag to disable cursor/desktop */

/* Windowed mode (v38.29): when 1 (set by the `doom` shell command, cleared
 * when it is `doom -fullscreen`), DOOM renders into a normal WM window like
 * any other desktop app instead of grabbing the whole framebuffer. */
int doom_windowed = 0;

/* The WM window id DOOM owns while windowed (0 = none open). */
static int doom_win_id = 0;

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
    /* Windowed mode makes the queue cross-task: the main loop pushes
     * (doom_handle_scancode) while the DOOM task pops (DG_GetKey). gui_lock
     * is the shared GUI spinlock — same one the window syscalls use. In
     * legacy fullscreen mode the queue stays single-threaded and the lock is
     * just a tiny no-contention overhead. */
    extern uint32_t gui_lock(void);
    extern void gui_unlock(uint32_t eflags);
    uint32_t g_ef = gui_lock();
    int next = (key_queue_head + 1) % KEY_QUEUE_SIZE;
    if (next != key_queue_tail) {
        key_queue[key_queue_head].pressed = pressed;
        key_queue[key_queue_head].key = key;
        key_queue_head = next;
    }
    gui_unlock(g_ef);
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
 * without filtering, these flood the queue and cause "stuck keys".
 *
 * Windowed mode: the main loop is the sole keyboard consumer (single
 * consumer, v38.9) and forwards every scancode to doom_handle_scancode()
 * while our window holds focus — polling the raw buffer here would race
 * with it and steal keys from the focused desktop window. Legacy
 * fullscreen mode still polls directly because the main loop skips the
 * keyboard entirely there. */
static void doom_poll_keyboard(void) {
    if (doom_windowed) return;

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

/* ===== Windowed-mode glue (v38.29) =====
 * Called from the main loop (kernel.c) while the DOOM window holds focus.
 * Mirrors the press/release + typematic filtering of doom_poll_keyboard(),
 * but for ONE scancode routed through the WM instead of a raw buffer poll.
 */
void doom_handle_scancode(uint8_t sc) {
    if (sc == 0) return;
    uint8_t base = sc & 0x7F;
    int pressed = !(sc & 0x80);
    /* Filter: skip if state hasn't changed (typematic repeat / stale release) */
    if (pressed && key_held[base]) return;
    if (!pressed && !key_held[base]) return;
    key_held[base] = pressed ? 1 : 0;
    unsigned char dkey = scancode_to_doomkey(sc);
    if (dkey == 0) return;
    key_queue_push(pressed, dkey);
}

/* True while the DOOM window is open AND the WM has focus on it — i.e. the
 * exact condition under which the main loop should route keys to DOOM. */
int doom_window_has_focus(void) {
    extern int wm_focused;
    return doom_windowed && doom_win_id > 0 && wm_focused == doom_win_id;
}

/* ===== DG_Init ===== */
void DG_Init(void) {
    doom_running = 1;
    write_serial_string("[DOOM] DG_Init: Mectov OS platform initialized\n");
}

/* ===== Doom heartbeat overlay =====
 * Frame counter + uptime drawn into the target buffer after each blit
 * (top-right corner). Lets the user distinguish a frozen QEMU window
 * (display/audio main loop stalled while the guest keeps running) from a
 * dead OS: if the counter advances, the game loop is alive; if it is static
 * while the serial log still grows, the window is stale. In fullscreen mode
 * the target is the physical framebuffer; in windowed mode it is the DOOM
 * window's content buffer.
 */
static uint32_t doom_frame_counter = 0;

static uint32_t pow10(int e) {
    uint32_t r = 1;
    for (int i = 0; i < e; i++) r *= 10;
    return r;
}

static void doom_draw_heartbeat_buf(uint32_t* tgt, int scr_w, int scr_h, int stride) {
    if (!tgt) return;

    uint32_t uptime = timer_ticks / 1000;
    uint32_t fc = doom_frame_counter;

    /* Build "Fdddddd Uddddd" (frame, 6 digits; uptime, 5 digits ~27h) */
    char text[16];
    int n = 0;
    text[n++] = 'F';
    for (int d = 5; d >= 0; d--) { text[n++] = (char)('0' + (fc / pow10(d)) % 10); }
    text[n++] = ' ';
    text[n++] = 'U';
    for (int d = 4; d >= 0; d--) { text[n++] = (char)('0' + (uptime / pow10(d)) % 10); }

    int chars = n;
    int bw = chars * 8 + 8;  /* box width  */
    int bx = scr_w - bw - 6;
    int by = 4;
    /* Raw write with no clipping — refuse tiny targets to keep bx >= 0 */
    if (bx < 0 || scr_h < 24) return;

    /* Dark box + hairline border */
    for (int y = 0; y < 20; y++) {
        for (int x = 0; x < bw; x++) {
            uint32_t col = (y < 1 || y >= 19 || x < 1 || x >= bw - 1) ? 0x00333333 : 0x00000000;
            tgt[(by + y) * stride + (bx + x)] = col;
        }
    }

    /* Green digits */
    for (int i = 0; i < chars; i++) {
        unsigned char c = (unsigned char)text[i];
        int gx = bx + 4 + i * 8;
        for (int j = 0; j < 16; j++) {
            unsigned char row = font8x16_data[c][j];
            for (int k = 0; k < 8; k++) {
                if (row & (0x80 >> k))
                    tgt[(by + 2 + j) * stride + (gx + k)] = 0x0000FF00;
            }
        }
    }

    /* Blinking status dot (toggles every 8 frames) */
    int on = (fc / 8) & 1;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            uint32_t col = on ? 0x00FF4400 : 0x00443300;
            tgt[(by + 8 + y) * stride + (bx + bw - 6 + x)] = col;
        }
}

static void doom_draw_heartbeat(void) {
    if (!fb_addr) return;
    doom_draw_heartbeat_buf(fb_addr, (int)fb_width, (int)fb_height, (int)(fb_pitch / 4));
}

/* ===== doom_win_draw — WM window render callback =====
 * The compositor (wm.c draw_one) calls this on the BSP whenever the DOOM
 * window's buffer is dirty, with the render target set to the window's
 * content buffer. We integer-scale DG_ScreenBuffer into that buffer and
 * stamp the heartbeat overlay on top; draw_one then composites the buffer
 * to the desktop back buffer like any other window. Integer scaling keeps
 * the 320x200-era look chunky instead of blurry, and any window size works
 * (drag to resize — DOOM stays centered).
 */
static void doom_win_draw(int id, int cx, int cy, int cw, int ch) {
    (void)cx; (void)cy;
    if (!DG_ScreenBuffer || cw <= 0 || ch <= 0) return;
    extern int get_win_index(int wid);
    int idx = get_win_index(id);
    if (idx < 0) return;
    uint32_t* buf = wm_wins[idx].content_buffer;
    if (!buf || wm_wins[idx].resizing) return;

    /* Largest integer scale that fits the content area */
    int scale = cw / (int)DOOMGENERIC_RESX;
    int s2 = ch / (int)DOOMGENERIC_RESY;
    if (s2 < scale) scale = s2;
    if (scale < 1) scale = 1;

    int dw = (int)DOOMGENERIC_RESX * scale;
    int dh = (int)DOOMGENERIC_RESY * scale;
    int ox = (cw - dw) / 2; if (ox < 0) ox = 0;
    int oy = (ch - dh) / 2; if (oy < 0) oy = 0;
    int max_x = dw; if (ox + max_x > cw) max_x = cw - ox;
    int max_y = dh; if (oy + max_y > ch) max_y = ch - oy;

    uint32_t* src = (uint32_t*)DG_ScreenBuffer;
    for (int y = 0; y < max_y; y++) {
        uint32_t* srow = src + (y / scale) * DOOMGENERIC_RESX;
        uint32_t* drow = buf + (oy + y) * cw + ox;
        for (int x = 0; x < max_x; x++) {
            drow[x] = srow[x / scale];
        }
    }

    doom_draw_heartbeat_buf(buf, cw, ch, cw);
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
    
    /* Poll keyboard every frame (no-op in windowed mode — keys arrive via
     * doom_handle_scancode from the main loop while we have focus) */
    doom_poll_keyboard();

    if (++doom_tick_serial_counter >= DOOM_TICK_EVERY) {
        doom_tick_serial_counter = 0;
        write_serial_string("[DOOM] tick f=");
        write_serial_hex(doom_frame_counter);
        write_serial_string(" t=");
        write_serial_hex(timer_ticks / 1000);
        write_serial_string("\n");
    }

    if (doom_windowed) {
        /* Windowed (v38.29): hand the frame to the WM compositor instead of
         * blitting the framebuffer. Marking the window dirty makes the main
         * loop run doom_win_draw() on the next full_redraw(), which scales
         * DG_ScreenBuffer into the window's content buffer and composites
         * it onto the desktop like any other app. */
        doom_frame_counter++;
        if (doom_win_id <= 0 || !wm_is_open(doom_win_id)) {
            /* Window closed (X button) → leave the game loop. */
            doom_running = 0;
            return;
        }
        extern volatile int needs_redraw;
        wm_invalidate(doom_win_id);
        needs_redraw = 1;
        return;
    }
    
    /* FULLSCREEN NATIVE 1:1 BLIT */
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
    /* Also poll latest keys (no-op in windowed mode) */
    doom_poll_keyboard();
    
    /* Pop under the same lock key_queue_push uses: in windowed mode the main
     * loop is the producer, this task the consumer (SMP). */
    extern uint32_t gui_lock(void);
    extern void gui_unlock(uint32_t eflags);
    uint32_t g_ef = gui_lock();
    int avail = (key_queue_tail != key_queue_head);
    if (avail) {
        *pressed = key_queue[key_queue_tail].pressed;
        *key = key_queue[key_queue_tail].key;
        key_queue_tail = (key_queue_tail + 1) % KEY_QUEUE_SIZE;
    }
    gui_unlock(g_ef);
    return avail;
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

    if (doom_windowed) {
        /* Windowed (v38.29): open a normal WM window, like Snake/Flappy. The
         * desktop keeps rendering and routing input; each frame is handed to
         * the compositor via doom_win_draw(). Centered, 640x400 content + a
         * 1px frame and the titlebar. */
        extern uint32_t fb_width, fb_height;
        int ww = DOOMGENERIC_RESX + 2;
        int wh = DOOMGENERIC_RESY + TITLEBAR_H + 2;
        int wx = ((int)fb_width - ww) / 2;
        int wy = ((int)fb_height - TASKBAR_H_PX - wh) / 2;
        if (wx < 0) wx = 0;
        if (wy < 0) wy = 0;
        doom_win_id = wm_open(wx, wy, ww, wh, "DOOM",
                              doom_win_draw, NULL, NULL, NULL);
        write_serial_string("[DOOM] window id=");
        write_serial_hex((uint32_t)doom_win_id);
        write_serial_string("\n");
        if (doom_win_id < 0) {
            write_serial_string("[DOOM] failed to open a WM window\n");
            doom_windowed = 0;
            return;
        }
    } else {
        /* Hide desktop cursor during DOOM */
        doom_fullscreen = 1;
    }
    
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

    if (doom_windowed) {
        /* Close the window; wm_close marks its region dirty so the main
         * loop repaints the desktop underneath on the next frame. */
        if (doom_win_id > 0) {
            wm_close(doom_win_id);
            doom_win_id = 0;
        }
    } else {
        doom_fullscreen = 0; /* Restore desktop rendering */
        
        extern int cursor_saved_x;
        cursor_saved_x = -1;
        
        extern void vga_force_sync(void);
        vga_force_sync();
        
        extern void full_redraw(void);
        full_redraw();
    }
}
