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
    if (fb_width > wp_w) {
        draw_rect(wp_w, 0, fb_width - wp_w, area_h, 0x00111122);
    }
    if (area_h > wp_h) {
        draw_rect(0, wp_h, fb_width, area_h - wp_h, 0x00111122);
    }

    // Overlay semi-transparent dark (Dim the whole screen)
    draw_rect_alpha(0, 0, fb_width, fb_height, 0x00000000);

    // ----- Glassmorphism Panel -----
    int pw = 360, ph = 300;
    int shake_off = shake ? ((shake & 1) ? 6 : -6) : 0;
    int px = (int)(fb_width  - pw) / 2 + shake_off;
    int py = (int)(fb_height - ph) / 2;
    int r = 16; // corner radius

    // Panel background: sleek dark slate
    uint32_t panel_bg = 0x001C1C1E;
    draw_rounded_rect(px, py, pw, ph, r, panel_bg);
    draw_rounded_rect_border(px, py, pw, ph, r, 0x003A3A3C);

    // ----- Avatar circle -----
    int av_cx = px + pw / 2;
    int av_cy = py + 60;
    // Sleek grey rings
    fill_circle(av_cx, av_cy, 24, 0x003A3A3C);
    fill_circle(av_cx, av_cy, 22, 0x002C2C2E);
    // User silhouette
    fill_circle(av_cx, av_cy - 4, 7, 0x008E8E93); // head
    draw_rounded_rect(av_cx - 10, av_cy + 5, 20, 10, 4, 0x008E8E93); // body

    // ----- Welcome text -----
    draw_string_px(px + (pw - 12*8)/2, py + 100, "Welcome back", 0x00A0A0A5, 0xFFFFFFFF);
    draw_string_px(px + (pw - 4*8)/2, py + 118, "User", 0x00FFFFFF, 0xFFFFFFFF);

    // ----- Password input field -----
    int ibx = px + 40, iby = py + 154, ibw = pw - 80, ibh = 36;
    // Input background (darker than panel)
    draw_rounded_rect(ibx, iby, ibw, ibh, 8, 0x000A0A0A);
    draw_rounded_rect_border(ibx, iby, ibw, ibh, 8, 0x003A3A3C);

    // Focus indicator (thin blue highlight at the bottom)
    draw_rounded_rect(ibx + 10, iby + ibh - 2, ibw - 20, 2, 1, 0x000A84FF);

    // Lock icon minimal
    draw_rect(ibx - 18, iby + 16, 8, 6, 0x00666666);
    draw_rounded_rect_border(ibx - 17, iby + 12, 6, 6, 2, 0x00666666);

    // Password dots
    int dot_color = 0x00FFFFFF;
    for (int i = 0; i < pass_len && i < 20; i++)
        fill_circle(ibx + 12 + i * 14, iby + ibh / 2, 3, dot_color);

    // Blinking cursor
    uint32_t cur_col = ((get_ticks() / 500) & 1) ? 0x00FFFFFF : 0x000A0A0A;
    int cursor_x = ibx + 12 + pass_len * 14;
    if (pass_len > 0) cursor_x += 6;
    draw_rect(cursor_x, iby + 10, 2, ibh - 20, cur_col);

    // Caps Lock indicator
    if (cap_lock) {
        int clx = ibx + ibw - 36;
        draw_rounded_rect(clx, iby + 8, 28, ibh - 16, 4, 0x00333333);
        draw_string_px(clx + 6, iby + 10, "UP", 0x00FFFFFF, 0xFFFFFFFF);
    }

    // ----- Error message -----
    if (err) {
        draw_string_px(px + (pw - 15*8)/2, py + 200, "Wrong password!", 0x00FF453A, 0xFFFFFFFF);
    }

    // ----- Sign In button -----
    int bbx = px + 40, bby = py + 230, bbw = pw - 80, bbh = 36;
    draw_rounded_rect(bbx, bby, bbw, bbh, 8, 0x000A84FF); // Solid modern blue
    // Highlight
    draw_rounded_rect_border(bbx, bby, bbw, bbh, 8, 0x000071E3);
    // Centered text
    draw_string_px(bbx + (bbw - 7*8)/2, bby + 10, "Sign In", 0x00FFFFFF, 0xFFFFFFFF);
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
        int pw2 = 360, ph2 = 300;
        int px2  = (int)(fb_width  - pw2) / 2;
        int py2  = (int)(fb_height - ph2) / 2;
        int bbx2 = px2 + (pw2 - 140) / 2, bby2 = py2 + 240;
        if ((mouse_btn & 1) &&
            mouse_x >= bbx2 && mouse_x < bbx2 + 140 &&
            mouse_y >= bby2 && mouse_y < bby2 + 36) {
            input[idx] = '\0';
            if (strcmp(input, pass) == 0) { beep(); return 1; }
            err = 1; shake = 10; idx = 0;
        }

        __asm__ __volatile__ ("hlt");
    }
}