#include "../include/login.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/utils.h"
#include "../include/speaker.h"
#include "../include/mouse.h"
#include "../include/timer.h"
#include "../include/rtc.h"
#include "../include/font8x16.h"
#include "../include/passwd.h"

// ---- Instrument-console palette (warm charcoal + phosphor amber) ----
// Mectov is a hand-built OS, so the gate should read as a machine console:
// the live clock is proof the box is awake. Amber is the phosphor of CRT
// terminals — warm rather than the acid-green cliché or the blue every OS
// uses. The flow is deliberately Windows-like: a minimal lock screen with
// just the time, dismissed by any keypress or click, followed by the
// password entry screen.
#define IC_BG_DEEP    0x000B0A08  // wallpaper dim layer (warm near-black)
#define IC_BG_PANEL   0x0016130F  // panel fill (warm charcoal)
#define IC_LINE       0x002C2821  // hairline borders
#define IC_INK        0x00EDE6D9  // primary text (warm off-white)
#define IC_DIM        0x008A8172  // secondary text
#define IC_AMBER      0x00E0A94F  // phosphor amber (accent)
#define IC_AMBER_BRT  0x00F5C566  // brighter amber (hover / active)
#define IC_DANGER     0x00C94F3D  // warm red (errors)
#define IC_DOT        0x00E0A94F  // password dots (amber)

// ---- Local number formatting (kernel has no vsprintf for pixel screens) ----
static void itoa2(char* out, int val) {
    out[0] = '0' + (val / 10) % 10;
    out[1] = '0' + val % 10;
    out[2] = '\0';
}

static void itoa_dec(char* out, int val) {
    char tmp[8];
    int n = 0;
    do { tmp[n++] = '0' + val % 10; val /= 10; } while (val);
    while (n) *out++ = tmp[--n];
    *out = '\0';
}

