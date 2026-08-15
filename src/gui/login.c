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
#include "../include/mem.h"

// ---- Instrument-console palette (warm charcoal + phosphor amber) ----
// Mectov is a hand-built OS, so the gate should read as a machine console:
// the live clock is proof the box is awake. Amber is the phosphor of CRT
// terminals — warm rather than the acid-green cliché or the blue every OS
// uses. The flow is deliberately Windows-like: a minimal lock screen with
// the full wallpaper, a digital clock + full date pinned to the bottom-left
// corner, and a password entry that only opens on SPACE (or a click). While
// typing the password the wallpaper blurs and every other caption (clock,
// footer) disappears so the eye lands on the panel alone. If the user stops
// typing for 4 seconds, the screen falls back to the clock view.
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

// ---- Live clock + date ----
// The CMOS read itself is gated: rtc_read_time() busy-waits on the UIP flag
// (up to 10ms), so reading it every 16ms frame would stall frames whenever
// the update window is hit. Re-read at most twice per second and only
// reformat when the second (or the day) actually changed.
static char clock_str[9];
static char date_str[32];
static int rtc_wall_sec = -1;   // last RTC second seen (wall time, for the idle window)

static void refresh_clock(void) {
    static int last_sec = -1;
    static int last_day = -1;
    static uint32_t last_read = 0;
    uint32_t now = get_ticks();
    if (last_read != 0 && now - last_read < 500) return;  // first call always reads
    last_read = now;
    rtc_time_t t = rtc_read_time();
    rtc_wall_sec = t.second;    // cached for gui_login's 4 s idle window

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
        date_str[k++] = ' ';
        itoa_dec(date_str + k, t.year); // full year, e.g. 2026
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

// ---- Shared background: full wallpaper, never cropped ----
// The wallpaper is scaled (nearest-neighbor) to fill the ENTIRE framebuffer,
// so it is always full-bleed at any resolution — no crop, no dark filler
// bands. A single cached scaled copy is built once per resolution and then
// memcpy'd every frame.
static uint32_t* wp_scaled = NULL;
static uint32_t wp_scaled_w = 0, wp_scaled_h = 0;

static void draw_background(void) {
    extern uint32_t _binary_obj_wallpaper_bin_start[];
    if (!is_vbe || fb_width == 0 || fb_height == 0) return;
    if (!wp_scaled || wp_scaled_w != fb_width || wp_scaled_h != fb_height) {
        if (wp_scaled) kfree(wp_scaled);
        wp_scaled = (uint32_t*)kmalloc(fb_width * fb_height * 4);
        if (!wp_scaled) return;
        wp_scaled_w = fb_width; wp_scaled_h = fb_height;
        uint32_t* wp_ptr = _binary_obj_wallpaper_bin_start;
        uint32_t wp_w = 1024, wp_h = 768;
        for (uint32_t y = 0; y < fb_height; y++) {
            uint32_t sy = (y * wp_h) / fb_height;
            for (uint32_t x = 0; x < fb_width; x++) {
                uint32_t sx = (x * wp_w) / fb_width;
                wp_scaled[y * fb_width + x] = wp_ptr[sy * wp_w + sx];
            }
        }
    }
    memcpy(back_buffer, wp_scaled, fb_width * fb_height * 4);
}

// ---- Blurred wallpaper (password entry screen) ----
// The same full-bleed scaled wallpaper, but run through a 5-tap box blur
// (horizontal then vertical pass) so the panel pops against a soft backdrop.
// Built once per resolution and cached, exactly like wp_scaled.
static uint32_t* wp_blurred = NULL;
static uint32_t wp_blur_w = 0, wp_blur_h = 0;

static void build_blur(void) {
    // Separable 5-tap box blur as two sliding-window passes. Each pass is O(n)
    // (a running sum per row / per column, 2 reads per pixel) and strictly
    // row-major, so even under slow emulation the cache holds up.
    uint32_t* tmp = (uint32_t*)kmalloc(fb_width * fb_height * 4);
    if (!tmp || !wp_scaled) return;
    int w = (int)fb_width, h = (int)fb_height;
    const uint32_t* src = wp_scaled;

    // ---- Horizontal pass: sliding window [x-2 .. x+2] ----
    for (int y = 0; y < h; y++) {
        const uint32_t* row = src + (uint32_t)y * w;
        uint32_t* out = tmp + (uint32_t)y * w;
        // Window at x=0, edges clamped left: {0,0,0,1,2}
        int cr = 3 * ((row[0] >> 16) & 0xFF) + ((row[1] >> 16) & 0xFF) + ((row[2] >> 16) & 0xFF);
        int cg = 3 * ((row[0] >> 8) & 0xFF) + ((row[1] >> 8) & 0xFF) + ((row[2] >> 8) & 0xFF);
        int cb = 3 * (row[0] & 0xFF) + (row[1] & 0xFF) + (row[2] & 0xFF);
        for (int x = 0; x < w; x++) {
            out[x] = ((uint32_t)(cr / 5) << 16) | ((uint32_t)(cg / 5) << 8) | (uint32_t)(cb / 5);
            if (x + 1 >= w) break;
            uint32_t a = row[(x + 3 >= w) ? w - 1 : x + 3];  // pixel entering
            uint32_t d = row[(x - 2 < 0) ? 0 : x - 2];        // pixel leaving
            cr += (int)((a >> 16) & 0xFF) - (int)((d >> 16) & 0xFF);
            cg += (int)((a >> 8) & 0xFF) - (int)((d >> 8) & 0xFF);
            cb += (int)(a & 0xFF) - (int)(d & 0xFF);
        }
    }

    // ---- Vertical pass: running column sums, window [y-2 .. y+2] ----
    uint32_t* cr = (uint32_t*)kmalloc((uint32_t)w * 4);
    uint32_t* cg = (uint32_t*)kmalloc((uint32_t)w * 4);
    uint32_t* cb = (uint32_t*)kmalloc((uint32_t)w * 4);
    if (!cr || !cg || !cb) {
        kfree(tmp); kfree(cr); kfree(cg); kfree(cb);
        return;
    }
    const uint32_t* r0 = tmp;
    const uint32_t* r1 = tmp + w;
    const uint32_t* r2 = tmp + 2 * w;
    for (int x = 0; x < w; x++) {  // column sums at y=0, edges clamped top: {0,0,1,2}
        cr[x] = 3 * ((r0[x] >> 16) & 0xFF) + ((r1[x] >> 16) & 0xFF) + ((r2[x] >> 16) & 0xFF);
        cg[x] = 3 * ((r0[x] >> 8) & 0xFF) + ((r1[x] >> 8) & 0xFF) + ((r2[x] >> 8) & 0xFF);
        cb[x] = 3 * (r0[x] & 0xFF) + (r1[x] & 0xFF) + (r2[x] & 0xFF);
    }
    for (int y = 0; y < h; y++) {
        uint32_t* out = wp_blurred + (uint32_t)y * w;
        for (int x = 0; x < w; x++)
            out[x] = ((uint32_t)(cr[x] / 5) << 16) | ((uint32_t)(cg[x] / 5) << 8) | (uint32_t)(cb[x] / 5);
        if (y + 1 >= h) break;
        const uint32_t* add = tmp + (uint32_t)((y + 3 >= h) ? h - 1 : y + 3) * w;  // row entering
        const uint32_t* rem = tmp + (uint32_t)((y - 2 < 0) ? 0 : y - 2) * w;        // row leaving
        for (int x = 0; x < w; x++) {
            cr[x] += (int)((add[x] >> 16) & 0xFF) - (int)((rem[x] >> 16) & 0xFF);
            cg[x] += (int)((add[x] >> 8) & 0xFF) - (int)((rem[x] >> 8) & 0xFF);
            cb[x] += (int)(add[x] & 0xFF) - (int)(rem[x] & 0xFF);
        }
    }
    kfree(cr); kfree(cg); kfree(cb);
    kfree(tmp);
}

static void draw_blurred_background(void) {
    if (!is_vbe || fb_width == 0 || fb_height == 0) return;
    if (!wp_scaled) draw_background();   // ensure the sharp copy exists
    if (!wp_scaled) return;
    if (!wp_blurred || wp_blur_w != fb_width || wp_blur_h != fb_height) {
        if (wp_blurred) kfree(wp_blurred);
        wp_blurred = (uint32_t*)kmalloc(fb_width * fb_height * 4);
        if (!wp_blurred) return;
        wp_blur_w = fb_width; wp_blur_h = fb_height;
        build_blur();
    }
    memcpy(back_buffer, wp_blurred, fb_width * fb_height * 4);
}

// ---- Screen transition: cross-fade between the lock and password screens ----
// Opening the panel (or the idle revert closing it) cross-fades the two full
// screens over ~0.5 s. Because the two screens differ only in wallpaper blur
// + captions + panel, a plain lerp between them is simultaneously a blur
// animation (sharp -> blurred), a caption fade and a panel fade — exactly the
// "fade/blur transition" in both directions. prev_frame holds a snapshot of
// the screen we are leaving; the blend is in-place into back_buffer (each
// output pixel only reads its own source pixels). Fixed-point t in 0..256
// (the kernel has no floats).
static uint32_t* prev_frame = NULL;

static int snapshot_prev(void) {
    if (!prev_frame) {
        prev_frame = (uint32_t*)kmalloc(fb_width * fb_height * 4);
        if (!prev_frame) return 0;   // OOM: caller falls back to an instant switch
    }
    memcpy(prev_frame, back_buffer, fb_width * fb_height * 4);
    return 1;
}

static void blend_transition(int t) {
    uint32_t n = fb_width * fb_height;
    uint32_t inv = 256 - t;
    uint32_t* dst = back_buffer;
    const uint32_t* src = prev_frame;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = src[i], b = dst[i];
        uint32_t r = (((a >> 16) & 0xFF) * inv + ((b >> 16) & 0xFF) * t) >> 8;
        uint32_t g = (((a >> 8) & 0xFF) * inv + ((b >> 8) & 0xFF) * t) >> 8;
        uint32_t bl = ((a & 0xFF) * inv + (b & 0xFF) * t) >> 8;
        dst[i] = (r << 16) | (g << 8) | bl;
    }
}

