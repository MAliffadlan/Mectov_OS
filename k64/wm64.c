/* M14: kernel window manager — the desktop's window system.
 *
 * M9 drew a fixed desktop (wallpaper, bars, ONE window) straight into the
 * framebuffer and kept the mouse cursor consistent with a save-under buffer.
 * That cannot host a second window: nothing could repaint what a window
 * uncovered, and any bulk pixel move had to remember to lift the cursor first
 * (see the M9 notes in k64/gui64.c — that ordering rule was the whole design).
 *
 * This file replaces both halves of the problem with one mechanism: the
 * compositor. Every draw goes into the M14 screen back buffer (gfx64), and a
 * *damaged rect* is resolved by re-compositing the full stack inside it —
 * wallpaper, top bar, taskbar, the windows in z-order, then the cursor — and
 * presenting that rect to the panel. Consequences worth naming, because they
 * are the point of the design:
 *
 *  - Overlapping windows work, at any depth, because "what is under this rect"
 *    is a function, not a saved copy.
 *  - The cursor cannot ghost or be overwritten by text. It is drawn last in
 *    every composite, so the M9 hide-before-scroll protocol is gone entirely
 *    (console64's hide hook is simply not wired any more).
 *  - A window is a rectangle plus a repaint callback. Any window must be able
 *    to draw its client from its own state at any time: the console repaints
 *    from its shadow grid, a live counter window from its counters. Ring-3
 *    clients will need a canvas instead (M15) — a callback cannot be asked to
 *    run from an IRQ-driven composite.
 *
 * Damage accounting is deliberately small: up to DMG_MAX rects, merged on
 * insert when they touch. A drag invalidates the old and the new window rect
 * each packet and the two merge into one, so a drag costs one composite of a
 * window-sized rect plus one present per mouse packet — bounded work, no
 * full-screen repaint unless windows are spread across the screen.
 *
 * Locking: invalidation/compositing take wm64_lock (irqsave), never the
 * console lock. The console's dirty hook calls in from under the console lock,
 * so a lock order of console -> wm is the only direction that exists; the
 * compositor itself takes no other lock and must never print.
 */
#include "cpu64.h"
#include "spin64.h"

/* ---- instrument-console palette (src/gui/login.c, as M9) ---- */
#define IC_BG_PANEL 0x0016130Fu /* panel fill (warm charcoal) */
#define IC_TITLE 0x001F1B15u    /* title bar (a shade up from the panel) */
#define IC_LINE 0x002C2821u     /* hairline borders */
#define IC_INK 0x00EDE6D9u      /* primary text */
#define IC_DIM 0x008A8172u      /* secondary text */
#define IC_AMBER 0x00E0A94Fu    /* phosphor amber (accent) */
#define IC_AMBER_BRT 0x00F5C566u
#define IC_GRID 0x001B1712u     /* wallpaper grid hairlines */
#define IC_SHADOW 0x00080604u   /* window drop shadow */
#define IC_FOCUS_LINE 0x00A8874Fu

/* Wallpaper gradient (top -> bottom, darker than the panels so the chrome
 * reads as raised). */
#define WP_TOP 0x001E1A14u
#define WP_BOT 0x000C0A08u

#define TOPBAR_H 24
#define TASKBAR_H 28
#define TITLE_H 20
#define BORDER 1
#define SHADOW 5
#define BTN_W 16
#define BTN_H 16
#define GRID_STEP 64

/* Desktop margins for wm64_default_rect(): the console window keeps the M9
 * geometry so the desktop screenshot gates (scripts/gui_test.py) keep their
 * sample points. */
#define WIN_MARGIN_X 72
#define WIN_MARGIN_Y 56

#define CUR_W 16
#define CUR_H 24
#define CUR_SHADOW 1

/* The terminal window's client is a text grid; its cells are the console's
 * (k64/console64.c CONS_CELL_*), and repainting works in whole cells. See
 * align_to_console_cells() for why every rect that touches it is snapped. */
#define CON_CELL_W 8
#define CON_CELL_H 16

#define DMG_MAX 24
#define STATUS_MAX 80
#define TB_BTN_W 132
#define TB_BTN_GAP 4
#define TB_BTN_X0 96 /* right of the START box, as M9 laid it out */

/* ---- mouse cursor sprite (mirror of src/drivers/vga.c) ---- */
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

typedef struct {
    int x0, y0, x1, y1; /* inclusive */
} rect64_t;

static win64_t wins[WM64_MAX_WIN];
static int zorder[WM64_MAX_WIN]; /* bottom -> top */
static int zcount;
static int focused = -1;
static int next_id = 1;
static int ready;
static int cur_x = 400, cur_y = 300; /* 32-bit kernel's boot position */
static int cur_moved_reported;
static int drag_id = -1;
static char status[STATUS_MAX];
static int status_len;
static int status_x0 = -1; /* left edge of the last status text, for erasing */

static rect64_t dmg[DMG_MAX];
static int ndmg;
static int in_flush; /* inside wm64_flush: drawing is the repair, so new
                      * damage reports from the repaint callbacks are dropped
                      * (cons_repaint() notifies, and would loop forever) */
static spin64_t wm64_lock = SPIN64_INIT;

/* ---- tiny rect helpers ---- */
static int r_empty(const rect64_t *r) { return r->x1 < r->x0 || r->y1 < r->y0; }
static int r_hit(const rect64_t *r, int x, int y) {
    return x >= r->x0 && x <= r->x1 && y >= r->y0 && y <= r->y1;
}
static int r_overlap(const rect64_t *a, const rect64_t *b) {
    return !(a->x1 < b->x0 || b->x1 < a->x0 || a->y1 < b->y0 || b->y1 < a->y0);
}
static void r_union(rect64_t *a, const rect64_t *b) {
    if (b->x0 < a->x0) a->x0 = b->x0;
    if (b->y0 < a->y0) a->y0 = b->y0;
    if (b->x1 > a->x1) a->x1 = b->x1;
    if (b->y1 > a->y1) a->y1 = b->y1;
}
static rect64_t r_full(void) {
    rect64_t r = { 0, 0, gfx_width() - 1, gfx_height() - 1 };
    return r;
}
static rect64_t r_clip(rect64_t r) {
    rect64_t s = r_full();
    if (r.x0 < s.x0) r.x0 = s.x0;
    if (r.y0 < s.y0) r.y0 = s.y0;
    if (r.x1 > s.x1) r.x1 = s.x1;
    if (r.y1 > s.y1) r.y1 = s.y1;
    return r;
}

