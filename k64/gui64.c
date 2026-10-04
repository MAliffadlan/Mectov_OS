/* M14: the desktop shell — what the kernel puts on screen, on top of wm64.
 *
 * M9's gui64.c WAS the desktop: it drew the wallpaper, both bars, one window
 * and the composited cursor itself, straight into the framebuffer. All of that
 * moved into the window manager (k64/wm64.c) when the desktop grew a second
 * window, because a fixed layout cannot answer "what was behind this window?".
 * What is left here is the shell's own policy:
 *
 *   - open the terminal window: the M8 text console re-homed into a client
 *     area, so the boot log's tail and everything printed afterwards streams
 *     inside it (cons_rehome carries the grid, the dirty hook reports each
 *     changed rect to the compositor);
 *   - open the live system window (SMP/tick/heap/frames counters);
 *   - wire the mouse into the WM: move packets drive hover + drag, button
 *     edges drive click/raise/minimize/close;
 *   - print the geometry markers the screenshot gates grep for.
 *
 * The console hook is the only hot path: every kernel byte reaches
 * cons_putc() -> gui64_dirty() -> wm64_invalidate(), which composites and
 * presents just that rect. A cell is 8x16, so a boot log costs a few hundred
 * pixels per line.
 *
 * Note what is NOT here any more: the console lock around the desktop draw.
 * M9 needed it because the console's view was still full-screen while the
 * chrome was being painted, so an AP printing mid-draw would scroll the
 * half-built desktop away. Drawing now lands in the compositor's surface, and
 * a print that races the transition is erased by the very damage it reports
 * (the rect it damaged is not inside the terminal window's client). */
#include "cpu64.h"

/* M9 geometry, kept verbatim: the screenshot gates sample these margins. */
#define WIN_MARGIN_X 72
#define WIN_MARGIN_Y 56
#define TOPBAR_H 24
#define TASKBAR_H 28

static int gui_on;
static int con_id = -1;
static int sys_id = -1;

/* Text with a clip, for a window client: the WM hands us the damaged rect and
 * a callback must not paint outside it (a stray pixel would land on top of a
 * window the running composite pass is not repainting). */
static void text_clip(int cx, int cy, int cw, int ch, int x, int y,
                      const char *s, u32 fg) {
    int x1 = cx + cw - 1, y1 = cy + ch - 1;
    for (; s && *s; s++, x += 8) {
        if (x + 8 <= cx || x > x1) continue;
        if (y + 16 <= cy || y > y1) continue;
        gfx_cell(x, y, (unsigned char)*s, fg, -1);
    }
}
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
/* "label   value unit" row helper: pads the label to 8 characters. */
static int row(int cx, int cy, int cw, int ch, int x, int y, const char *label,
               u64 v, const char *unit, u32 fg) {
    char buf[64];
    char *p = put_str(buf, label);
    while ((int)(p - buf) < 8) *p++ = ' ';
    p = put_u64(v, p);
    if (unit) p = put_str(p, unit);
    *p = 0;
    text_clip(cx, cy, cw, ch, x, y, buf, fg);
    return y + 16;
}

/* The live system window: re-rendered from live counters whenever the
 * compositor needs its pixels (and once a second via wm64_set_live). */
static void sysmon_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    kmem64_t km;
    heap64_stats(&km);
    int x = cx + 8;
    int y = cy + 6;
    y = row(cx, cy, cw, ch, x, y, "cpu", (u64)smp_cpu_count(), " online", 0x00EDE6D9u);
    y = row(cx, cy, cw, ch, x, y, "tick", k64_ticks(), "", 0x008A8172u);
    y = row(cx, cy, cw, ch, x, y, "secs", k64_ticks() / 100, "", 0x008A8172u);
    y = row(cx, cy, cw, ch, x, y, "heap", km.arena_used / 1024, " KB", 0x008A8172u);
    y = row(cx, cy, cw, ch, x, y, "fs", (u64)fs64_mounts(), " mounts", 0x008A8172u);
    y = row(cx, cy, cw, ch, x, y, "free", pmm_free_frames(), " frames", 0x00E0A94Fu);
    /* A bar so a covered/uncovered repaint is obvious in a screenshot. */
    int bw = cw - 16;
    if (bw > 0) {
        u64 t = k64_ticks() % 100;
        int fill = (int)((t * (u64)bw) / 100);
        if (fill > 0) gfx_fill(x, y, fill, 6, 0x00E0A94Fu);
        if (fill < bw) gfx_fill(x + fill, y, bw - fill, 6, 0x002C2821u);
    }
}

/* The terminal window's client is the text console: the compositor repaints it
 * from the shadow grid (see cons_repaint_rect), so this window has no callback
 * of its own. It exists as a window so it can be dragged, covered, minimized
 * and closed like any other. */