// ---- Shared bottom-left corner: digital clock + full date ----
// Pinned to the bottom-left corner on BOTH screens (lock + password entry),
// so the eye always knows where the time lives. HH:MM:SS with blinking
// colons; below it the full day, date, month and year.
static void draw_clock_corner(void) {
    int scale = 5;
    int gy = (int)fb_height - 180;
    int gx = 40;
    // HH:MM:SS (8 glyphs), colons blink in sync
    for (int i = 0; i < 8; i++) {
        char ch = clock_str[i];
        if (ch == ':') {
            if ((get_ticks() / 500) & 1) gx += draw_char_scale(gx, gy, ':', scale, IC_AMBER_BRT);
            else gx += 8 * scale; // blink: skip the colon
        } else {
            gx += draw_char_scale(gx, gy, ch, scale, IC_AMBER_BRT);
        }
    }
    draw_str_scale(40, gy + 16 * scale + 12, date_str, 2, 3, IC_INK);
}

// ---- Lock screen: full wallpaper, clock + date bottom-left ----
// Deliberately bare: no branding, no hint, no version footer — just the
// wallpaper and the live clock. SPACE (or a click) still opens the panel.
static void draw_lock_screen(void) {
    if (!is_vbe || fb_width == 0 || fb_height == 0) return;
    draw_background();
    refresh_clock();

    draw_clock_corner();
}