/* ---- string builder (the kernel has no snprintf) ---- */
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

/* ---- window geometry ---- */
static void client_of(const win64_t *w, rect64_t *r) {
    r->x0 = w->x + BORDER;
    r->y0 = w->y + TITLE_H + BORDER;
    r->x1 = w->x + w->w - BORDER - 1;
    r->y1 = w->y + w->h - BORDER - 1;
}
static void outer_of(const win64_t *w, rect64_t *r) {
    r->x0 = w->x;
    r->y0 = w->y;
    r->x1 = w->x + w->w - 1;
    r->y1 = w->y + w->h - 1;
}
/* Title-bar buttons, right to left: close, maximize, minimize. */
static void btn_of(const win64_t *w, int which, rect64_t *r) {
    int slot = (which == WM64_BTN_CLOSE) ? 0
             : (which == WM64_BTN_MAX) ? 1 : 2;
    int x = w->x + w->w - BORDER - 2 - BTN_W - slot * (BTN_W + 2);
    r->x0 = x;
    r->x1 = x + BTN_W - 1;
    r->y0 = w->y + BORDER + 1;
    r->y1 = r->y0 + BTN_H - 1;
}
static void title_of(const win64_t *w, rect64_t *r) {
    r->x0 = w->x + BORDER;
    r->y0 = w->y + BORDER;
    r->x1 = w->x + w->w - BORDER - 1;
    r->y1 = w->y + TITLE_H - 1;
}
/* The desktop band a window may occupy: between the two bars. */
static void desk_band(rect64_t *r) {
    r->x0 = 0;
    r->y0 = TOPBAR_H;
    r->x1 = gfx_width() - 1;
    r->y1 = gfx_height() - TASKBAR_H - 1;
}
static void clamp_window(win64_t *w) {
    rect64_t band;
    desk_band(&band);
    if (w->w > band.x1 - band.x0 + 1) w->w = band.x1 - band.x0 + 1;
    if (w->h > band.y1 - band.y0 + 1) w->h = band.y1 - band.y0 + 1;
    if (w->x < band.x0) w->x = band.x0;
    if (w->y < band.y0) w->y = band.y0;
    if (w->x + w->w - 1 > band.x1) w->x = band.x1 - w->w + 1;
    if (w->y + w->h - 1 > band.y1) w->y = band.y1 - w->h + 1;
}
/* Topmost non-minimized window containing the point (chrome included).
 * Returns the window ID, not the table slot: zorder stores slots, and every
 * caller of this one speaks ids (it feeds focus, tb_slot_index and the drag
 * state). Returning the slot here made the first click on the terminal report
 * "focus id=0" and then index wins[-1] — a slot/id mixup that is silent until
 * a slot happens to differ from its id. */
static int win_at(int x, int y) {
    for (int zi = zcount - 1; zi >= 0; zi--) {
        win64_t *w = &wins[zorder[zi]];
        rect64_t r;
        outer_of(w, &r);
        if (!w->used || w->minimized) continue;
        if (r_hit(&r, x, y)) return w->id;
    }
    return -1;
}
/* Taskbar button slot for window index i (all windows get one, minimized
 * ones greyed): the button is how a minimized window comes back. */
static int tb_slot_at(int x, int y, int *slot_out) {
    int y0 = gfx_height() - TASKBAR_H + 5;
    if (y < y0 || y >= y0 + (TASKBAR_H - 10)) return 0;
    for (int i = 0; i < WM64_MAX_WIN; i++) {
        if (!wins[i].used) continue;
        int bx = TB_BTN_X0 + i * (TB_BTN_W + TB_BTN_GAP);
        if (x >= bx && x < bx + TB_BTN_W) {
            if (slot_out) *slot_out = i;
            return 1;
        }
    }
    return 0;
}
static int tb_slot_index(int id) {
    for (int i = 0; i < WM64_MAX_WIN; i++)
        if (wins[i].used && wins[i].id == id) return i;
    return -1;
}

/* ---- damage ---- */
static void dmg_merge_into(int idx, const rect64_t *r) {
    r_union(&dmg[idx], r);
    /* absorb anything the merge now touches, so the list stays short */
    for (int i = 0; i < ndmg; i++) {
        if (i == idx) continue;
        if (r_overlap(&dmg[idx], &dmg[i])) {
            r_union(&dmg[idx], &dmg[i]);
            dmg[i] = dmg[ndmg - 1];
            ndmg--;
            i = -1; /* restart: the union may touch another rect */
        }
    }
}
static void dmg_add_locked(rect64_t r) {
    r = r_clip(r);
    if (r_empty(&r)) return;
    if (ndmg < DMG_MAX) {
        for (int i = 0; i < ndmg; i++) {
            if (r_overlap(&dmg[i], &r)) {
                dmg_merge_into(i, &r);
                return;
            }
        }
        dmg[ndmg++] = r;
        return;
    }
    /* Full list: collapse everything into one rect. Correct, and only reachable
     * when damage is scattered across the screen anyway. */
    for (int i = 1; i < ndmg; i++) r_union(&dmg[0], &dmg[i]);
    ndmg = 1;
    dmg_merge_into(0, &r);
}

/* Resolve a damaged rect: repaint the whole stack inside it, then present it. */
static void composite(rect64_t c);

/* Floor/ceiling division that works for negative numerators (a damage rect can
 * start left of the console's view). */
