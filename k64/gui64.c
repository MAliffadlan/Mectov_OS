/* M9 GUI-1: the desktop shell — wallpaper, bars, a window, a mouse cursor.
 *
 * Scope: the 64-bit port has run headless since M1. M8 gave it a text console
 * on the framebuffer; this file gives it a desktop: a wallpaper, a title bar,
 * a taskbar, one window whose client area IS the M8 console (re-homed, so the
 * boot log and the live tick output keep streaming inside the window), and a
 * software mouse cursor composited over all of it. No window manager, no
 * widgets, no Ring-3 clients yet — those need a framebuffer syscall and a WM,
 * which is the next milestone.
 *
 * Look: the palette is the 32-bit kernel's instrument-console theme
 * (src/gui/login.c: charcoal + phosphor amber), the glyphs are the shared
 * 8x16 table, and the cursor sprite is the 32-bit desktop's 16x24 arrow with
 * its 0x111111 outline / white fill — so the two kernels look like the same
 * machine. Strings stay ASCII: the font covers CP437, but the kernel writes
 * UTF-8 bytes for anything above 0x7F, which would land on the wrong glyphs.
 *
 * Compositing rule (the only non-obvious part): everything else draws
 * directly into the framebuffer, so the cursor keeps a save-under buffer of
 * its 17x25 footprint. It is restored on every move, and console64 notifies
 * us (cons_set_dirty_hook) whenever text lands under it — then the backdrop
 * is RE-READ from the new pixels before the arrow is repainted. Re-reading
 * (instead of restoring first) is what keeps a freshly drawn glyph from being
 * wiped by a stale save. Draw order at boot: chrome, then the console
 * re-home, then the cursor.
 *
 * Cost: the wallpaper is ~786K pixels of UC writes at 1024x768x32 (one-time,
 * a few tens of ms) and the cursor costs 425 pixels per repaint, so motion
 * stays cheap. The desktop is static otherwise: nothing redraws per frame.
 */
#include "cpu64.h"

/* ---- instrument-console palette (src/gui/login.c) ---- */
#define IC_BG_PANEL 0x0016130Fu /* panel fill (warm charcoal) */
#define IC_LINE 0x002C2821u     /* hairline borders */
#define IC_INK 0x00EDE6D9u      /* primary text */
#define IC_DIM 0x008A8172u      /* secondary text */
#define IC_AMBER 0x00E0A94Fu    /* phosphor amber (accent) */
#define IC_AMBER_BRT 0x00F5C566u
#define IC_GRID 0x001B1712u     /* wallpaper grid hairlines */

/* Wallpaper gradient (top -> bottom, darker than the panels so the chrome
 * reads as raised). */
#define WP_TOP 0x001E1A14u
#define WP_BOT 0x000C0A08u

#define TOPBAR_H 24
#define TASKBAR_H 28
#define WIN_TITLE_H 20
#define WIN_BORDER 1
#define GRID_STEP 64

/* ---- mouse cursor sprite (mirror of src/drivers/vga.c cursor_mask/inner) */
#define CUR_W 16
#define CUR_H 24
#define CUR_SHADOW 1 /* arrow is drawn with a 1px offset shadow */

static const u16 cursor_mask[CUR_H] = {
    0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00,
    0xFF80, 0xFFC0, 0xFFE0, 0xFFF0, 0xFFF8, 0xFFC0, 0xFF80, 0xE3C0,
    0xC3C0, 0x83C0, 0x01E0, 0x01E0, 0x00E0, 0x0000, 0x0000, 0x0000,
};
static const u16 cursor_inner[CUR_H] = {
    0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00,
    0x7F80, 0x7FC0, 0x7FE0, 0x7FF0, 0x7FF8, 0x7FC0, 0x7F80, 0x6380,
    0x4380, 0x0380, 0x01C0, 0x01C0, 0x00C0, 0x0000, 0x0000, 0x0000,
};

static u32 cur_save[(CUR_W + CUR_SHADOW) * (CUR_H + CUR_SHADOW)];
static int cur_x = 400, cur_y = 300; /* 32-bit boot position */
static int cur_visible = 0;
/* Enabled only once the chrome is fully drawn: before that, console redraws
 * during gui64_init would composite the arrow over a half-built desktop. */
static int cur_enabled = 0;
static int cur_moved_reported = 0;

/* ---- tiny string builder (gfx_text takes strings; the kernel has no
 * snprintf). Both helpers append FORWARD and return the new end, so calls
 * chain: p = put_u64(v, p). ---- */
static char *put_u64(u64 v, char *out) {
    char tmp[21];
    int n = 0;
    if (!v) tmp[n++] = '0';
    while (v && n < 20) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) *out++ = tmp[--n];
    return out;
}
static char *put_str(char *out, const char *s) {
    while (*s) *out++ = *s++;
    return out;
}

