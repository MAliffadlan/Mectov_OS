#include "../include/apps.h"
#include "../include/vga.h"
#include "../include/wm.h"
#include "../include/utils.h"
#include "../include/security.h"

int power_win_id = -1;
int power_open = 0;

static void power_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    draw_rect(cx, cy, cw, ch, GUI_BG); // Clear

    draw_string_px(cx + 36, cy + 10, "Select Power Option:", GUI_TEXT, GUI_BG);

    // Shutdown Button (Red)
    draw_rect(cx + 40, cy + 30, 120, 24, 0x00CC0000);
    draw_rect_border(cx + 40, cy + 30, 120, 24, GUI_BORDER);
    draw_string_px(cx + 64, cy + 34, "Shut Down", GUI_WHITE, 0x00CC0000);

    // Restart Button (Orange)
    draw_rect(cx + 40, cy + 60, 120, 24, 0x00DD6600);
    draw_rect_border(cx + 40, cy + 60, 120, 24, GUI_BORDER);
    draw_string_px(cx + 70, cy + 64, "Restart", GUI_WHITE, 0x00DD6600);

    // Lock Button (Blue)
    draw_rect(cx + 40, cy + 90, 120, 24, 0x000066CC);
    draw_rect_border(cx + 40, cy + 90, 120, 24, GUI_BORDER);
    draw_string_px(cx + 60, cy + 94, "Lock Screen", GUI_WHITE, 0x000066CC);
}

static void power_key(int id, char c, uint8_t sc) {
    (void)id; (void)c; (void)sc;
}

static void power_mouse(int id, int cx, int cy, int btn) {
    (void)id;
    if (btn == 1) { // Left click
        if (cx >= 40 && cx <= 160) {
            if (cy >= 30 && cy <= 54) {
                shutdown();
            } else if (cy >= 60 && cy <= 84) {
                reboot();
            } else if (cy >= 90 && cy <= 114) {
                wm_close(power_win_id);
                lock_screen();
            }
        }
    }
}

static void power_tick(int id) {
    if (!wm_is_open(id)) {
        power_open = 0;
        power_win_id = -1;
    }
}

void open_power_app() {
    if (power_open && wm_is_open(power_win_id)) { wm_raise(power_win_id); return; }
    power_win_id = wm_open((fb_width - 200) / 2, (fb_height - 150) / 2, 200, 150, "Power Options", power_draw, power_key, power_tick, power_mouse);
    power_open = (power_win_id >= 0);
}