static int fdiv_s(int a, int b) {
    int q = a / b;
    if ((a % b) != 0 && ((a < 0) != (b < 0))) q--;
    return q;
}

/* The console repaints WHOLE cells from its shadow grid, so a damaged rect
 * that cuts a cell in half makes it draw outside itself — one pixel column
 * over the terminal's own frame, or over a window next to it that this pass
 * is not compositing. Damage that touches the terminal's client is therefore
 * expanded to the console's cell grid first: the repaint then lands exactly on
 * the presented rect, and the extra pixels are presented too (they are part of
 * the same frame). This is why the check "a callback must draw inside its
 * clip" is satisfiable at all for a text client. */
static void align_to_console_cells(rect64_t *r) {
    int idx = -1;
    for (int i = 0; i < WM64_MAX_WIN; i++)
        if (wins[i].used && wins[i].console && !wins[i].minimized) idx = i;
    if (idx < 0) return;
    rect64_t cl;
    client_of(&wins[idx], &cl);
    if (!r_overlap(r, &cl)) return;
    int x0 = cl.x0 + fdiv_s(r->x0 - cl.x0, CON_CELL_W) * CON_CELL_W;
    int y0 = cl.y0 + fdiv_s(r->y0 - cl.y0, CON_CELL_H) * CON_CELL_H;
    int x1 = cl.x0 + (fdiv_s(r->x1 - cl.x0, CON_CELL_W) + 1) * CON_CELL_W - 1;
    int y1 = cl.y0 + (fdiv_s(r->y1 - cl.y0, CON_CELL_H) + 1) * CON_CELL_H - 1;
    if (x0 < cl.x0) x0 = cl.x0;
    if (y0 < cl.y0) y0 = cl.y0;
    if (x1 > cl.x1) x1 = cl.x1;
    if (y1 > cl.y1) y1 = cl.y1;
    if (x0 < r->x0) r->x0 = x0;
    if (y0 < r->y0) r->y0 = y0;
    if (x1 > r->x1) r->x1 = x1;
    if (y1 > r->y1) r->y1 = y1;
    *r = r_clip(*r);
}

/* Present every recorded rect. Called with wm64_lock held. */
static void flush_locked(void) {
    in_flush = 1;
    int guard = 8; /* callbacks must not generate damage, but never spin */
    while (ndmg > 0 && guard-- > 0) {
        rect64_t list[DMG_MAX];
        int n = ndmg;
        for (int i = 0; i < n; i++) list[i] = dmg[i];
        ndmg = 0;
        for (int i = 0; i < n; i++) {
            rect64_t r = list[i];
            align_to_console_cells(&r);
            composite(r);
            gfx_present(r.x0, r.y0, r.x1, r.y1);
        }
        if (ndmg > 0) { /* fold late damage into the leftovers */ }
    }
    ndmg = 0;
    in_flush = 0;
}

void wm64_invalidate(int x0, int y0, int x1, int y1) {
    if (!ready) return;
    rect64_t r = { x0, y0, x1, y1 };
    u64 f = spin64_lock_irqsave(&wm64_lock);
    if (in_flush) { spin64_unlock_irqrestore(&wm64_lock, f); return; }
    dmg_add_locked(r);
    /* Flush from inside the lock: no caller may hold wm64_lock itself, and a
     * nested flush (console print -> composite -> repaint) is blocked by
     * in_flush above. */
    flush_locked();
    spin64_unlock_irqrestore(&wm64_lock, f);
}

/* Record damage without resolving it: the event handlers below collect
 * everything an input event touched and flush ONCE at the end, so a drag does
 * not composite twice per packet. */
static void dmg_record(int x0, int y0, int x1, int y1) {
    if (!ready) return;
    rect64_t r = { x0, y0, x1, y1 };
    u64 f = spin64_lock_irqsave(&wm64_lock);
    if (!in_flush) dmg_add_locked(r);
    spin64_unlock_irqrestore(&wm64_lock, f);
}

void wm64_flush(void) {
    if (!ready) return;
    u64 f = spin64_lock_irqsave(&wm64_lock);
    flush_locked();
    spin64_unlock_irqrestore(&wm64_lock, f);
}

