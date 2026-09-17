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
static u32 c_fg = CONS_FG, c_bg = CONS_BG;
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
    c_cur_x = c_cur_y = -1; /* the desktop removed the underline's pixels too */
}

/* Draw cell (cx,cy) and tell the desktop (mouse cursor lives on top). */
static void draw_cell(int cx, int cy, unsigned char ch) {
    gfx_cell(c_ox + cx * CONS_CELL_W, c_oy + cy * CONS_CELL_H, ch, c_fg,
             (int)c_bg);
    notify(cx * CONS_CELL_W, cy * CONS_CELL_H, CONS_CELL_W, CONS_CELL_H);
}

static void cursor_erase(void) {
    if (c_cur_x < 0) return;
    draw_cell(c_cur_x, c_cur_y, (unsigned char)c_grid[c_cur_y][c_cur_x]);
    c_cur_x = c_cur_y = -1;
}

static void cursor_paint(void) {
    if (!c_ready) return;
    if (c_cur_x == c_cx && c_cur_y == c_cy) return;
    int px = c_cx * CONS_CELL_W;
    int py = c_cy * CONS_CELL_H + (CONS_CELL_H - 2);
    gfx_fill(c_ox + px, c_oy + py, CONS_CELL_W, 2, CONS_CURSOR);
    notify(px, py, CONS_CELL_W, 2);
    c_cur_x = c_cx;
    c_cur_y = c_cy;
}

/* Shift both the pixels and the shadow grid up by one text row. */
static void scroll(void) {
    int vw = c_cols * CONS_CELL_W, vh = c_rows * CONS_CELL_H;
    hide_cursor_for_bulk();
    gfx_shift_up(c_ox, c_oy, vw, vh, CONS_CELL_H);
    gfx_fill(c_ox, c_oy + vh - CONS_CELL_H, vw, CONS_CELL_H, c_bg);
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
    if (c_cx == c_cur_x && c_cy == c_cur_y) {
        c_cur_x = c_cur_y = -1; /* the cell draw below paints over the underline */
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
