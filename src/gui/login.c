#include "../include/login.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/utils.h"
#include "../include/speaker.h"
#include "../include/mouse.h"
#include "../include/timer.h"

static void draw_login(int pass_len, int shake, int err, int cap_lock) {
    if (!is_vbe || fb_width == 0 || fb_height == 0) return;

    // ----- Background: wallpaper -----
    extern uint32_t _binary_obj_wallpaper_bin_start[];
    uint32_t* wp_ptr = _binary_obj_wallpaper_bin_start;
    uint32_t wp_w = 1024;
    uint32_t wp_h = 768;
    uint32_t area_h = fb_height;
    uint32_t copy_w = (fb_width < wp_w) ? fb_width : wp_w;
    uint32_t copy_h = (area_h < wp_h) ? area_h : wp_h;
    for (uint32_t y = 0; y < copy_h; y++) {
        memcpy(&back_buffer[y * fb_width], &wp_ptr[y * wp_w], copy_w * 4);
    }
    // Fill leftover
    if (fb_width > wp_w) {
        draw_rect(wp_w, 0, fb_width - wp_w, area_h, 0x00111122);
    }
    if (area_h > wp_h) {
        draw_rect(0, wp_h, fb_width, area_h - wp_h, 0x00111122);
    }

    // Overlay dim (semi-transparent dark)
    draw_rect_alpha(0, 0, fb_width, fb_height, 0x88000000);

    // ----- Center panel (rounded, with shadow) -----
    int pw = 340, ph = 260;
    int shake_off = shake ? ((shake & 1) ? 8 : -8) : 0;
    int px = (int)(fb_width  - pw) / 2 + shake_off;
    int py = (int)(fb_height - ph) / 2;

    // Shadow
    draw_soft_shadow(px, py, pw, ph, 12, 220);

    // Panel background (rounded)
    draw_rounded_rect(px, py, pw, ph, WIN_RADIUS, GUI_BG);
    draw_rounded_rect_border(px, py, pw, ph, WIN_RADIUS, GUI_BORDER);

    // ----- Header gradient -----
    draw_gradient_v(px, py + 4, pw, 32, GUI_TITLE_A, GUI_TITLE_B);
    // Rounded top corners — overdraw corners manually (gradient doesn't clip automatically)
    // But WIN_RADIUS handles the outer shape; gradient just fills inside.

    // Title
    draw_string_px(px + (pw - 14*8)/2, py + 12, "Mectov OS v13.5", GUI_TEXT, GUI_TITLE_A);

    // ----- Lock icon (modern) -----
    // Circle with padlock
    int lock_cx = px + pw/2;
    int lock_cy = py + 68;
    fill_circle(lock_cx, lock_cy, 16, GUI_TEAL);
    fill_circle(lock_cx, lock_cy, 12, GUI_BG);
    // Lock shackle (rounded top)
    draw_rounded_rect(lock_cx - 6, lock_cy - 12, 12, 10, 3, GUI_TEAL);
    // Lock body
    draw_rounded_rect(lock_cx - 8, lock_cy - 4, 16, 12, 2, GUI_TEAL);
    // Keyhole
    fill_circle(lock_cx, lock_cy + 2, 3, GUI_BG);
    draw_rect(lock_cx - 1, lock_cy + 4, 2, 3, GUI_BG);

    // ----- Subtitle -----
    draw_string_px(px + (pw - 12*8)/2, py + 100, "Sign in to continue", GUI_DIM, GUI_BG);

    // ----- "Password" label -----
    draw_string_px(px + 24, py + 124, "Password", GUI_DIM, GUI_BG);

    // ----- Input box (rounded) -----
    int ibx = px + 24, iby = py + 140, ibw = pw - 48, ibh = 28;
    draw_rounded_rect(ibx, iby, ibw, ibh, 4, 0x00111122);
    draw_rounded_rect_border(ibx, iby, ibw, ibh, 4, GUI_BORDER);

    // Masked password dots
    int dot_color = 0x00A6E3A1;
    for (int i = 0; i < pass_len; i++)
        fill_circle(ibx + 8 + i * 12, iby + ibh/2, 3, dot_color);

    // Blinking cursor
    uint32_t cur_col = ((get_ticks() / 500) & 1) ? GUI_TEXT : 0x00111122;
    int cursor_x = ibx + 8 + pass_len * 12;
    if (pass_len > 0) cursor_x += 4;
    draw_rect(cursor_x, iby + 6, 2, ibh - 12, cur_col);

    // ----- Caps Lock indicator -----
    if (cap_lock) {
        draw_string_px(ibx + ibw - 40, iby + 6, "CAPS", GUI_CLOSE, 0x00111122);
    }

    // ----- Error message (rounded pill) -----
    if (err) {
        int pill_w = 140;
        int pill_x = px + (pw - pill_w) / 2;
        draw_rounded_rect(pill_x, py + 176, pill_w, 20, 10, 0x00331111);
        draw_rounded_rect_border(pill_x, py + 176, pill_w, 20, 10, GUI_CLOSE);
        draw_string_px(pill_x + (pill_w - 14*8)/2, py + 178, "Wrong password!", GUI_CLOSE, 0x00331111);
    }

    // ----- Login button (rounded, gradient) -----
    int bbx = px + pw/2 - 60, bby = py + 212;
    draw_rounded_rect(bbx, bby, 120, 32, 6, GUI_BLUE);
    draw_rounded_rect_border(bbx, bby, 120, 32, 6, 0x004466AA);
    // Inner highlight
    draw_rounded_rect(bbx + 2, bby + 2, 116, 14, 4, 0x00A0C0FF);
    draw_string_px(bbx + (120 - 5*8)/2, bby + 8, "Sign In", 0x00111122, 0x00A0C0FF);
}

int gui_login() {
    const char* pass = "mectov123";
    char input[32];
    int idx = 0, shake = 0, err = 0, cap_lock_active = 0;

    cursor_saved_x = -1;
    input[0] = '\0';

    uint32_t last_draw = 0;

    while (1) {
        uint32_t now = get_ticks();
        if (now != last_draw) {
            last_draw = now;
            draw_login(idx, shake, err, cap_lock_active);
            draw_mouse_cursor(mouse_x, mouse_y);
            swap_buffers();
            if (shake > 0) shake--;
        }

        // Keyboard
        uint8_t sc = k_get_scancode();
        if (sc != 0 && sc < 0x80) {
            // Track Caps Lock
            if (sc == 0x3A) { cap_lock_active = !cap_lock_active; }

            char c = scancode_to_char(sc);
            if (c == '\n') {
                input[idx] = '\0';
                if (strcmp(input, pass) == 0) { beep(); return 1; }
                err = 1; shake = 10; idx = 0;
            } else if (c == '\b') {
                if (idx > 0) { idx--; err = 0; }
            } else if (c != 0 && idx < 31) {
                input[idx++] = c; err = 0;
            }
        }

        // Mouse click on Sign In button
        int pw2 = 340, ph2 = 260;
        int px2  = (int)(fb_width  - pw2) / 2;
        int py2  = (int)(fb_height - ph2) / 2;
        int bbx2 = px2 + pw2/2 - 60, bby2 = py2 + 212;
        if ((mouse_btn & 1) &&
            mouse_x >= bbx2 && mouse_x < bbx2 + 120 &&
            mouse_y >= bby2 && mouse_y < bby2 + 32) {
            input[idx] = '\0';
            if (strcmp(input, pass) == 0) { beep(); return 1; }
            err = 1; shake = 10; idx = 0;
        }

        __asm__ __volatile__ ("hlt");
    }
}