/* ---- painting: chrome ---- */
static void paint_wallpaper(rect64_t c) {
    int h = gfx_height();
    for (int y = c.y0; y <= c.y1; y++)
        gfx_fill(c.x0, y, c.x1 - c.x0 + 1, 1, gfx_vgrad_color(WP_TOP, WP_BOT, y, h));
    for (int x = GRID_STEP; x < gfx_width(); x += GRID_STEP) {
        if (x < c.x0 || x > c.x1) continue;
        gfx_vline(x, c.y0, c.y1 - c.y0 + 1, IC_GRID);
    }
    for (int y = GRID_STEP; y < h; y += GRID_STEP) {
        if (y < c.y0 || y > c.y1) continue;
        gfx_hline(c.x0, y, c.x1 - c.x0 + 1, IC_GRID);
    }
}
/* Cell-level clipped text: draws only the glyphs the damaged rect can show. */
static void text_in(rect64_t c, int x, int y, const char *s, u32 fg, int bg) {
    for (; s && *s; s++, x += 8) {
        if (x + 8 <= c.x0 || x > c.x1) continue;
        if (y + 16 <= c.y0 || y > c.y1) continue;
        gfx_cell(x, y, (unsigned char)*s, fg, bg);
    }
}
static void paint_topbar(rect64_t c) {
    int w = gfx_width();
    int y0 = c.y0 < 0 ? 0 : c.y0;
    int y1 = c.y1 > TOPBAR_H - 1 ? TOPBAR_H - 1 : c.y1;
    if (y1 < y0) return;
    gfx_fill(c.x0, y0, c.x1 - c.x0 + 1, y1 - y0 + 1, IC_BG_PANEL);
    if (y0 <= TOPBAR_H - 1 && y1 >= TOPBAR_H - 1)
        gfx_hline(c.x0, TOPBAR_H - 1, c.x1 - c.x0 + 1, IC_LINE);
    text_in(c, 12, 4, "MECTOV OS 64", IC_AMBER, -1);
    text_in(c, 12 + 13 * 8, 4, "M14 WM", IC_DIM, -1);
    char buf[64];
    char *p = put_u64((u64)w, buf);
    p = put_str(p, "x");
    p = put_u64((u64)gfx_height(), p);
    p = put_str(p, " framebuffer");
    if (focused >= 0) {
        int idx = tb_slot_index(focused);
        if (idx >= 0) {
            p = put_str(p, "  focus: ");
            const char *t = wins[idx].title;
            for (int i = 0; t[i] && i < 20; i++) *p++ = t[i];
        }
    }
    *p = 0;
    int len = (int)(p - buf);
    int x = w - 12 - len * 8;
    if (x < 12 + 21 * 8) x = 12 + 21 * 8; /* do not overlap the left text */
    text_in(c, x, 4, buf, IC_DIM, -1);
}
static void paint_taskbar(rect64_t c) {
    int w = gfx_width(), h = gfx_height();
    int y = h - TASKBAR_H;
    if (c.y1 < y) return;
    int y0 = c.y0 > y ? c.y0 : y;
    int y1 = c.y1;
    gfx_fill(c.x0, y0, c.x1 - c.x0 + 1, y1 - y0 + 1, IC_BG_PANEL);
    if (y0 <= y && y1 >= y) gfx_hline(c.x0, y, c.x1 - c.x0 + 1, IC_LINE);

    /* START block: same geometry as M9 (the pixel gates sample at 36,y). */
    gfx_fill(10, y + 7, 14, 14, IC_AMBER);
    text_in(c, 34, y + 6, "START", IC_AMBER_BRT, -1);
    gfx_vline(TB_BTN_X0 - 8, y + 6, 16, IC_LINE);

    /* One button per window. */
    for (int i = 0; i < WM64_MAX_WIN; i++) {
        if (!wins[i].used) continue;
        rect64_t b;
        b.x0 = TB_BTN_X0 + i * (TB_BTN_W + TB_BTN_GAP);
        b.y0 = y + 5;
        b.x1 = b.x0 + TB_BTN_W - 1;
        b.y1 = y + TASKBAR_H - 6;
        if (b.x0 > w - 1) break;
        int active = (focused == wins[i].id) && !wins[i].minimized;
        gfx_fill(b.x0, b.y0, b.x1 - b.x0 + 1, b.y1 - b.y0 + 1, IC_TITLE);
        gfx_frame(b.x0, b.y0, b.x1 - b.x0 + 1, b.y1 - b.y0 + 1,
                  active ? IC_FOCUS_LINE : IC_LINE);
        if (active) gfx_fill(b.x0 + 1, b.y1 - 2, b.x1 - b.x0 - 1, 2, IC_AMBER);
        char label[20];
        int n = 0;
        for (const char *t = wins[i].title; *t && n < 15; t++) label[n++] = *t;
        label[n] = 0;
        text_in(c, b.x0 + 6, b.y0 + 1, label,
                wins[i].minimized ? IC_DIM : IC_INK, -1);
    }

    /* Right-hand status text (wm64_tick refreshes it once a second). */
    if (status_len > 0) {
        int x = w - 12 - status_len * 8;
        text_in(c, x, y + 6, status, IC_DIM, -1);
    }
}

