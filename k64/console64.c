/* VGA-1 (M8) text console on the Multiboot2 framebuffer — M9: view-aware.
 *
 * Scope: the 64-bit bring-up was serial-only through M7. M8 put the screen
 * back (boot log, demo output and fatal dumps visible without a serial
 * capture); M9 moves the same console into a window of the GUI desktop. So
 * the console is now a TEXT VIEW over a rectangle of the framebuffer:
 *   cons_init()      — full-screen view (boot log, M8 behaviour)
 *   cons_init_at()   — explicit view rect
 *   cons_rehome()    — move the view (GUI startup), keeping the last lines
 *
 * Design:
 *  - All pixel work goes through k64/gfx64.c, which owns the pixel format and
 *    the clipping. This file only knows text cells, the shadow grid and the
 *    cursor, so the console and the desktop can never disagree about the
 *    packing (M8 had its own put_px; that duplication is gone).
 *  - A 1-byte-per-cell shadow grid is kept alongside the pixels: erasing the
 *    cursor is "repaint this cell from the grid" (the cell may hold whichever
 *    glyph was there before the underline), and scroll() shifts the grid in
 *    the same step as the pixels. Repainting from the grid costs one cell.
 *  - Every kernel byte already funnels through s_putc_locked()/s_rawc()
 *    (kernel64.c), which also calls cons_putc(): serial and screen stay in
 *    lockstep, and s_write() (SYS64_PRINT) gives Ring-3 output for free. All
 *    of that runs under serial_lock with IF=0, so cons_putc never blocks,
 *    sleeps or takes a lock of its own.
 *  - c_dirty tells the desktop which rectangle changed, because the mouse
 *    cursor is composited over these pixels: the GUI re-saves and repaints
 *    its sprite when text lands under it. With no GUI hooked the pointer is
 *    NULL and a cell draw is just a draw.
 *  - Cost: scrolling shifts the view with 8-byte copies (gfx_shift_up) and a
 *    glyph is 128 pixel writes, so a boot log is a rounding error next to the
 *    serial writes it mirrors.
 *
 * Mode selection is GRUB's: QEMU/q35 + grub-mkrescue gives 1024x768x32 with
 * pitch 4096 (a 128x48 grid full-screen). Larger views are clamped to
 * CONS_MAX_COLS/ROWS and the console then covers the top-left part of the
 * view only; unusable modes (no FB tag, odd bpp) make the init return 0 and
 * the kernel keeps running serial-only, as its log line says.
 */
#include "cpu64.h"

/* Caller-provided views that fit the shadow grid without allocation. */
#define CONS_MAX_COLS 256 /* <= 2048 px */
#define CONS_MAX_ROWS 64  /* <= 1024 px */

#define CONS_CELL_W 8
#define CONS_CELL_H 16

/* Instrument-console palette (src/gui/login.c) as defaults; the desktop
 * recolours the view with cons_set_colors() so the terminal window matches
 * the rest of the chrome. */
#define CONS_BG 0x00000000u
#define CONS_FG 0x00D4D4D4u
#define CONS_CURSOR 0x00E0A94Fu

static int c_ox, c_oy; /* view origin (absolute framebuffer pixels) */
static int c_cols, c_rows;
static int c_cx, c_cy;  /* cursor cell */
static int c_ready;
static int c_cur_x = -1, c_cur_y = -1; /* cell carrying the underline (-1 none) */
/* M14: whether the underline is part of the current cell's pixels. The
 * compositor repaints console cells on demand, so "is there an underline here"
 * has to be state and not just a side effect of the last draw: a repaint of
 * the cursor's cell must put the underline back (it is drawn AFTER the glyph
 * below, exactly like cursor_paint does), or a scrolling console would lose
 * its cursor entirely the first time the desktop repainted that cell. */
