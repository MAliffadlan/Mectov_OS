#include "../include/apps.h"
#include "../include/vga.h"
#include "../include/wm.h"
#include "../include/utils.h"
#include "../include/security.h"

int power_win_id = -1;
int power_open = 0;

// Flag for overlay – checked in kernel.c full_redraw()
volatile int power_overlay_active = 0;

// Button dimensions (relative to window content area)
#define BTN_W 200
#define BTN_H 32
#define BTN_GAP 8

static void power_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    // Sleek dark background
    draw_rounded_rect(cx, cy, cw, ch, 10, 0x00151515);
    draw_rounded_rect_border(cx, cy, cw, ch, 10, 0x00333333);

    // Title
    draw_string_px(cx + (cw - 5*8)/2, cy + 12, "Power", 0x00AAAAAA, 0xFFFFFFFF);

    // ---- Logout Button (Sleek dark grey) ----
    int bx = cx + (cw - BTN_W) / 2;
    int by = cy + 38;
    draw_rounded_rect(bx, by, BTN_W, BTN_H, 6, 0x00222222);
    draw_rounded_rect_border(bx, by, BTN_W, BTN_H, 6, 0x00333333);
    draw_string_px(bx + 40, by + 8, "Log out", 0x00DDDDDD, 0xFFFFFFFF);
    // Logout icon (door with arrow)
    draw_rect(bx + 14, by + 8, 8, 16, 0x00888888);
    draw_rect(bx + 16, by + 10, 4, 12, 0x00222222);
    draw_line(bx + 26, by + 16, bx + 22, by + 16, 0x00888888);
    draw_line(bx + 24, by + 14, bx + 26, by + 16, 0x00888888);
    draw_line(bx + 24, by + 18, bx + 26, by + 16, 0x00888888);

    // ---- Restart Button (Sleek dark grey) ----
    by += BTN_H + BTN_GAP;
    draw_rounded_rect(bx, by, BTN_W, BTN_H, 6, 0x00222222);
    draw_rounded_rect_border(bx, by, BTN_W, BTN_H, 6, 0x00333333);
    draw_string_px(bx + 40, by + 8, "Restart", 0x00DDDDDD, 0xFFFFFFFF);
    // Restart icon (circular arrow)
    draw_circle(bx + 18, by + 16, 7, 0x00888888);
    draw_circle(bx + 18, by + 16, 6, 0x00888888);
    draw_rect(bx + 16, by + 8, 5, 5, 0x00222222); // Break the circle
    draw_line(bx + 21, by + 9, bx + 18, by + 12, 0x00888888);
    draw_line(bx + 21, by + 15, bx + 18, by + 12, 0x00888888);

    // ---- Shutdown Button (Darker Red Accent) ----
    by += BTN_H + BTN_GAP;
    draw_rounded_rect(bx, by, BTN_W, BTN_H, 6, 0x00331111);
    draw_rounded_rect_border(bx, by, BTN_W, BTN_H, 6, 0x00552222);
    draw_string_px(bx + 40, by + 8, "Shut Down", 0x00FF6666, 0xFFFFFFFF);
    // Shutdown icon (power symbol)
    draw_circle(bx + 18, by + 16, 7, 0x00FF6666);
    draw_circle(bx + 18, by + 16, 6, 0x00FF6666);
    draw_rect(bx + 15, by + 8, 7, 6, 0x00331111); // Break top
    draw_rect(bx + 17, by + 8, 2, 7, 0x00FF6666); // Line
}

static void power_key(int id, char c, uint8_t sc) {
    (void)id; (void)c;
    if (sc == 0x01) { // ESC key
        wm_close(power_win_id);
        power_open = 0;
        power_win_id = -1;
        power_overlay_active = 0;
    }
}

static void power_mouse(int id, int cx, int cy, int btn) {
    (void)id;
    if (btn == 1) {
        int wx = (fb_width - 240) / 2;
        int wy = (fb_height - 160) / 2;
        int content_x = cx;  // relative to window content area
        int content_y = cy;

        int bx = (240 - BTN_W) / 2;
        int by_start = 38;  // Must match power_draw offset

        // Hit test each button (all coordinates relative to content area)
        if (content_x >= bx && content_x <= bx + BTN_W) {
            // Logout
            if (content_y >= by_start && content_y <= by_start + BTN_H) {
                wm_close(power_win_id);
                power_open = 0;
                power_win_id = -1;
                power_overlay_active = 0;
                extern volatile int pending_logout;
                pending_logout = 1;
                return;
            }
            // Restart
            if (content_y >= by_start + BTN_H + BTN_GAP &&
                content_y <= by_start + BTN_H + BTN_GAP + BTN_H) {
                power_overlay_active = 0;
                wm_close(power_win_id);
                power_open = 0;
                power_win_id = -1;
                reboot();
                return;
            }
            // Shutdown
            if (content_y >= by_start + 2*(BTN_H + BTN_GAP) &&
                content_y <= by_start + 2*(BTN_H + BTN_GAP) + BTN_H) {
                power_overlay_active = 0;
                wm_close(power_win_id);
                power_open = 0;
                power_win_id = -1;
                shutdown();
                return;
            }
        }

        // Click outside buttons = close dialog (clicked on overlay area inside window)
        // We'll close only if click is outside the dialog box area entirely
        // Actually the WM only delivers clicks within the client area, so
        // if click doesn't hit any button, treat as outside-dialog dismiss.
        if (content_x < 0 || content_x >= 240 || content_y < 0 || content_y >= 160 ||
            (content_x >= bx && content_x <= bx + BTN_W &&
             content_y >= by_start && content_y <= by_start + 3*BTN_H + 2*BTN_GAP)) {
            // Click inside dialog area but not on a button — keep open
        } else {
            // Outside dialog area
            wm_close(power_win_id);
            power_open = 0;
            power_win_id = -1;
            power_overlay_active = 0;
        }
    }
}

static void power_tick(int id) {
    if (!wm_is_open(id)) {
        power_open = 0;
        power_win_id = -1;
        power_overlay_active = 0;
    }
}

void open_power_dialog() {
    if (power_open && wm_is_open(power_win_id)) { wm_raise(power_win_id); return; }

    power_overlay_active = 1;

    int dw = 240, dh = 160;
    power_win_id = wm_open((fb_width - dw) / 2, (fb_height - dh) / 2, dw, dh,
                           "Power Options",
                           power_draw, power_key, power_tick, power_mouse);
    power_open = (power_win_id >= 0);
}

// Legacy – keep for compatibility
void open_power_app() {
    open_power_dialog();
}