/* ---- painting: one window ---- */
static void paint_window(const win64_t *w, rect64_t c) {
    if (w->minimized) return;
    rect64_t body, cl, tb;
    outer_of(w, &body);
    client_of(w, &cl);
    title_of(w, &tb);

    /* Drop shadow first: the body drawn below covers the overlapping part. */
    gfx_fill(body.x0 + SHADOW, body.y0 + SHADOW, w->w, w->h, IC_SHADOW);

    /* Frame: four strips, never a body fill — a body fill would erase the
     * client area that this composite pass is not repainting. */
    gfx_fill(body.x0, body.y0, w->w, BORDER, IC_LINE);
    gfx_fill(body.x0, body.y1 - BORDER + 1, w->w, BORDER, IC_LINE);
    gfx_fill(body.x0, body.y0, BORDER, w->h, IC_LINE);
    gfx_fill(body.x1 - BORDER + 1, body.y0, BORDER, w->h, IC_LINE);

    /* Title bar. */
    gfx_fill(tb.x0, tb.y0, tb.x1 - tb.x0 + 1, TITLE_H - BORDER, IC_TITLE);
    gfx_hline(tb.x0, tb.y0 + TITLE_H - BORDER, tb.x1 - tb.x0 + 1, IC_LINE);
    int is_focus = (focused == w->id);
    text_in(c, w->x + 8, w->y + 2, w->title,
            is_focus ? IC_INK : IC_DIM, -1);
    if (is_focus)
        gfx_fill(body.x0 + 1, tb.y0 + TITLE_H - BORDER + 0, 1, 1,
                 IC_FOCUS_LINE);

    /* Title-bar buttons, with hover feedback. */
    for (int which = WM64_BTN_CLOSE; which <= WM64_BTN_MIN; which++) {
        rect64_t b;
        btn_of(w, which, &b);
        if (w->hover == which)
            gfx_fill(b.x0, b.y0, b.x1 - b.x0 + 1, b.y1 - b.y0 + 1, IC_LINE);
        const char *g = which == WM64_BTN_CLOSE ? "x"
                      : which == WM64_BTN_MAX ? "[]" : "_";
        int gx = b.x0 + (BTN_W - (int)(which == WM64_BTN_MAX ? 2 : 1) * 8) / 2;
        text_in(c, gx, b.y0 + 1, g, which == WM64_BTN_CLOSE ? IC_INK : IC_DIM, -1);
    }

    /* Client: repainted from the window's own state, but only when the damaged
     * rect actually reaches into it. Everything is drawn inside `ic`, never
     * beyond it: a callback that painted its whole client would overwrite the
     * pixels of a window above it that this pass is not compositing. */
    rect64_t ic;
    ic.x0 = c.x0 > cl.x0 ? c.x0 : cl.x0;
    ic.y0 = c.y0 > cl.y0 ? c.y0 : cl.y0;
    ic.x1 = c.x1 < cl.x1 ? c.x1 : cl.x1;
    ic.y1 = c.y1 < cl.y1 ? c.y1 : cl.y1;
    if (ic.x0 > ic.x1 || ic.y0 > ic.y1) return; /* chrome only */
    if (w->console) {
        /* The terminal's client IS the text console: redraw the cells that this
         * rect covers from the shadow grid. That is why a console window needs
         * no canvas and no draw callback — the grid is the canvas, and this is
         * an exact, rect-sized repaint.
         * Only while the console really lives in this client, though: during
         * the bring-up it is still a full-screen view, and repainting from that
         * grid would place cells at the full-screen origin instead of the
         * client's (drawing over the window frame). Until cons_rehome lands,
         * the client is just panel fill. */
        int vx = 0, vy = 0, vw = 0, vh = 0;
        int live = cons_view_rect(&vx, &vy, &vw, &vh) && vx == cl.x0 &&
                   vy == cl.y0 && vw <= cl.x1 - cl.x0 + 1 &&
                   vh <= cl.y1 - cl.y0 + 1;
        if (!live) { /* console is not in this window (yet): plain client fill */
            gfx_fill(ic.x0, ic.y0, ic.x1 - ic.x0 + 1, ic.y1 - ic.y0 + 1,
                     IC_BG_PANEL);
            return;
        }
        /* The view ends on a whole cell, so a client a few pixels wider keeps a
         * sliver of padding: fill it, then repaint the grid part. */
        int ix1 = ic.x1, iy1 = ic.y1;
        if (ix1 > vx + vw - 1) {
            gfx_fill(vx + vw, ic.y0, ix1 - (vx + vw) + 1, ic.y1 - ic.y0 + 1,
                     IC_BG_PANEL);
            ix1 = vx + vw - 1;
        }
        if (iy1 > vy + vh - 1) {
            gfx_fill(ic.x0, vy + vh, ix1 - ic.x0 + 1, iy1 - (vy + vh) + 1,
                     IC_BG_PANEL);
            iy1 = vy + vh - 1;
        }
        if (ic.x0 <= ix1 && ic.y0 <= iy1)
            cons_repaint_rect(ic.x0, ic.y0, ix1, iy1);
        return;
    }
    gfx_fill(ic.x0, ic.y0, ic.x1 - ic.x0 + 1, ic.y1 - ic.y0 + 1, IC_BG_PANEL);
    if (w->draw)
        w->draw(w->id, ic.x0, ic.y0, ic.x1 - ic.x0 + 1, ic.y1 - ic.y0 + 1);
}

/* ---- painting: cursor (last, so it is always on top) ---- */
static void paint_cursor(rect64_t c) {
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++) {
            if (!(cursor_mask[j] & (u16)(0x8000 >> i))) continue;
            int sx = cur_x + i + CUR_SHADOW, sy = cur_y + j + CUR_SHADOW;
            if (sx < c.x0 || sx > c.x1 || sy < c.y0 || sy > c.y1) continue;
            u32 bg = (u32)gfx_read(sx, sy);
            u32 sh = ((((bg >> 16) & 0xFF) * 140 >> 8) << 16) |
                     ((((bg >> 8) & 0xFF) * 140 >> 8) << 8) |
                     (((bg & 0xFF) * 140 >> 8));
            gfx_px(sx, sy, sh);
        }
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++) {
            if (!(cursor_mask[j] & (u16)(0x8000 >> i))) continue;
            int px = cur_x + i, py = cur_y + j;
            if (px < c.x0 || px > c.x1 || py < c.y0 || py > c.y1) continue;
            int fill = cursor_inner[j] & (u16)(0x8000 >> i);
            gfx_px(px, py, fill ? 0x00FFFFFFu : 0x00111111u);
        }
}

static void composite(rect64_t c) {
    c = r_clip(c);
    if (r_empty(&c)) return;
    paint_wallpaper(c);
    if (c.y0 <= TOPBAR_H - 1) paint_topbar(c);
    paint_taskbar(c);
    for (int zi = 0; zi < zcount; zi++) {
        win64_t *w = &wins[zorder[zi]];
        if (!w->used || w->minimized) continue;
        rect64_t body;
        outer_of(w, &body);
        body.x1 += SHADOW; /* the shadow is part of the window's footprint */
        body.y1 += SHADOW;
        if (!r_overlap(&body, &c)) continue;
        paint_window(w, c);
    }
    if (gfx_backbuf_live()) paint_cursor(c);
}

/* ---- z-order / focus ----
 * zorder holds WIN TABLE SLOTS (not window ids): it indexes wins[] directly,
 * which is what every drawing/iteration path wants. ids are what callers see,
 * so each entry point converts once (tb_slot_index). */
static void z_remove_slot(int slot) {
    int k = 0;
    for (int i = 0; i < zcount; i++)
        if (zorder[i] != slot) zorder[k++] = zorder[i];
    zcount = k;
}
static void z_push_top_slot(int slot) {
    z_remove_slot(slot);
    if (zcount < WM64_MAX_WIN) zorder[zcount++] = slot;
}

static void refocus(void) {
    focused = -1;
    for (int zi = zcount - 1; zi >= 0; zi--) {
        win64_t *w = &wins[zorder[zi]];
        if (w->used && !w->minimized) { focused = w->id; return; }
    }
}