static int c_cur_on;
static u32 c_fg = CONS_FG, c_bg = CONS_BG;
/* M14: emit no console pixels, but KEEP filling the shadow grid.
 *
 * The desktop's back buffer is live from gfx_backbuf_alloc() (inside
 * wm64_init) until cons_rehome() moves the view into the terminal window's
 * client. In that window the console still believes it owns the whole screen:
 * c_ox/c_oy are still 0,0 and c_fg is still the M8 default grey. A print in
 * that window therefore paints grey text into the compositor's surface at the
 * old origin, and nothing clears or reports it as damage -- so it survives in
 * the back buffer and shows up on the panel whenever some later, unrelated
 * damage rect happens to cover those pixels. That was cons_test's "M8 grey
 * leftovers" gate failing on a couple of runs out of four, always on the last
 * text row, which is exactly where a print between the two points lands.
 *
 * Distinct from cons_freeze(), which stops the grid too: the grid is what
 * cons_rehome() carries the boot log's tail from, so freezing here would drop
 * those lines out of the terminal window. */
static int c_quiet;
static void (*c_dirty)(int x0, int y0, int x1, int y1);
static void (*c_hide)(void);
static char c_grid[CONS_MAX_ROWS][CONS_MAX_COLS];

static void notify(int px, int py, int pw, int ph) {
    if (!c_dirty) return;
    c_dirty(c_ox + px, c_oy + py, c_ox + px + pw - 1, c_oy + py + ph - 1);
}
static void notify_view(void) {
    notify(0, 0, c_cols * CONS_CELL_W, c_rows * CONS_CELL_H);
}

/* Bulk mutations (scroll, view wipe) move pixels the desktop's mouse cursor
 * may be sitting on. A post-hoc repaint is NOT enough there: the shift would
 * copy the arrow sprite upward and leave a ghost, so the cursor is taken off
 * the screen first (c_hide restores its saved backdrop) and the caller's
 * cursor_paint()/notify() puts it back afterwards. Cell-sized draws only need
 * the post-notify, because they never move existing pixels. */
static void hide_cursor_for_bulk(void) {
    if (c_hide) c_hide();
    c_cur_on = 0;
    c_cur_x = c_cur_y = -1; /* a bulk move wipes the underline's pixels too */
}

/* Draw cell (cx,cy) and tell the desktop (mouse cursor lives on top).
 * The bounds check is belt-and-braces for the one case the console lock cannot
 * cover: a print on this CPU with c_ox/c_oy already moved by a re-home whose
 * c_rows has not landed yet would compute a cell below the screen, and M14's
 * back buffer makes "below the screen" an unmapped page. Grid coordinates are
 * also clamped so the shadow write cannot escape either. */
static void draw_cell(int cx, int cy, unsigned char ch) {
    if (cx < 0 || cy < 0 || cx >= c_cols || cy >= c_rows) return;
    if (c_quiet) return; /* the grid store in put_cell() already happened */
    gfx_cell(c_ox + cx * CONS_CELL_W, c_oy + cy * CONS_CELL_H, ch, c_fg,
             (int)c_bg);
    notify(cx * CONS_CELL_W, cy * CONS_CELL_H, CONS_CELL_W, CONS_CELL_H);
}

static void cursor_erase(void) {
    if (!c_cur_on) return;
    int ox = c_cur_x, oy = c_cur_y;
    c_cur_on = 0;
    c_cur_x = c_cur_y = -1;
    draw_cell(ox, oy, (unsigned char)c_grid[oy][ox]);
}

static void cursor_paint(void) {
    if (!c_ready || c_quiet) return;
    if (c_cur_on && c_cur_x == c_cx && c_cur_y == c_cy) return;
    int px = c_cx * CONS_CELL_W;
    int py = c_cy * CONS_CELL_H + (CONS_CELL_H - 2);
    /* State BEFORE the notify: that notify goes straight into the desktop
     * compositor, which may repaint this very cell — and a repaint that does
     * not know the underline belongs here would erase what we are drawing. */
    c_cur_on = 1;
    c_cur_x = c_cx;
    c_cur_y = c_cy;
    gfx_fill(c_ox + px, c_oy + py, CONS_CELL_W, 2, CONS_CURSOR);
    notify(px, py, CONS_CELL_W, 2);
}