/* ---- mouse cursor compositing ---- */
static void cursor_hide(void) {
    if (!cur_visible) return;
    int n = 0;
    for (int j = 0; j < CUR_H + CUR_SHADOW; j++)
        for (int i = 0; i < CUR_W + CUR_SHADOW; i++, n++) {
            u32 c = cur_save[n];
            gfx_px(cur_x + i, cur_y + j, c);
        }
    cur_visible = 0;
}

static void cursor_draw(void) {
    int n = 0;
    for (int j = 0; j < CUR_H + CUR_SHADOW; j++)
        for (int i = 0; i < CUR_W + CUR_SHADOW; i++, n++)
            cur_save[n] = gfx_read(cur_x + i, cur_y + j);
    /* shadow first (built from the saved backdrop), then the arrow on top */
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++)
            if (cursor_mask[j] & (u16)(0x8000 >> i)) {
                u32 bg = cur_save[(j + CUR_SHADOW) * (CUR_W + CUR_SHADOW) +
                                  i + CUR_SHADOW];
                u32 sh = (((bg >> 16) & 0xFF) * 140 >> 8) << 16 |
                         (((bg >> 8) & 0xFF) * 140 >> 8) << 8 |
                         ((bg & 0xFF) * 140 >> 8);
                gfx_px(cur_x + i + CUR_SHADOW, cur_y + j + CUR_SHADOW, sh);
            }
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++)
            if (cursor_mask[j] & (u16)(0x8000 >> i)) {
                int fill = cursor_inner[j] & (u16)(0x8000 >> i);
                gfx_px(cur_x + i, cur_y + j, fill ? 0x00FFFFFFu : 0x00111111u);
            }
    cur_visible = 1;
}

static void cursor_clamp(void) {
    int maxx = gfx_width() - (CUR_W + CUR_SHADOW);
    int maxy = gfx_height() - (CUR_H + CUR_SHADOW);
    if (maxx < 0) maxx = 0;
    if (maxy < 0) maxy = 0;
    if (cur_x < 0) cur_x = 0;
    if (cur_y < 0) cur_y = 0;
    if (cur_x > maxx) cur_x = maxx;
    if (cur_y > maxy) cur_y = maxy;
}

/* console64 calls this for every changed rectangle: keep the sprite on top
 * of text that landed under it (see the file header for why we re-read).
 * Note this must repaint even when the sprite is currently lifted: the
 * console's pre-hide hook takes it down before a scroll, and this post-hook
 * is what puts it back. */
void gui64_dirty(int x0, int y0, int x1, int y1) {
    if (!cur_enabled) return;
    if (x1 < cur_x || x0 > cur_x + CUR_W + CUR_SHADOW) return;
    if (y1 < cur_y || y0 > cur_y + CUR_H + CUR_SHADOW) return;
    cursor_hide(); /* no-op when already lifted */
    cursor_draw();
}

/* IRQ12 calls this after the driver updated its position. */
static void gui64_on_move(int dx, int dy) {
    (void)dx;
    (void)dy;
    if (!gfx_ready()) return;
    cursor_hide();
    cur_x = mouse64_x();
    cur_y = mouse64_y();
    cursor_clamp();
    cursor_draw();
    if (!cur_moved_reported) {
        cur_moved_reported = 1;
        s_printf("[K64] gui: cursor moved to %u,%u\n", (u64)cur_x, (u64)cur_y);
    }
}

/* ---- chrome ---- */
static void draw_wallpaper(int w, int h) {
    gfx_vgrad(0, 0, w, h, WP_TOP, WP_BOT);
    for (int x = GRID_STEP; x < w; x += GRID_STEP) gfx_vline(x, 0, h, IC_GRID);
    for (int y = GRID_STEP; y < h; y += GRID_STEP) gfx_hline(0, y, w, IC_GRID);
}

static void draw_topbar(int w) {
    gfx_fill(0, 0, w, TOPBAR_H, IC_BG_PANEL);
    gfx_hline(0, TOPBAR_H - 1, w, IC_LINE);
    gfx_text(12, 4, "MECTOV OS 64", IC_AMBER, -1);
    gfx_text(12 + 13 * 8, 4, "M9 GUI-1", IC_DIM, -1);

    char buf[64];
    char *p = put_u64((u64)gfx_width(), buf);
    p = put_str(p, "x");
    p = put_u64((u64)gfx_height(), p);
    p = put_str(p, " framebuffer");
    *p = 0;
    gfx_text(w - 12 - (int)(p - buf) * 8, 4, buf, IC_DIM, -1);
}