// ---- Password entry screen ----
// Panel geometry shared by render and hit-test (one source of truth).
#define LOGIN_PW  380
#define LOGIN_PH  178
#define LOGIN_PY  ((int)(fb_height - LOGIN_PH) / 2 + 56)

// Screen-transition animation. Fixed-point progress 0..256; +10 per draw
// frame (~60 fps) gives ~26 frames ≈ 0.45 s of guest time per direction.
#define TRANS_STEP 10

static void draw_login(int pass_len, int shake, int err, int cap_lock) {
    if (!is_vbe || fb_width == 0 || fb_height == 0) return;

    // Soft blurred backdrop — the panel is now the only thing in focus. The
    // clock + date and the version footer are deliberately NOT drawn here:
    // while typing the password, every caption disappears so nothing competes
    // with the input field.
    draw_blurred_background();
    refresh_clock();

    int shake_off = shake ? ((shake & 1) ? 6 : -6) : 0;
    int cx = (int)fb_width / 2 + shake_off;

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
}

int gui_login() {
    // Password comes from /etc/passwd (see sys_get_password); the hardcoded
    // default is only a fallback for a fresh disk with no file yet.
    char pass[PASSWD_MAX_LEN + 1];
    sys_get_password(pass, (int)sizeof(pass));
    char input[32];
    int idx = 0, shake = 0, err = 0, cap_lock_active = 0;
    int locked = 1;                 // destination screen: 1 = lock, 0 = password
    int panel_shown = 0;            // password screen fully visible (blur ready, idle armed)
    int open_rtc_sec = -1;          // RTC second the panel first rendered (idle anchor)
    int trans_active = 0;           // screen cross-fade in progress
    int trans_open = 0;             // 1 = opening panel, 0 = closing back to lock
    int trans_t = 0;                // fixed-point progress 0..256
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
            if (trans_active) {
                // Render the destination screen, then cross-fade it over the
                // snapshot of the screen we are leaving.
                if (locked) draw_lock_screen();
                else draw_login(idx, shake, err, cap_lock_active);
                blend_transition(trans_t);
                trans_t += TRANS_STEP;
                if (trans_t >= 256) {
                    trans_t = 256;
                    trans_active = 0;
                    if (!locked) {
                        // Panel fully on screen: arm the 4s idle clock. Anchor
                        // to the NEXT whole second (the RTC value truncates
                        // the sub-second fraction, so anchoring to the current
                        // second would shrink the window to as little as ~3 s).
                        panel_shown = 1;
                        open_rtc_sec = (rtc_read_time().second + 1) % 60;
                    } else {
                        // Close finished: reset the gate state for a fresh
                        // password attempt.
                        idx = 0; err = 0; shake = 0;
                        input[0] = '\0';
                    }
                }
            } else if (locked) {
                draw_lock_screen();
            } else {
                draw_login(idx, shake, err, cap_lock_active);
                // The 4s idle clock starts only once the password screen is
                // actually on screen — building the blurred wallpaper can take
                // a while on slow emulation, and that must not count as "idle".
                // Anchored to the RTC (wall time): raw PIT ticks run faster
                // than the wall clock under QEMU TCG, so a tick-counted window
                // would collapse to ~2-3 s there.
                if (!panel_shown) {
                    panel_shown = 1;
                    open_rtc_sec = (rtc_read_time().second + 1) % 60;
                }
            }
            extern int cursor_draw_x, cursor_draw_y;
            cursor_draw_x = mouse_x;
            cursor_draw_y = mouse_y;
            swap_buffers();
            if (shake > 0) shake--;
        }

        // ---- 4-second idle revert: no typing on the password screen falls
        // back to the lock screen (clock + date view), through the close
        // cross-fade. Measured in RTC seconds (wall time), so it means 4 real
        // seconds on KVM, TCG and hardware alike — PIT ticks drift against
        // the wall clock under TCG.
        if (!locked && panel_shown && rtc_wall_sec >= 0) {
            int elapsed = rtc_wall_sec - open_rtc_sec;
            if (elapsed < 0) elapsed += 60;    // RTC seconds wrap at 60
            if (elapsed > 30) elapsed -= 60;   // anchor can be 1 s "ahead" of the
            if (elapsed >= 4) {                // stale cache across a minute roll
                locked = 1;
                panel_shown = 0;
                open_rtc_sec = -1;
                if (snapshot_prev()) {
                    trans_active = 1; trans_open = 0; trans_t = 0;
                }
                // (if the snapshot failed, the switch is simply instant)
            }
        }

        // ---- State machine ----
        uint8_t kbd_mods = 0;
        uint8_t sc = k_get_scancode_ex(&kbd_mods);
        if (sc != 0 && sc < 0x80) {
            if (trans_active && !trans_open) {
                // Fading back to the lock screen — the user came back. Cancel
                // the close, return to the panel and restart the idle clock;
                // this key is consumed (it is the "wake" key, like the
                // dismissing SPACE).
                trans_active = 0;
                locked = 0;
                panel_shown = 1;
                open_rtc_sec = (rtc_read_time().second + 1) % 60;
            } else if (locked) {
                // Only SPACE (0x39) opens the password entry — anything else
                // is ignored so the lock screen stays clean. The dismissing
                // press is consumed so it never reaches the password field;
                // only the *next* keypress types (Windows behavior).
                if (sc == 0x3A) cap_lock_active = !cap_lock_active;
                if (sc == 0x39) {
                    locked = 0;
                    panel_shown = 0;
                    open_rtc_sec = -1;
                    if (snapshot_prev()) {
                        trans_active = 1; trans_open = 1; trans_t = 0;
                    }
                }
            } else {
                // Any key resets the idle window; re-anchor to the cached
                // RTC second (refresh_clock keeps it fresh, ≤~0.5 s stale).
                if (rtc_wall_sec >= 0) open_rtc_sec = rtc_wall_sec;
                char c = scancode_to_char_mods(sc, kbd_mods);
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

        if (trans_active && !trans_open) {
            // Same wake logic as the key path: a click during the close fade
            // cancels it and brings the panel back.
            if ((mouse_btn & 1) && now >= click_ignore_until) {
                click_ignore_until = now + 300;
                trans_active = 0;
                locked = 0;
                panel_shown = 1;
                open_rtc_sec = (rtc_read_time().second + 1) % 60;
            }
        } else if (locked) {
            // A left click also opens the password entry; ignore the held
            // button for a beat so it cannot immediately trigger Sign In.
            if ((mouse_btn & 1) && now >= click_ignore_until) {
                click_ignore_until = now + 300;
                locked = 0;
                panel_shown = 0;
                open_rtc_sec = -1;
                if (snapshot_prev()) {
                    trans_active = 1; trans_open = 1; trans_t = 0;
                }
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

        // Drain the NIC so the DHCP client can complete (or time out into the
        // static fallback) before the user even logs in — the main loop's
        // net_poll() does not run until after gui_login() returns. Same task-0
        // context as the main loop, so no new RX/TX contention.
        extern void net_poll(void);
        net_poll();

        __asm__ __volatile__ ("hlt");
    }
}
