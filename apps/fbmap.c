// fbmap.c — Ring 3 direct-framebuffer compositor demo.
//
// Phase 1 proved a Ring 3 task can own pixels (SYS_FB_MAP). Phase 2 proves
// the full takeover handshake:
//   * while this task holds the scanout, the kernel desktop stops
//     presenting frames entirely (kernel.c fb_active branch)
//   * keystrokes route to the holder via SYS_GET_KEY (main-loop pump)
//   * raw mouse state flows through SYS_GET_MOUSE as always
//   * SYS_FB_RELEASE hands the scanout back and the desktop repaints
//
// Draws an animated gradient + bouncing box for 120 frames (paint proof),
// then goes interactive: the box chases the mouse cursor and ANY key exits,
// releasing the scanout cleanly. A hard frame cap keeps automated runs
// bounded even if no key ever arrives.
//
// Run:  run /apps/fbmap.mct   (or launch from the Start menu)
#include "src/include/syscall.h"

static void wlog(const char* s) {
    int n = 0;
    while (s[n]) n++;
    syscall(SYS_WRITE, 2, (int)(uintptr_t)s, n);
}

static void put_px(fb_info_t* fb, int x, int y, uint32_t c) {
    if ((uint32_t)x >= fb->width || (uint32_t)y >= fb->height) return;
    if (fb->bpp == 32) {
        uint32_t* row = (uint32_t*)(fb->base + y * fb->pitch);
        row[x] = c;
    } else {
        // 24bpp: 3 bytes per pixel, rows are pitch-aligned
        uint8_t* p = (uint8_t*)(fb->base + y * fb->pitch + x * 3);
        p[0] = (uint8_t)(c & 0xFF);
        p[1] = (uint8_t)((c >> 8) & 0xFF);
        p[2] = (uint8_t)((c >> 16) & 0xFF);
    }
}

void _start(void) {
    wlog("FBMAP start\n");

    fb_info_t fb;
    if (sys_fb_map(&fb) != 0) {
        wlog("FBMAP FAIL: sys_fb_map rejected (active session? vbe?)\n");
        sys_exit();
        for (;;) ;
    }

    wlog("FBMAP mapped: kernel desktop suppressed, Ring 3 owns the scanout\n");

    // Bouncing box state
    int bx = (int)fb.width / 4, by = (int)fb.height / 4;
    int dx = 3, dy = 2;

    const int ANIM_FRAMES = 120;
    const int MAX_FRAMES  = 2400;   // hard cap: never hang a headless run

    int frame = 0;
    for (; frame < MAX_FRAMES; frame++) {
        // Full-screen horizontal gradient sweep (row-by-row).
        uint32_t t = (uint32_t)frame * 4;
        for (uint32_t y = 0; y < fb.height; y++) {
            uint8_t r = (uint8_t)((y * 255u) / fb.height);
            uint8_t g = (uint8_t)(((y + t) * 128u / fb.height) & 0xFF);
            for (uint32_t x = 0; x < fb.width; x++) {
                uint8_t b = (uint8_t)(((x + t) * 160u) / fb.width);
                put_px(&fb, (int)x, (int)y, ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
            }
        }

        // Animation phase: bounce. Interactive phase: chase the mouse.
        mouse_state_t m = sys_get_mouse();
        if (frame < ANIM_FRAMES) {
            bx += dx; by += dy;
            if (bx < 0 || bx + 60 > (int)fb.width)  { dx = -dx; bx += dx * 2; }
            if (by < 0 || by + 60 > (int)fb.height) { dy = -dy; by += dy * 2; }
        } else {
            if (frame == ANIM_FRAMES) {
                // Drain keys queued during the animation so a stray press
                // cannot exit us before the interactive phase began.
                while (sys_get_key() != 0) { }
                wlog("FBMAP INTERACTIVE: move mouse, any key releases\n");
            }
            bx += (m.x - 30 - bx) / 4;
            by += (m.y - 30 - by) / 4;
        }

        // The box
        for (int y = 0; y < 60; y++)
            for (int x = 0; x < 60; x++)
                put_px(&fb, bx + x, by + y, 0x00FFFFFF);

        // Mouse crosshair overlay
        for (int i = -6; i <= 6; i++) {
            put_px(&fb, m.x + i, m.y, 0x000000FF);
            put_px(&fb, m.x, m.y + i, 0x000000FF);
        }

        // Any key ends the takeover cleanly.
        if (frame >= ANIM_FRAMES && sys_get_key() != 0) break;
    }

    wlog(frame >= MAX_FRAMES ? "FBMAP cap hit\n" : "FBMAP key exit\n");
    if (sys_fb_release() == 0) {
        wlog("FBMAP RELEASED: desktop restored\n");
    } else {
        wlog("FBMAP FAIL: release rejected\n");
    }
    sys_exit();
    for (;;) ;
}
