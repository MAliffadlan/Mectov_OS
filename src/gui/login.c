#include "../include/login.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/utils.h"
#include "../include/speaker.h"
#include "../include/mouse.h"
#include "../include/timer.h"
#include "../include/rtc.h"
#include "../include/font8x16.h"

// ---- Instrument-console palette (warm charcoal + phosphor amber) ----
// Deliberate departure from the previous glassmorphism/macOS-style login:
// Mectov is a hand-built OS, so the gate should read as a machine console —
// the live clock and system manifest are proof the box is awake, not a
// consumer welcome screen. Amber is the phosphor of CRT terminals, warm
// rather than the acid-green cliché or the blue every OS uses.
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

// ---- Live manifest (cores + uptime), two groups centered around cx ----
static void draw_manifest(int cy, int cx) {
    extern uint32_t smp_cpu_count;
    extern uint32_t get_uptime_seconds(void);

    char num[3];
    uint32_t cores = (smp_cpu_count > 0) ? smp_cpu_count : 1;
    uint32_t up = get_uptime_seconds();
    uint32_t h = up / 3600, m = (up / 60) % 60, s = up % 60;

    // Left group: 4 blocks + "4 cores"  (width 40 + 8 + 40 = 88)
    int lx = cx - 96;
    for (uint32_t i = 0; i < 4; i++) {
        draw_rect(lx, cy, 6, 10, (i < cores) ? IC_AMBER : 0x00222019);
        lx += 10;
    }
    itoa2(num, (int)cores);
    draw_string_px(lx + 2, cy, num, IC_INK, 0xFFFFFFFF);
    draw_string_px(lx + 12, cy, "cores", IC_DIM, 0xFFFFFFFF);

    // Right group: "up 00:12:34"  (width 16 + 8 + 64 = 88)
    int rx = cx + 18;
    draw_string_px(rx, cy, "up", IC_DIM, 0xFFFFFFFF);
    itoa2(num, (int)h); draw_string_px(rx + 26, cy, num, IC_AMBER, 0xFFFFFFFF);
    draw_string_px(rx + 44, cy, ":", IC_DIM, 0xFFFFFFFF);
    itoa2(num, (int)m); draw_string_px(rx + 52, cy, num, IC_AMBER, 0xFFFFFFFF);
    draw_string_px(rx + 70, cy, ":", IC_DIM, 0xFFFFFFFF);
    itoa2(num, (int)s); draw_string_px(rx + 78, cy, num, IC_AMBER, 0xFFFFFFFF);
}

// Render the current time once per second from the cached string. The
// CMOS read itself is gated too: rtc_read_time() busy-waits on the UIP
// flag (up to 10ms), so reading it every 16ms frame (draw_login's cadence)
// would stall frames whenever the update window is hit. Re-read at most
// twice per second and only reformat when the second actually changed.
static char clock_str[9];

static void refresh_clock(void) {
    static int last_sec = -1;
    static uint32_t last_read = 0;
    uint32_t now = get_ticks();
    if (now - last_read < 500) return;
    last_read = now;
    rtc_time_t t = rtc_read_time();
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

// Panel geometry shared by render and hit-test (one source of truth).
#define LOGIN_PW  380
#define LOGIN_PH  178
#define LOGIN_PY  ((int)(fb_height - LOGIN_PH) / 2 + 56)

static void draw_login(int pass_len, int shake, int err, int cap_lock) {
    if (!is_vbe || fb_width == 0 || fb_height == 0) return;

    // ----- Background: wallpaper dimmed to a deep warm field -----
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
    // Two 50% blends (draw_rect_alpha) pull the wallpaper down to a warm
    // charcoal field; a dark band at top and bottom adds gentle depth.
    draw_rect_alpha(0, 0, fb_width, fb_height, 0x00000000);
    draw_rect_alpha(0, 0, fb_width, fb_height, 0x000B0A08);
    draw_rect_alpha(0, 0, fb_width, fb_height / 5, 0x00000000);
    draw_rect_alpha(0, fb_height - fb_height / 5, fb_width, fb_height / 5, 0x00000000);

    int shake_off = shake ? ((shake & 1) ? 6 : -6) : 0;
    int cx = (int)fb_width / 2 + shake_off;

    // ----- Wordmark: one line "MECTOV OS", 2x, letter-spaced -----
    // advance = 9*16 + 8*6 = 192
    const int ww = 192;
    draw_str_scale(cx - ww / 2, 148, "MECTOV OS", 2, 6, IC_AMBER);
    draw_rect(cx - ww / 2 - 6, 192, ww + 12, 1, 0x003C2E18);

    // ----- Signature: live clock (2x phosphor, centered) -----
    refresh_clock();
    // 8 glyphs, 2x, ls=5 → advance = 8*16 + 7*5 = 163
    draw_str_scale(cx - 81, 210, clock_str, 2, 5, IC_AMBER_BRT);

    // ----- Manifest: cores + uptime -----
    draw_manifest(290, cx);

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

    // ----- Footer manifest (release string from utils.h) -----
    char footer[48];
    int fl = 0;
    const char* fv = "v";
    while (*fv) footer[fl++] = *fv++;
    const char* ov = OS_VERSION;
    while (*ov) footer[fl++] = *ov++;
    const char* fr = "  SMP  MECTOVFS  1024x768";
    while (*fr) footer[fl++] = *fr++;
    footer[fl] = '\0';
    draw_string_px((int)(fb_width - fl * 8) / 2, 716, footer, IC_DIM, 0xFFFFFFFF);
}

int gui_login() {
    const char* pass = "mectov123";
    char input[32];
    int idx = 0, shake = 0, err = 0, cap_lock_active = 0;

    cursor_saved_x = -1;
    input[0] = '\0';
    clock_str[0] = '\0';

    uint32_t last_draw = 0;

    while (1) {
        uint32_t now = get_ticks();
        if (now - last_draw >= 16) {
            last_draw = now;
            extern void mark_dirty(int, int, int, int);
            mark_dirty(0, 0, fb_width, fb_height);
            draw_login(idx, shake, err, cap_lock_active);
            extern int cursor_draw_x, cursor_draw_y;
            cursor_draw_x = mouse_x;
            cursor_draw_y = mouse_y;
            swap_buffers();
            if (shake > 0) shake--;
        }

        // Keyboard
        uint8_t sc = k_get_scancode();
        if (sc != 0 && sc < 0x80) {
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

        // Mouse click on Sign In button (same geometry as the render)
        int box_x2 = (int)(fb_width - LOGIN_PW) / 2;
        int bbx2 = box_x2 + 32, bby2 = LOGIN_PY + 88;
        if ((mouse_btn & 1) &&
            mouse_x >= bbx2 && mouse_x < bbx2 + LOGIN_PW - 64 &&
            mouse_y >= bby2 && mouse_y < bby2 + 30) {
            input[idx] = '\0';
            if (strcmp(input, pass) == 0) { beep(); return 1; }
            err = 1; shake = 10; idx = 0;
        }

        __asm__ __volatile__ ("hlt");
    }
}