// ---- Date names (rtc dow: 1=Sunday..7=Saturday) ----
static const char* const day_names[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};
static const char* const month_names[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

// ---- Scaled bitmap text: renders the 8x16 font at `scale` pixels per bit ----
static int draw_char_scale(int px, int py, char c, int scale, uint32_t fg) {
    const unsigned char* g = font8x16_data[(unsigned char)c];
    for (int y = 0; y < 16; y++) {
        unsigned char row = g[y];
        if (!row) continue;
        for (int x = 0; x < 8; x++) {
            if (row & (0x80 >> x)) {
                draw_rect(px + x * scale, py + y * scale, scale, scale, fg);
            }
        }
    }
    return 8 * scale;
}

// Draw a string at `scale` with `ls` extra pixels between glyphs.
// Returns the total advance width.
static int draw_str_scale(int px, int py, const char* s, int scale, int ls, uint32_t fg) {
    int cx = px;
    for (; *s; s++) {
        if (*s == ' ') { cx += 4 * scale + ls; continue; }
        cx += draw_char_scale(cx, py, *s, scale, fg) + ls;
    }
    return cx - px;
}

// Advance width of a string (same metrics as draw_str_scale), for centering.
static int str_advance(const char* s, int scale, int ls) {
    int w = 0;
    for (; *s; s++) w += (*s == ' ') ? (4 * scale + ls) : (8 * scale + ls);
    return w;
}

// ---- Live clock + date ----
// The CMOS read itself is gated: rtc_read_time() busy-waits on the UIP flag
// (up to 10ms), so reading it every 16ms frame would stall frames whenever
// the update window is hit. Re-read at most twice per second and only
// reformat when the second (or the day) actually changed.
static char clock_str[9];
static char date_str[32];

static void refresh_clock(void) {
    static int last_sec = -1;
    static int last_day = -1;
    static uint32_t last_read = 0;
    uint32_t now = get_ticks();
    if (last_read != 0 && now - last_read < 500) return;  // first call always reads
    last_read = now;
    rtc_time_t t = rtc_read_time();

    if ((int)t.day != last_day) {  // rebuild the date once per day
        last_day = t.day;
        const char* dn = day_names[(t.dow - 1) % 7];
        const char* mn = month_names[(t.month - 1) % 12];
        int k = 0;
        while (*dn) date_str[k++] = *dn++;
        date_str[k++] = ',';
        date_str[k++] = ' ';
        itoa_dec(date_str + k, t.day);
        while (date_str[k]) k++;
        date_str[k++] = ' ';
        while (*mn) date_str[k++] = *mn++;
        date_str[k] = '\0';
    }

    int sec = t.second;
    if (sec == last_sec) return;
    last_sec = sec;
    itoa2(clock_str, t.hour);
    clock_str[2] = ':';
    itoa2(clock_str + 3, t.minute);
    clock_str[5] = ':';
    itoa2(clock_str + 6, t.second);
    clock_str[8] = '\0';
}

// ---- Shared background: wallpaper dimmed to a warm charcoal field ----
static void draw_background(void) {
    extern uint32_t _binary_obj_wallpaper_bin_start[];
    uint32_t* wp_ptr = _binary_obj_wallpaper_bin_start;
    uint32_t wp_w = 1024, wp_h = 768;
    uint32_t copy_w = (fb_width  < wp_w) ? fb_width  : wp_w;
    uint32_t copy_h = (fb_height < wp_h) ? fb_height : wp_h;
    for (uint32_t y = 0; y < copy_h; y++) {
        memcpy(&back_buffer[y * fb_width], &wp_ptr[y * wp_w], copy_w * 4);
    }
    if (fb_width > wp_w)  draw_rect(wp_w, 0, fb_width - wp_w, fb_height, IC_BG_DEEP);
    if (fb_height > wp_h) draw_rect(0, wp_h, fb_width, fb_height - wp_h, IC_BG_DEEP);
    // Two 50% blends pull the wallpaper down to a warm charcoal field; a
    // dark band at top and bottom adds gentle depth.
    draw_rect_alpha(0, 0, fb_width, fb_height, 0x00000000);
    draw_rect_alpha(0, 0, fb_width, fb_height, 0x000B0A08);
    draw_rect_alpha(0, 0, fb_width, fb_height / 5, 0x00000000);
    draw_rect_alpha(0, fb_height - fb_height / 5, fb_width, fb_height / 5, 0x00000000);
}

// ---- Bottom-center release string (both screens) ----
static void draw_footer(void) {
    char footer[48];
    int fl = 0;
    const char* fv = "v"; while (*fv) footer[fl++] = *fv++;
    const char* ov = OS_VERSION; while (*ov) footer[fl++] = *ov++;
    const char* fr = "  SMP  MECTOVFS  1024x768";
    while (*fr) footer[fl++] = *fr++;
    footer[fl] = '\0';
    draw_string_px((int)(fb_width - fl * 8) / 2, 716, footer, IC_DIM, 0xFFFFFFFF);
}

// ---- Lock screen: one big clock, a date, nothing else ----
static void draw_lock_screen(void) {
    if (!is_vbe || fb_width == 0 || fb_height == 0) return;
    draw_background();
    refresh_clock();

    int cx = (int)fb_width / 2;

    // Brand wordmark — small, top center, deliberate restraint
    const char* wm = "MECTOV OS";
    draw_str_scale(cx - str_advance(wm, 2, 6) / 2, 64, wm, 2, 6, IC_AMBER);

    // Hero clock: HH:MM at 8x with a blinking colon
    int scale = 8;
    int gy = 248;
    int gx = cx - (5 * 8 * scale) / 2;  // 5 glyphs, no spacing
    // draw_char_scale returns the glyph ADVANCE (8*scale), so accumulate
    // into gx with += — assigning (=) would reset gx to 64 and stack every
    // following glyph on top of each other.
    gx += draw_char_scale(gx, gy, clock_str[0], scale, IC_AMBER_BRT);
    gx += draw_char_scale(gx, gy, clock_str[1], scale, IC_AMBER_BRT);
    if ((get_ticks() / 500) & 1) {
        gx += draw_char_scale(gx, gy, ':', scale, IC_AMBER_BRT);
    } else {
        gx += 8 * scale;  // blink: skip the colon, background shows through
    }
    gx += draw_char_scale(gx, gy, clock_str[3], scale, IC_AMBER_BRT);
    draw_char_scale(gx, gy, clock_str[4], scale, IC_AMBER_BRT);

    // Date line under the clock
    draw_str_scale(cx - str_advance(date_str, 2, 4) / 2, 424, date_str, 2, 4, IC_INK);

    // Hint: how to proceed
    const char* hint = "click anywhere or press any key";
    draw_str_scale(cx - str_advance(hint, 1, 0) / 2, 690, hint, 1, 0, IC_DIM);

    draw_footer();
}

// ---- Password entry screen ----
// Panel geometry shared by render and hit-test (one source of truth).
#define LOGIN_PW  380
#define LOGIN_PH  178
#define LOGIN_PY  ((int)(fb_height - LOGIN_PH) / 2 + 56)

static void draw_login(int pass_len, int shake, int err, int cap_lock) {
    if (!is_vbe || fb_width == 0 || fb_height == 0) return;
    draw_background();
    refresh_clock();

    int shake_off = shake ? ((shake & 1) ? 6 : -6) : 0;
    int cx = (int)fb_width / 2 + shake_off;

    // Clock + date shrink to the top-left corner (Windows-style after dismiss)
    draw_str_scale(32, 28, clock_str, 2, 4, IC_AMBER_BRT);
    draw_str_scale(32, 58, date_str, 1, 2, IC_DIM);

    // User avatar above the panel: amber ring, warm fill, brand initial
    int avx = cx, avy = LOGIN_PY - 46;
    fill_circle(avx, avy, 26, IC_BG_PANEL);
    draw_circle(avx, avy, 26, IC_AMBER);
    draw_str_scale(avx - 8, avy - 16, "M", 2, 0, IC_AMBER);

    // ----- Flat instrument panel -----
    int py = LOGIN_PY;
    int box_x = (int)(fb_width - LOGIN_PW) / 2 + shake_off;
    draw_rounded_rect(box_x, py, LOGIN_PW, LOGIN_PH, 6, IC_BG_PANEL);
    draw_rounded_rect_border(box_x, py, LOGIN_PW, LOGIN_PH, 6, IC_LINE);

    // Identity line
    draw_string_px(box_x + 32, py + 18, "root@mectov", IC_DIM, 0xFFFFFFFF);
    draw_string_px(box_x + 32 + 12 * 8, py + 18, "~", IC_DIM, 0xFFFFFFFF);

    // ----- Password input (underline style, terminal-like) -----
    int ibx = box_x + 32, iby = py + 46;
    int ibw = LOGIN_PW - 64, ibh = 26;
    draw_rect(ibx, iby, ibw, ibh, 0x00000000); // clear old frame content
    draw_rect(ibx, iby + ibh - 2, ibw, 2, IC_AMBER); // focus underline

    // password dots (amber)
    for (int i = 0; i < pass_len && i < 20; i++)
        fill_circle(ibx + 6 + i * 12, iby + ibh / 2 - 1, 3, IC_DOT);

    // blinking block cursor (amber)
    uint32_t cur_col = ((get_ticks() / 500) & 1) ? IC_AMBER_BRT : 0x00000000;
    int cursor_x = ibx + 6 + pass_len * 12;
    if (pass_len > 0) cursor_x += 6;
    draw_rect(cursor_x, iby + 3, 8, ibh - 8, cur_col);

    // Caps Lock indicator (right of the field)
    if (cap_lock) {
        draw_rect(ibx + ibw - 8, iby + 4, 8, ibh - 8, 0x00222019);
        draw_string_px(ibx + ibw - 6, iby + 5, "CAPS", IC_AMBER, 0xFFFFFFFF);
    }

    // ----- Error copy: names the cause, suggests the fix -----
    if (err) {
        draw_string_px(box_x + (LOGIN_PW - 15 * 8) / 2, py + LOGIN_PH + 12,
                       "Access denied - check Caps", IC_DANGER, 0xFFFFFFFF);
        draw_string_px(box_x + (LOGIN_PW - 15 * 8) / 2, py + LOGIN_PH + 28,
                       "Lock, then try again.", IC_DANGER, 0xFFFFFFFF);
    }

    // ----- Sign In: flat, amber outline, hover inverts -----
    int bbx = box_x + 32, bby = py + 88, bbw = LOGIN_PW - 64, bbh = 30;
    int hover = (mouse_btn & 1) &&
                mouse_x >= bbx && mouse_x < bbx + bbw &&
                mouse_y >= bby && mouse_y < bby + bbh;
    if (hover) {
        draw_rect(bbx, bby, bbw, bbh, IC_AMBER);
        draw_string_px(bbx + (bbw - 8 * 8) / 2, bby + 7, "SIGN IN", 0x0016130F, 0xFFFFFFFF);
    } else {
        draw_rect(bbx, bby, bbw, bbh, IC_BG_PANEL);
        draw_rect_border(bbx, bby, bbw, bbh, IC_AMBER);
        draw_string_px(bbx + (bbw - 8 * 8) / 2, bby + 7, "SIGN IN", IC_AMBER, 0xFFFFFFFF);
    }

    draw_footer();
}

int gui_login() {
    // Password comes from /etc/passwd (see sys_get_password); the hardcoded
    // default is only a fallback for a fresh disk with no file yet.
    char pass[PASSWD_MAX_LEN + 1];
    sys_get_password(pass, (int)sizeof(pass));
    char input[32];
    int idx = 0, shake = 0, err = 0, cap_lock_active = 0;
    int locked = 1;                 // Windows-style: dismiss with any key/click
    uint32_t click_ignore_until = 0; // debounce the dismissing click

    cursor_saved_x = -1;
    input[0] = '\0';
    clock_str[0] = '\0';
    date_str[0] = '\0';

    uint32_t last_draw = 0;

    while (1) {
        uint32_t now = get_ticks();
        if (now - last_draw >= 16) {
            last_draw = now;
            extern void mark_dirty(int, int, int, int);
            mark_dirty(0, 0, fb_width, fb_height);
            if (locked) draw_lock_screen();
            else draw_login(idx, shake, err, cap_lock_active);
            extern int cursor_draw_x, cursor_draw_y;
            cursor_draw_x = mouse_x;
            cursor_draw_y = mouse_y;
            swap_buffers();
            if (shake > 0) shake--;
        }

        // ---- State machine ----
        uint8_t sc = k_get_scancode();
        if (sc != 0 && sc < 0x80) {
            if (locked) {
                // Any key dismisses the lock screen. The dismissing press is
                // consumed so it never reaches the password field — only the
                // *next* keypress types (Windows behavior).
                if (sc == 0x3A) cap_lock_active = !cap_lock_active;
                locked = 0;
            } else {
                char c = scancode_to_char(sc);
                if (sc == 0x3A) { cap_lock_active = !cap_lock_active; }

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
        }

        if (locked) {
            // Any left click dismisses; ignore the held button for a beat so
            // it cannot immediately trigger Sign In on the next screen.
            if ((mouse_btn & 1) && now >= click_ignore_until) {
                click_ignore_until = now + 300;
                locked = 0;
            }
        } else {
            // Mouse click on Sign In button (same geometry as the render)
            int box_x2 = (int)(fb_width - LOGIN_PW) / 2;
            int bbx2 = box_x2 + 32, bby2 = LOGIN_PY + 88;
            if ((mouse_btn & 1) && now >= click_ignore_until &&
                mouse_x >= bbx2 && mouse_x < bbx2 + LOGIN_PW - 64 &&
                mouse_y >= bby2 && mouse_y < bby2 + 30) {
                input[idx] = '\0';
                if (strcmp(input, pass) == 0) { beep(); return 1; }
                err = 1; shake = 10; idx = 0;
            }
        }

        __asm__ __volatile__ ("hlt");
    }
}