/* ---- window entry points ---- */
int wm64_init(void) {
    if (!gfx_ready()) {
        s_puts("[K64] wm: no framebuffer, desktop skipped\n");
        return 0;
    }
    gfx_backbuf_alloc(); /* absent buffer: chrome still draws, just no overlap */
    ndmg = 0;
    zcount = 0;
    focused = -1;
    status_len = 0;
    status_x0 = -1;
    for (int i = 0; i < WM64_MAX_WIN; i++) wins[i].used = 0;
    /* One full-screen composite: this is the moment the desktop replaces
     * whatever the boot console left on the panel. */
    rect64_t all = r_full();
    u64 f = spin64_lock_irqsave(&wm64_lock);
    composite(all);
    gfx_present(all.x0, all.y0, all.x1, all.y1);
    spin64_unlock_irqrestore(&wm64_lock, f);
    ready = 1;
    s_printf("[K64] wm: compositor up %s\n",
             gfx_backbuf_live() ? "back-buffered (overlap ready)"
                                : "direct (no back buffer: no overlap)");
    return 1;
}

int wm64_open(int x, int y, int w, int h, const char *title,
              void (*draw)(int id, int cx, int cy, int cw, int ch),
              int is_console) {
    if (!ready) return -1;
    int slot = -1;
    for (int i = 0; i < WM64_MAX_WIN; i++)
        if (!wins[i].used) { slot = i; break; }
    if (slot < 0) {
        s_puts("[K64] wm: no free window slot\n");
        return -1;
    }
    win64_t *win = &wins[slot];
    for (int i = 0; i < (int)sizeof(wins[0]); i++) ((char *)win)[i] = 0;
    win->used = 1;
    win->id = next_id++;
    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    int n = 0;
    for (; title && title[n] && n < (int)sizeof(win->title) - 1; n++)
        win->title[n] = title[n];
    win->title[n] = 0;
    win->draw = draw;
    win->console = is_console;
    clamp_window(win);
    z_push_top_slot(slot);
    focused = win->id;
    rect64_t r;
    outer_of(win, &r);
    r.x1 += SHADOW;
    r.y1 += SHADOW;
    /* Print first (the log goes through the console window), then present. */
    s_printf("[K64] wm: open id=%u \"%s\" %u,%u %ux%u slot=%u\n",
             (u64)win->id, win->title, (u64)win->x, (u64)win->y,
             (u64)win->w, (u64)win->h, (u64)slot);
    dmg_record(0, 0, gfx_width() - 1, TOPBAR_H - 1);
    dmg_record(0, gfx_height() - TASKBAR_H, gfx_width() - 1, gfx_height() - 1);
    dmg_record(r.x0, r.y0, r.x1, r.y1);
    wm64_flush();
    return win->id;
}

void wm64_close(int id) {
    int idx = tb_slot_index(id);
    if (idx < 0) return;
    win64_t *w = &wins[idx];
    rect64_t r;
    outer_of(w, &r);
    r.x1 += SHADOW;
    r.y1 += SHADOW;
    w->used = 0;
    z_remove_slot(idx);
    refocus();
    dmg_record(r.x0, r.y0, r.x1, r.y1);
    /* Both bars show this window's name (and the taskbar its button). */
    dmg_record(0, 0, gfx_width() - 1, TOPBAR_H - 1);
    dmg_record(0, gfx_height() - TASKBAR_H, gfx_width() - 1, gfx_height() - 1);
    s_printf("[K64] wm: close id=%u (windows left=%u)\n", (u64)id,
             (u64)wm64_count());
    wm64_flush();
}

int wm64_count(void) {
    int n = 0;
    for (int i = 0; i < WM64_MAX_WIN; i++)
        if (wins[i].used && !wins[i].minimized) n++;
    return n;
}
int wm64_console_id(void) {
    for (int i = 0; i < WM64_MAX_WIN; i++)
        if (wins[i].used && wins[i].console) return wins[i].id;
    return -1;
}
int wm64_client_rect(int id, int *x, int *y, int *w, int *h) {
    int idx = tb_slot_index(id);
    if (idx < 0) return 0;
    rect64_t r;
    client_of(&wins[idx], &r);
    if (x) *x = r.x0;
    if (y) *y = r.y0;
    if (w) *w = r.x1 - r.x0 + 1;
    if (h) *h = r.y1 - r.y0 + 1;
    return 1;
}
int wm64_win_rect(int id, int *x, int *y, int *w, int *h) {
    int idx = tb_slot_index(id);
    if (idx < 0) return 0;
    if (x) *x = wins[idx].x;
    if (y) *y = wins[idx].y;
    if (w) *w = wins[idx].w;
    if (h) *h = wins[idx].h;
    return 1;
}
int wm64_cursor_x(void) { return cur_x; }
int wm64_cursor_y(void) { return cur_y; }
/* Raise + focus, for the shell (a freshly opened window is focused; the shell
 * then hands focus back to the terminal it opened first). */
void wm64_focus(int id) {
    int idx = tb_slot_index(id);
    if (idx < 0) return;
    z_push_top_slot(idx);
    focused = id;
    dmg_record(0, 0, gfx_width() - 1, TOPBAR_H - 1);
    dmg_record(0, gfx_height() - TASKBAR_H, gfx_width() - 1, gfx_height() - 1);
    wm64_flush();
}
void wm64_set_live(int id, int live) {
    int idx = tb_slot_index(id);
    if (idx >= 0) wins[idx].live = live;
}

void wm64_marker(void) {
    /* s_printf has no %d, so "no focus" and "no console window" are reported
     * as 0 (ids start at 1: 0 is never a live id). */
    int con = wm64_console_id();
    s_printf("[K64] wm: windows=%u focus=%u console=%u zcount=%u\n",
             (u64)wm64_count(), (u64)(focused < 0 ? 0 : focused),
             (u64)(con < 0 ? 0 : con), (u64)zcount);
    for (int zi = 0; zi < zcount; zi++) {
        win64_t *w = &wins[zorder[zi]];
        rect64_t c;
        client_of(w, &c);
        s_printf("[K64] wm: win id=%u z=%u \"%s\" %u,%u %ux%u client=%u,%u,%u,%u%s\n",
                 (u64)w->id, (u64)zi, w->title, (u64)w->x, (u64)w->y,
                 (u64)w->w, (u64)w->h, (u64)c.x0, (u64)c.y0,
                 (u64)(c.x1 - c.x0 + 1), (u64)(c.y1 - c.y0 + 1),
                 w->minimized ? " minimized" : "");
    }
}