static void draw_taskbar(int w, int h) {
    int y = h - TASKBAR_H;
    gfx_fill(0, y, w, TASKBAR_H, IC_BG_PANEL);
    gfx_hline(0, y, w, IC_LINE);
    gfx_fill(10, y + 7, 14, 14, IC_AMBER);
    gfx_text(34, y + 6, "START", IC_AMBER_BRT, -1);
    gfx_vline(34 + 6 * 8 + 6, y + 6, 16, IC_LINE);

    char buf[64];
    char *p = put_str(buf, "SMP ");
    p = put_u64((u64)smp_cpu_count(), p);
    p = put_str(p, " cpu   uptime ");
    p = put_u64((u64)k64_ticks() / 100, p); /* PIT is 100 Hz */
    p = put_str(p, "s");
    *p = 0;
    gfx_text(w - 12 - (int)(p - buf) * 8, y + 6, buf, IC_DIM, -1);
}

static void draw_window(int wx, int wy, int ww, int wh) {
    /* drop shadow, then the frame: charcoal title bar over a darker client */
    gfx_fill(wx + 5, wy + 5, ww, wh, 0x00080604u);
    gfx_fill(wx, wy, ww, wh, IC_BG_PANEL);
    gfx_frame(wx, wy, ww, wh, IC_LINE);
    gfx_fill(wx + WIN_BORDER, wy + WIN_BORDER, ww - 2 * WIN_BORDER,
             WIN_TITLE_H, 0x001F1B15u);
    gfx_hline(wx, wy + WIN_TITLE_H, ww, IC_LINE);
    static const char title[] = "console - mectov64";
    gfx_text(wx + 8, wy + 2, title, IC_INK, -1);
    gfx_text(wx + 8 + (int)(sizeof(title) - 1) * 8 + 8, wy + 2,
             "(M8 text, windowed)", IC_DIM, -1);
    gfx_text(wx + ww - 14, wy + 2, "x", IC_DIM, -1);
}

int gui64_init(void) {
    if (!gfx_ready()) {
        s_puts("[K64] gui: no framebuffer, desktop skipped\n");
        return 0;
    }
    int w = gfx_width(), h = gfx_height();
    int ww = w - 2 * 72;
    int wh = h - TOPBAR_H - TASKBAR_H - 2 * 56;
    int wx = 72, wy = TOPBAR_H + 56;
    if (ww < 160 || wh < 80) { /* absurdly small panel: shrink the margins */
        wx = 8;
        wy = TOPBAR_H + 8;
        ww = w - 16;
        wh = h - TOPBAR_H - TASKBAR_H - 16;
    }
    int cx = wx + WIN_BORDER;
    int cy = wy + WIN_TITLE_H + WIN_BORDER;
    int cw = ww - 2 * WIN_BORDER;
    int ch = wh - WIN_TITLE_H - 2 * WIN_BORDER;

    /* ONE critical section for the whole transition. APs are already running
     * the demos and print through the same console, and a print landing
     * mid-draw would scroll the half-built desktop away (the console's view
     * is still the full screen until the re-home below). Nothing in here may
     * print: the lock is a plain spinlock and the log lines come after. */
    u64 lock = console_lock();
    draw_wallpaper(w, h);
    draw_topbar(w);
    draw_taskbar(w, h);
    draw_window(wx, wy, ww, wh);
    /* The terminal window's client area becomes the console's view; it keeps
     * the tail of the boot log and keeps streaming into the window. */
    cons_set_colors(IC_INK, IC_BG_PANEL);
    cons_set_dirty_hook(gui64_dirty);
    cons_set_hide_hook(cursor_hide); /* lift the arrow before scrolls/wipes */
    int rehomed = cons_rehome((u32)cx, (u32)cy, (u32)cw, (u32)ch);
    int vcols = 0, vrows = 0;
    cons_view_cells(&vcols, &vrows);
    /* cursor last: it must sit on top of everything drawn above. */
    cur_x = mouse64_x();
    cur_y = mouse64_y();
    cursor_clamp();
    cur_enabled = 1;
    cursor_draw();
    mouse64_set_move_hook(gui64_on_move);
    console_unlock(lock);

    if (!rehomed)
        s_puts("[K64] gui: console rehome failed (M9 continues without it)\n");
    else
        s_printf("[K64] cons: rehomed %ux%u cells at %u,%u (window %ux%u)\n",
                 (u64)vcols, (u64)vrows, (u64)cx, (u64)cy, (u64)cw, (u64)ch);
    s_printf("[K64] gui: desktop up win=%u,%u,%u,%u client=%u,%u,%u,%u "
             "topbar=%u taskbar=%u cursor=%u,%u\n",
             (u64)wx, (u64)wy, (u64)ww, (u64)wh, (u64)cx, (u64)cy, (u64)cw,
             (u64)ch, (u64)TOPBAR_H, (u64)TASKBAR_H, (u64)cur_x, (u64)cur_y);
    return 1;
}