/* Shift both the pixels and the shadow grid up by one text row. */
static void scroll(void) {
    int vw = c_cols * CONS_CELL_W, vh = c_rows * CONS_CELL_H;
    hide_cursor_for_bulk();
    if (!c_quiet) {
        gfx_shift_up(c_ox, c_oy, vw, vh, CONS_CELL_H);
        gfx_fill(c_ox, c_oy + vh - CONS_CELL_H, vw, CONS_CELL_H, c_bg);
    }
    for (int y = 1; y < c_rows; y++)
        for (int x = 0; x < c_cols; x++) c_grid[y - 1][x] = c_grid[y][x];
    for (int x = 0; x < c_cols; x++) c_grid[c_rows - 1][x] = ' ';
    notify_view();
}

static void cursor_newline(void) {
    cursor_erase();
    c_cx = 0;
    if (++c_cy >= c_rows) {
        scroll();
        c_cy = c_rows - 1;
    }
    cursor_paint();
}

static void put_cell(unsigned char ch) {
    if (c_cx < 0 || c_cx >= c_cols || c_cy < 0 || c_cy >= c_rows) return;
    if (c_cur_on && c_cx == c_cur_x && c_cy == c_cur_y) {
        c_cur_on = 0; /* the cell draw below paints over the underline */
        c_cur_x = c_cur_y = -1;
    } else {
        cursor_erase();
    }
    c_grid[c_cy][c_cx] = (char)ch;
    draw_cell(c_cx, c_cy, ch);
    if (++c_cx >= c_cols) {
        c_cx = 0;
        if (++c_cy >= c_rows) {
            scroll();
            c_cy = c_rows - 1;
        }
    }
    cursor_paint();
}

void cons_putc(char c) {
    if (!c_ready) return;
    if (c == '\n') {
        cursor_newline();
    } else if (c == '\r') {
        cursor_erase();
        c_cx = 0;
        cursor_paint();
    } else if (c == '\b' || c == 0x7F) {
        cursor_erase();
        if (c_cx > 0) c_cx--;
        c_grid[c_cy][c_cx] = ' ';
        draw_cell(c_cx, c_cy, ' ');
        cursor_paint();
    } else if (c == '\t') {
        int next = (c_cx + 8) & ~7;
        while (c_cx < next && c_cx < c_cols) put_cell(' ');
    } else if ((unsigned char)c >= 0x20) {
        put_cell((unsigned char)c);
    } /* other control bytes: ignored (serial has them verbatim) */
}

void cons_set_colors(u32 fg, u32 bg) {
    c_fg = fg;
    c_bg = bg;
}
void cons_set_dirty_hook(void (*fn)(int x0, int y0, int x1, int y1)) {
    c_dirty = fn;
}
void cons_set_hide_hook(void (*fn)(void)) { c_hide = fn; }

/* M14: stop/start emitting console pixels. The grid keeps filling either way,
 * so the re-home that ends the quiet period still carries the boot log's tail
 * into the terminal window. See c_quiet for why the quiet window exists. */
void cons_set_quiet(int q) { c_quiet = q; }

/* Size the view from a rect, clamp to the shadow grid, wipe it and put the
 * cursor at home. `wipe_all` also clears the rest of the framebuffer: only
 * the full-screen (boot log) init wants that, to drop GRUB's leftovers. */
static int view_from(int ox, int oy, u32 w, u32 h, int wipe_all) {
    int cols = (int)(w / CONS_CELL_W);
    int rows = (int)(h / CONS_CELL_H);
    if (cols < 1 || rows < 1) return 0;
    if (cols > CONS_MAX_COLS) cols = CONS_MAX_COLS;
    if (rows > CONS_MAX_ROWS) rows = CONS_MAX_ROWS;
    c_ox = ox;
    c_oy = oy;
    c_cols = cols;
    c_rows = rows;
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) c_grid[y][x] = ' ';
    c_cx = c_cy = 0;
    c_cur_on = 0;
    c_cur_x = c_cur_y = -1;
    c_ready = 1;
    hide_cursor_for_bulk();
    if (wipe_all) gfx_fill(0, 0, gfx_width(), gfx_height(), c_bg);
    else gfx_fill(c_ox, c_oy, cols * CONS_CELL_W, rows * CONS_CELL_H, c_bg);
    notify_view();
    cursor_paint();
    return 1;
}