void wm64_set_status(const char *s) {
    int n = 0;
    for (; s && s[n] && n < STATUS_MAX - 1; n++) status[n] = s[n];
    status[n] = 0;
    status_len = n;
}

/* ---- mouse ---- */
static void move_window_to(int id, int nx, int ny) {
    int idx = tb_slot_index(id);
    if (idx < 0) return;
    win64_t *w = &wins[idx];
    if (nx == w->x && ny == w->y) return;
    w->x = nx;
    w->y = ny;
    clamp_window(w);
    if (w->console) {
        /* The console's view moves with its window: same size, so the shadow
         * grid is untouched and only the origin changes. */
        rect64_t c;
        client_of(w, &c);
        cons_move_view(c.x0, c.y0);
    }
}
/* Record the whole footprint (shadow included) of a window. */
static void dmg_footprint(const win64_t *w) {
    dmg_record(w->x, w->y, w->x + w->w - 1 + SHADOW, w->y + w->h - 1 + SHADOW);
}

void wm64_on_mouse_move(int dx, int dy) {
    (void)dx;
    (void)dy;
    if (!ready) return;
    int nx = mouse64_x(), ny = mouse64_y();
    if (drag_id >= 0) {
        int idx = tb_slot_index(drag_id);
        if (idx >= 0) {
            win64_t *w = &wins[idx];
            dmg_footprint(w);
            move_window_to(drag_id, nx - w->dx, ny - w->dy);
            dmg_footprint(w);
        }
    } else {
        /* Hover feedback on title-bar buttons (and the taskbar is static). */
        int hit = win_at(nx, ny);
        for (int i = 0; i < WM64_MAX_WIN; i++) {
            if (!wins[i].used) continue;
            int want = WM64_BTN_NONE;
            if (hit == wins[i].id) {
                for (int which = WM64_BTN_CLOSE; which <= WM64_BTN_MIN; which++) {
                    rect64_t b;
                    btn_of(&wins[i], which, &b);
                    if (r_hit(&b, nx, ny)) { want = which; break; }
                }
            }
            if (want != wins[i].hover) {
                rect64_t b;
                btn_of(&wins[i], want == WM64_BTN_NONE ? wins[i].hover : want, &b);
                wins[i].hover = want;
                dmg_record(b.x0, b.y0, b.x1, b.y1);
            }
        }
    }
    if (nx != cur_x || ny != cur_y) {
        int ox = cur_x, oy = cur_y; /* the cursor's own old footprint */
        int hit = win_at(ox, oy);
        (void)hit;
        cur_x = nx;
        cur_y = ny;
        rect64_t old = { ox, oy, ox + CUR_W + CUR_SHADOW, oy + CUR_H + CUR_SHADOW };
        rect64_t nw = { cur_x, cur_y, cur_x + CUR_W + CUR_SHADOW,
                        cur_y + CUR_H + CUR_SHADOW };
        dmg_record(old.x0, old.y0, old.x1, old.y1);
        dmg_record(nw.x0, nw.y0, nw.x1, nw.y1);
    }
    if (!cur_moved_reported) {
        cur_moved_reported = 1;
        s_printf("[K64] gui: cursor moved to %u,%u\n", (u64)cur_x, (u64)cur_y);
    }
    wm64_flush();
}

static void maximize_toggle(int id) {
    int idx = tb_slot_index(id);
    if (idx < 0) return;
    win64_t *w = &wins[idx];
    rect64_t band;
    desk_band(&band);
    dmg_footprint(w);
    if (!w->maximized) {
        w->sx = w->x;
        w->sy = w->y;
        w->sw = w->w;
        w->sh = w->h;
        w->x = band.x0;
        w->y = band.y0;
        w->w = band.x1 - band.x0 + 1;
        w->h = band.y1 - band.y0 + 1;
        w->maximized = 1;
    } else {
        w->x = w->sx;
        w->y = w->sy;
        w->w = w->sw;
        w->h = w->sh;
        w->maximized = 0;
    }
    clamp_window(w);
    if (w->console) {
        /* A resize is a different view size, so the console is re-homed (its
         * grid is re-wrapped and the log tail is carried over). Under the
         * console lock and without printing, for the reason spelled out in
         * gui64_init: cons_rehome updates c_ox/c_oy and c_cols/c_rows in
         * separate stores, and a kernel print on another CPU that lands
         * between them draws cells with the new origin and the old row count —
         * off the bottom of the screen, which in M14 is an unmapped heap page
         * (the back buffer). */
        rect64_t c;
        client_of(w, &c);
        u64 lk = console_lock();
        cons_rehome((u32)c.x0, (u32)c.y0, (u32)(c.x1 - c.x0 + 1),
                    (u32)(c.y1 - c.y0 + 1));
        console_unlock(lk);
    }
    dmg_footprint(w);
    s_printf("[K64] wm: maximize id=%u -> %u,%u %ux%u\n", (u64)id, (u64)w->x,
             (u64)w->y, (u64)w->w, (u64)w->h);
    wm64_flush();
}

static void minimize_window(int id) {
    int idx = tb_slot_index(id);
    if (idx < 0) return;
    win64_t *w = &wins[idx];
    if (w->minimized) return;
    dmg_footprint(w);
    w->minimized = 1;
    if (focused == id) refocus();
    s_printf("[K64] wm: minimize id=%u (windows left=%u)\n", (u64)id,
             (u64)wm64_count());
    dmg_record(0, 0, gfx_width() - 1, TOPBAR_H - 1);
    dmg_record(0, gfx_height() - TASKBAR_H, gfx_width() - 1, gfx_height() - 1);
    wm64_flush();
}

