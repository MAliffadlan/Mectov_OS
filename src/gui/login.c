#include "../include/login.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/utils.h"
#include "../include/speaker.h"
#include "../include/mouse.h"
#include "../include/timer.h"

static void draw_login(int pass_len, int shake, int err) {
    if (!is_vbe || fb_width == 0 || fb_height == 0) return;

    // Full-screen Mint gradient background
    for (uint32_t y = 0; y < fb_height; y++) {
        uint32_t t = (y * 255) / fb_height;
        uint32_t r = 30 + (t * 10) / 255;
        uint32_t g = 34 + (t * 20) / 255;
        uint32_t b = 36 + (t * 10) / 255;
        draw_rect(0, y, fb_width, 1, (r << 16) | (g << 8) | b);
    }

    // Panel centered (320x210)
    int pw = 320, ph = 210;
    int shake_off = shake ? ((shake & 1) ? 8 : -8) : 0;
    int px = (int)(fb_width  - pw) / 2 + shake_off;
    int py = (int)(fb_height - ph) / 2;

    // Shadow + panel
    draw_rect(px + 6, py + 6, pw, ph, 0x00000020);
    draw_rect(px, py, pw, ph, GUI_BG);
    draw_rect_border(px - 1, py - 1, pw + 2, ph + 2, GUI_BORDER);
    
    // Header bar gradient
    draw_rect(px, py, pw, 32, GUI_TITLE_A);
    draw_rect(px, py + 32, pw, 1, GUI_BORDER);

    // Title
    draw_string_px(px + (pw - 21*8)/2, py + 8, "Mectov OS v13.5 [VBE]", GUI_TEXT, GUI_TITLE_A);

    // Lock icon area
    draw_string_px(px + (pw - 8)/2, py + 52, "@", GUI_CYAN, GUI_BG);

    // Subtitle
    draw_string_px(px + (pw - 14*8)/2, py + 72, "Please sign in", GUI_DIM, GUI_BG);

    // "Password:" label
    draw_string_px(px + 24, py + 96, "Password:", GUI_DIM, GUI_BG);

    // Input box
    int ibx = px + 24, iby = py + 114, ibw = pw - 48, ibh = 24;
    draw_rect(ibx, iby, ibw, ibh, GUI_WHITE);
    draw_rect_border(ibx, iby, ibw, ibh, GUI_BORDER);
    
    // Masked password stars
    for (int i = 0; i < pass_len; i++)
        draw_char_px(ibx + 6 + i*8, iby + 4, '*', GUI_TEXT, 0xFFFFFFFF);
        
    // Blinking cursor
    uint32_t cur_col = (get_ticks() & 16) ? GUI_TEXT : GUI_WHITE;
    int cursor_x = ibx + 6 + pass_len * 8;
    draw_rect(cursor_x, iby + 4, 2, 16, cur_col);

    // Error message
    if (err)
        draw_string_px(px + 24, py + 144, "Wrong password!", GUI_CLOSE, GUI_BG);

    // Login button
    int bbx = px + pw/2 - 40, bby = py + 168;
    draw_rect(bbx, bby, 80, 26, GUI_BTN);
    draw_rect_border(bbx, bby, 80, 26, GUI_BORDER);
    draw_string_px(bbx + (80 - 5*8)/2, bby + 5, "Login", GUI_TEXT, GUI_BTN);
}

int gui_login() {
    const char* pass = "mectov123";
    char input[32];
    int idx = 0, shake = 0, err = 0;

    cursor_saved_x = -1;
    input[0] = '\0';

    uint32_t last_draw = 0;

    while (1) {
        // Redraw on every timer tick or when something changes
        uint32_t now = get_ticks();
        if (now != last_draw) {
            last_draw = now;
            draw_login(idx, shake, err);
            draw_mouse_cursor(mouse_x, mouse_y);
            swap_buffers();
            if (shake > 0) shake--;
        }

        // Keyboard
        uint8_t sc = k_get_scancode();
        if (sc != 0 && sc < 0x80) {
            char c = scancode_to_char(sc);
            if (c == '\n') {
                input[idx] = '\0';
                if (strcmp(input, pass) == 0) { beep(); return 1; }
                err = 1; shake = 8; idx = 0;
            } else if (c == '\b') {
                if (idx > 0) { idx--; err = 0; }
            } else if (c != 0 && idx < 31) {
                input[idx++] = c; err = 0;
            }
        }

        // Mouse click on Login button
        int pw2 = 320, ph2 = 210;
        int px2  = (int)(fb_width  - pw2) / 2;
        int py2  = (int)(fb_height - ph2) / 2;
        int bbx2 = px2 + pw2/2 - 40, bby2 = py2 + 168;
        if ((mouse_btn & 1) &&
            mouse_x >= bbx2 && mouse_x < bbx2 + 80 &&
            mouse_y >= bby2 && mouse_y < bby2 + 26) {
            input[idx] = '\0';
            if (strcmp(input, pass) == 0) { beep(); return 1; }
            err = 1; shake = 8; idx = 0;
        }

        __asm__ __volatile__ ("hlt");
    }
}