static int cons_boot(u64 addr, u32 pitch, u32 w, u32 h, u32 bpp, int ox, int oy,
                     int wipe_all) {
    /* gfx64 owns the pixel format; the console only needs it to be live. */
    if (!gfx_init(addr, pitch, w, h, bpp)) return 0;
    if (!view_from(ox, oy, w, h, wipe_all)) return 0;
    s_printf("[K64] cons: %ux%u cells (%ux%u fb, %ubpp, pitch=%u)\n",
             (u64)c_cols, (u64)c_rows, (u64)w, (u64)h, (u64)bpp, (u64)pitch);
    return 1;
}

/* M8 entry point: console on the whole screen (and wipe GRUB's pixels). */
int cons_init(u64 addr, u32 pitch, u32 w, u32 h, u32 bpp) {
    return cons_boot(addr, pitch, w, h, bpp, 0, 0, 1);
}

/* M9: console inside an explicit view rect (a desktop window's client area).
 * The pixel format must already be live (cons_init ran); gfx_init is
 * idempotent, so passing the framebuffer again is fine. */
int cons_init_at(u64 addr, u32 pitch, u32 ox, u32 oy, u32 w, u32 h, u32 bpp) {
    if (!gfx_init(addr, pitch, g_fb_w, g_fb_h, bpp)) return 0;
    return view_from((int)ox, (int)oy, w, h, 0);
}

/* M9: move a live console into a new rect, carrying the tail of the log so
 * the terminal window shows where the boot got to instead of an empty sheet.
 * The old grid is shifted in place (destination rows are always lower than
 * their source, so a forward copy cannot smear). */
int cons_rehome(u32 ox, u32 oy, u32 w, u32 h) {
    if (!c_ready) return 0;
    int ncols = (int)(w / CONS_CELL_W), nrows = (int)(h / CONS_CELL_H);
    if (ncols < 1 || nrows < 1) return 0;
    if (ncols > CONS_MAX_COLS) ncols = CONS_MAX_COLS;
    if (nrows > CONS_MAX_ROWS) nrows = CONS_MAX_ROWS;
    int oc = c_cols, orows = c_rows;
    int keep = orows < nrows ? orows : nrows;
    int top = nrows - keep; /* blank padding when the new view is taller */
    for (int y = 0; y < keep; y++)
        for (int x = 0; x < ncols; x++)
            c_grid[top + y][x] = (x < oc) ? c_grid[orows - keep + y][x] : ' ';
    c_ox = (int)ox;
    c_oy = (int)oy;
    c_cols = ncols;
    c_rows = nrows;
    /* Repaint: blank the view (the desktop cleared the client rect already,
     * but the old view may have been larger), then the carried lines. */
    hide_cursor_for_bulk();
    gfx_fill(c_ox, c_oy, ncols * CONS_CELL_W, nrows * CONS_CELL_H, c_bg);
    for (int y = 0; y < nrows; y++) {
        for (int x = 0; x < ncols; x++) {
            char ch = c_grid[y][x];
            if (ch != ' ') {
                gfx_cell(c_ox + x * CONS_CELL_W, c_oy + y * CONS_CELL_H,
                         (unsigned char)ch, c_fg, (int)c_bg);
            }
        }
    }
    /* Continue where the carried log stopped: find the last written row and
     * park the cursor just after its final character (blank rows are pulled
     * in from above). A full bottom line scrolls once so the next byte has
     * room, exactly as normal output would. */
    c_cur_x = c_cur_y = -1;
    c_cy = nrows - 1;
    c_cx = 0;
    for (int y = nrows - 1; y >= 0; y--) {
        int last = -1;
        for (int x = 0; x < ncols; x++)
            if (c_grid[y][x] != ' ') last = x;
        if (last >= 0) {
            c_cy = y;
            c_cx = last + 1;
            break;
        }
    }
    if (c_cx >= ncols) {
        c_cx = 0;
        scroll();
        c_cy = nrows - 1;
    }
    notify_view();
    cursor_paint();
    /* No log line here on purpose: the caller (gui64_init) holds the console
     * lock across the whole desktop draw, and printing under it would
     * self-deadlock. It reports the new geometry once it unlocks. */
    return 1;
}