void wm64_on_mouse_button(int x, int y, int buttons, int changed) {
    if (!ready) return;
    if (!(buttons & 1) && !(changed & 1)) {
        return; /* middle/right changes: unused for now */
    }
    if (buttons & 1) {
        int id = win_at(x, y);
        if (id >= 0) {
            int idx = tb_slot_index(id);
            win64_t *w = &wins[idx];
            rect64_t tb;
            title_of(w, &tb);
            if (r_hit(&tb, x, y)) {
                for (int which = WM64_BTN_CLOSE; which <= WM64_BTN_MIN; which++) {
                    rect64_t b;
                    btn_of(w, which, &b);
                    if (!r_hit(&b, x, y)) continue;
                    if (which == WM64_BTN_CLOSE) {
                        wm64_close(id);
                    } else if (which == WM64_BTN_MAX) {
                        maximize_toggle(id);
                    } else {
                        minimize_window(id);
                    }
                    wm64_flush();
                    return;
                }
                z_push_top_slot(idx);
                if (focused != id) {
                    focused = id;
                    s_printf("[K64] wm: raise id=%u \"%s\"\n", (u64)id, w->title);
                }
                w->dragging = 1;
                w->dx = x - w->x;
                w->dy = y - w->y;
                drag_id = id;
                dmg_record(0, 0, gfx_width() - 1, TOPBAR_H - 1);
                wm64_flush();
                return;
            }
            z_push_top_slot(idx);
            if (focused != id) {
                focused = id;
                s_printf("[K64] wm: focus id=%u \"%s\"\n", (u64)id, w->title);
            }
            dmg_record(0, 0, gfx_width() - 1, TOPBAR_H - 1);
            dmg_record(0, gfx_height() - TASKBAR_H, gfx_width() - 1,
                       gfx_height() - 1);
            wm64_flush();
            return;
        }
        int slot = -1;
        if (tb_slot_at(x, y, &slot)) {
            win64_t *w = &wins[slot];
            int was_focus = (focused == w->id);
            if (w->minimized) {
                w->minimized = 0;
                z_push_top_slot(slot);
                focused = w->id;
                s_printf("[K64] wm: restore id=%u \"%s\"\n", (u64)w->id, w->title);
            } else if (was_focus) {
                minimize_window(w->id);
            } else {
                z_push_top_slot(slot);
                focused = w->id;
                s_printf("[K64] wm: taskbar focus id=%u \"%s\"\n", (u64)w->id,
                         w->title);
            }
            dmg_footprint(w);
            dmg_record(0, 0, gfx_width() - 1, TOPBAR_H - 1);
            dmg_record(0, gfx_height() - TASKBAR_H, gfx_width() - 1,
                       gfx_height() - 1);
            wm64_flush();
            return;
        }
        if (y < TOPBAR_H) {
            s_printf("[K64] wm: topbar click %u,%u\n", (u64)x, (u64)y);
            wm64_flush();
            return;
        }
        if (y >= gfx_height() - TASKBAR_H) {
            s_printf("[K64] wm: start clicked %u,%u\n", (u64)x, (u64)y);
            wm64_flush();
            return;
        }
        if (focused != -1) {
            focused = -1;
            s_printf("[K64] wm: desktop focus\n");
            dmg_record(0, 0, gfx_width() - 1, TOPBAR_H - 1);
            dmg_record(0, gfx_height() - TASKBAR_H, gfx_width() - 1,
                       gfx_height() - 1);
            wm64_flush();
        }
        return;
    }
    /* release */
    if (drag_id >= 0) {
        int idx = tb_slot_index(drag_id);
        if (idx >= 0) {
            win64_t *w = &wins[idx];
            w->dragging = 0;
            s_printf("[K64] wm: drag id=%u to %u,%u\n", (u64)drag_id,
                     (u64)w->x, (u64)w->y);
        }
        drag_id = -1;
        wm64_flush();
    }
}

void wm64_tick(void) {
    if (!ready) return;
    /* A rejected pixel store means some caller passed a coordinate outside the
     * panel (gfx64 counts them). Report it once: silent clipping would hide a
     * layout bug, and before the check existed it was a #PF inside the
     * compositor, which is a much worse way to find out. */
    static u64 oob_seen;
    u64 oob = gfx_oob_count();
    if (oob != oob_seen) {
        int ox = 0, oy = 0;
        oob_seen = oob;
        if (gfx_oob_last(&ox, &oy))
            s_printf("[K64] wm: gfx rejected %u pixel store(s), first at %u,%u\n",
                     oob, (u64)ox, (u64)oy);
        gfx_oob_clear();
    }
    char buf[STATUS_MAX];
    char *p = put_str(buf, "SMP ");
    p = put_u64((u64)smp_cpu_count(), p);
    p = put_str(p, " cpu   up ");
    p = put_u64(k64_ticks() / 100, p); /* PIT is 100 Hz */
    p = put_str(p, "s");
    *p = 0;
    int old_x = status_x0;
    status_len = (int)(p - buf);
    for (int i = 0; i <= status_len; i++) status[i] = buf[i];
    status_x0 = gfx_width() - 12 - status_len * 8;

    int x0 = status_x0;
    if (old_x >= 0 && old_x < x0) x0 = old_x;
    dmg_record(x0, gfx_height() - TASKBAR_H, gfx_width() - 1,
               gfx_height() - 1);
    /* Live windows repaint their client once a second: that is how counters
     * and clocks in a window stay current without a per-frame renderer. */
    for (int i = 0; i < WM64_MAX_WIN; i++) {
        if (!wins[i].used || !wins[i].live || wins[i].minimized) continue;
        rect64_t c;
        client_of(&wins[i], &c);
        dmg_record(c.x0, c.y0, c.x1, c.y1);
    }
    wm64_flush();
}