int gui64_init(void) {
    if (!gfx_ready()) {
        s_puts("[K64] gui: no framebuffer, desktop skipped\n");
        return 0;
    }
    if (!wm64_init()) return 0;

    /* From here the pixels on screen belong to the compositor, but the console
     * still has a full-screen view and the M8 grey palette — see c_quiet in
     * console64.c. Quieting it for the length of the transition keeps stray
     * grey glyphs out of the back buffer; the re-home at the end of it clears
     * the flag again and repaints from the grid, so the terminal window opens
     * on the boot log's tail exactly as before. */
    cons_set_quiet(1);

    int w = gfx_width(), h = gfx_height();
    int ww = w - 2 * WIN_MARGIN_X;
    int wh = h - TOPBAR_H - TASKBAR_H - 2 * WIN_MARGIN_Y;
    int wx = WIN_MARGIN_X, wy = TOPBAR_H + WIN_MARGIN_Y;
    if (ww < 160 || wh < 80) { /* absurdly small panel: shrink the margins */
        wx = 8;
        wy = TOPBAR_H + 8;
        ww = w - 16;
        wh = h - TOPBAR_H - TASKBAR_H - 16;
    }
    /* Terminal window, then the console moved into it. cons_rehome carries the
     * boot log's tail so the window opens showing where the boot got to. */
    con_id = wm64_open(wx, wy, ww, wh, "console - mectov64", 0, 1);
    if (con_id < 0) {
        /* No window to move the console into: it stays full-screen, which is
         * what it already is, so stop quieting it rather than leave the boot
         * log invisible for the rest of the run. */
        cons_set_quiet(0);
        return 0;
    }
    int cx = 0, cy = 0, cw = 0, ch = 0;
    wm64_client_rect(con_id, &cx, &cy, &cw, &ch);

    /* The console transition (recolour + re-home) runs under the console lock,
     * and NOTHING in it may print. This is M9's lesson narrowed to what needs
     * it: cons_rehome writes c_ox/c_oy and c_cols/c_rows as separate stores
     * while another CPU's kernel print walks the same fields, so a print that
     * lands between them draws with the NEW origin and the OLD row count —
     * cells at row 47 of a 36-row view, i.e. below the screen, which in M14 is
     * an unmapped heap page (the back buffer) and a #PF inside the compositor.
     * Seen under KVM at 4 vCPUs, never under TCG, which is what made it look
     * like a scheduler bug at first. */
    int rehomed, vcols = 0, vrows = 0;
    u64 lock = console_lock();
    cons_set_colors(0x00EDE6D9u, 0x0016130Fu); /* IC_INK on IC_BG_PANEL */
    cons_set_dirty_hook(gui64_dirty);
    cons_set_quiet(0); /* the recolour above landed; pixels are welcome again */
    rehomed = cons_rehome((u32)cx, (u32)cy, (u32)cw, (u32)ch);
    cons_view_cells(&vcols, &vrows);
    console_unlock(lock);

    /* Live system window, bottom right: it overlaps the terminal's client on
     * purpose — that overlap is what proves z-order, drag and repaint. */
    int sw = 300, sh = 200;
    if (w < 700 || h < 500) { sw = w / 3; sh = h / 4; }
    sys_id = wm64_open(w - sw - 20, h - TASKBAR_H - sh - 16, sw, sh,
                       "system - mectov64", sysmon_draw, 0);
    wm64_set_live(sys_id, 1);
    /* Opening the second window focused it; the shell wants the terminal
     * focused (and on top) as the default, which also exercises raise. */
    wm64_focus(con_id);

    mouse64_set_move_hook(wm64_on_mouse_move);
    mouse64_set_button_hook(wm64_on_mouse_button);
    gui_on = 1;

    if (!rehomed)
        s_puts("[K64] gui: console rehome failed (M14 continues without it)\n");
    else
        s_printf("[K64] cons: rehomed %ux%u cells at %u,%u (window %ux%u)\n",
                 (u64)vcols, (u64)vrows, (u64)cx, (u64)cy, (u64)cw, (u64)ch);
    s_printf("[K64] gui: desktop up win=%u,%u,%u,%u client=%u,%u,%u,%u "
             "topbar=%u taskbar=%u cursor=%u,%u\n",
             (u64)wx, (u64)wy, (u64)ww, (u64)wh, (u64)cx, (u64)cy, (u64)cw,
             (u64)ch, (u64)TOPBAR_H, (u64)TASKBAR_H, (u64)wm64_cursor_x(),
             (u64)wm64_cursor_y());
    wm64_marker();
    return 1;
}

/* Console output -> compositor damage. Runs under the console lock (the caller
 * has just written a cell), so it must not print or take another lock: it does
 * exactly one thing, hand the rect to the WM. */
void gui64_dirty(int x0, int y0, int x1, int y1) {
    if (!gui_on) return;
    wm64_invalidate(x0, y0, x1, y1);
}

/* BSP tick (1 Hz, see task64.c): refresh the taskbar status text and the live
 * windows. No serial output here: this runs inside the timer IRQ. */
void gui64_tick(void) {
    if (!gui_on) return;
    wm64_tick();
}