/* Cell grid of the live view (for the caller's log line after a re-home). */
void cons_view_cells(int *cols, int *rows) {
    if (cols) *cols = c_cols;
    if (rows) *rows = c_rows;
}

/* M14: repaint the part of the view inside an absolute screen rect, from the
 * shadow grid. This is what makes a console WINDOW possible without a per-
 * window canvas: the grid is the canvas, so the compositor can ask for exactly
 * the pixels a damaged rect needs and get them back — spaces included, which
 * is what erases a line that has scrolled away or a window that moved off.
 * No damage is reported here: the caller (k64/wm64.c) is already resolving the
 * rect it asked about, and a notify from inside a composite would loop. */
void cons_repaint_rect(int x0, int y0, int x1, int y1) {
    if (!c_ready) return;
    int vx1 = c_ox + c_cols * CONS_CELL_W - 1;
    int vy1 = c_oy + c_rows * CONS_CELL_H - 1;
    if (x1 < c_ox || y1 < c_oy || x0 > vx1 || y0 > vy1) return;
    int cx0 = (x0 - c_ox) / CONS_CELL_W;
    int cy0 = (y0 - c_oy) / CONS_CELL_H;
    int cx1 = (x1 - c_ox) / CONS_CELL_W;
    int cy1 = (y1 - c_oy) / CONS_CELL_H;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 > c_cols - 1) cx1 = c_cols - 1;
    if (cy1 > c_rows - 1) cy1 = c_rows - 1;
    for (int y = cy0; y <= cy1; y++)
        for (int x = cx0; x <= cx1; x++)
            gfx_cell(c_ox + x * CONS_CELL_W, c_oy + y * CONS_CELL_H,
                     (unsigned char)c_grid[y][x], c_fg, (int)c_bg);
    if (c_cur_on) { /* the underline is not part of the grid, so put it back */
        int ux = c_ox + c_cx * CONS_CELL_W;
        int uy = c_oy + c_cy * CONS_CELL_H + (CONS_CELL_H - 2);
        if (!(ux > x1 || ux + CONS_CELL_W - 1 < x0 || uy > y1 || uy + 1 < y0))
            gfx_fill(ux, uy, CONS_CELL_W, 2, CONS_CURSOR);
    }
}

/* Whole-view repaint: cons_rehome() after a resize, and any caller that wants
 * the view re-established unconditionally. */
void cons_repaint(void) {
    if (!c_ready) return;
    cons_repaint_rect(c_ox, c_oy, c_ox + c_cols * CONS_CELL_W - 1,
                      c_oy + c_rows * CONS_CELL_H - 1);
    c_cur_x = c_cur_y = -1; /* force the underline back in */
    cursor_paint();
    notify_view();
}

/* M14: stop drawing altogether, permanently. Called first by the fatal fault
 * paths: they run with interrupts off, possibly on a CR3 whose mappings are
 * the reason we are dying, and every console cell they draw would now go
 * through the desktop compositor — one more fault per printed character, which
 * buries the dump instead of showing it. Frozen, s_rawc/s_raws still reach the
 * serial log, which is where a post-mortem is read from. */
void cons_freeze(void) { c_ready = 0; }

/* M14: the live view's rect in screen pixels. The window manager uses it to
 * tell whether the console actually lives in the client rect it is about to
 * repaint: during bring-up the console is still full-screen while its window
 * already exists, and repainting that client from a full-screen grid draws
 * cells at the wrong origin (the columns that straddle the client's edge land
 * one pixel over the window frame). */
int cons_view_rect(int *x, int *y, int *w, int *h) {
    if (!c_ready) return 0;
    if (x) *x = c_ox;
    if (y) *y = c_oy;
    if (w) *w = c_cols * CONS_CELL_W;
    if (h) *h = c_rows * CONS_CELL_H;
    return 1;
}

/* M14: move a live view without changing its size — a window being dragged.
 * The grid is untouched (cons_rehome is for resizes, where it has to re-wrap),
 * so this is origin + repaint. The old pixels are the window manager's
 * business: it damaged the old footprint before calling. */
void cons_move_view(int ox, int oy) {
    if (!c_ready) return;
    if (c_ox == ox && c_oy == oy) return;
    c_ox = ox;
    c_oy = oy;
    cons_repaint();
}
