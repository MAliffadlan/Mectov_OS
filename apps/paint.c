// Pixel Paint (v38.75) — cell-based paint app for the Mectov desktop.
//
// Design constraint that shaped this app: Ring 3 windows render through a
// replayed display list capped at MAX_DRAW_CMDS (512), and the WM clears the
// client buffer on every re-render — so a freehand canvas that records one
// rect per brush point overflows the list within seconds of drawing.
//
// Solution: the canvas is a COARSE CELL GRID (24px cells). Every stroke
// stamps whole cells; a cell is at most ONE display-list record regardless
// of how many times the user paints over it, and unpainted cells are never
// recorded. Worst case (every cell painted) is ~336 records + ~60 toolbar
// records — always under the cap, with zero kernel changes.
//
// Controls:
//   Left mouse drag   paint with the current color / brush
//   Click a swatch    pick color (1..8 also works)
//   Click B / `b`     cycle brush size (1x1, 2x2, 3x3 cells)
//   Click Clear / `c` blank the canvas
//   Click Quit / ESC  close the window

#include "src/include/syscall.h"

typedef struct {
    int type;
    int x, y;
    int key;
} gui_event_t;

#define PW 600            // window width  (client = PW-2 after WM frame)
#define PH 420            // window height (client = PH-22 after titlebar)

#define CELL   24         // cell size in pixels
#define GRID_X 11         // canvas top-left in client coords (11px margins both sides)
#define GRID_Y 34
#define GRID_COLS 24
#define GRID_ROWS 14
#define CANVAS_W (GRID_COLS * CELL)   // 576
#define CANVAS_H (GRID_ROWS * CELL)   // 336

#define TOOL_Y 6          // toolbar row
#define STATUS_Y (GRID_Y + CANVAS_H + 6)

static uint8_t cells[GRID_COLS * GRID_ROWS];   // palette index per cell, 0 = paper
static uint8_t inited   = 0;
static int     win_cw   = PW - 2;
static int     win_ch   = PH - 22;

static int cur_color    = 2;   // start with black
static int brush        = 0;   // 0 = 1x1, 1 = 2x2, 2 = 3x3 cells

// ---- File I/O (v38.76): save/open the cell grid to the VFS ----
// Format: magic "MCP1" + GRID_COLS*GRID_ROWS bytes of palette indices.
#define SAVE_PATH "home/paint.mcp"
#define IO_BUF_SIZE (4 + GRID_COLS * GRID_ROWS)   // 340 bytes
static char status_msg[32];   // transient feedback, drawn right of the hint
static int  status_len = 0;

static void set_status(const char* s) {
    int n = 0;
    while (*s && n < (int)sizeof(status_msg) - 1) status_msg[n++] = *s++;
    status_msg[n] = '\0';
    status_len = n;
}

static void do_save(void) {
    char buf[IO_BUF_SIZE];
    buf[0] = 'M'; buf[1] = 'C'; buf[2] = 'P'; buf[3] = '1';
    for (int i = 0; i < GRID_COLS * GRID_ROWS; i++)
        buf[4 + i] = (char)cells[i];
    int fd = sys_open(SAVE_PATH);
    if (fd < 0) {
        sys_create_file(SAVE_PATH);
        fd = sys_open(SAVE_PATH);
    }
    if (fd < 0) { set_status("Save FAILED"); return; }
    sys_write(fd, buf, IO_BUF_SIZE);
    sys_close(fd);
    set_status("Saved " SAVE_PATH);
}

static void do_open(void) {
    char buf[IO_BUF_SIZE];
    int fd = sys_open(SAVE_PATH);
    if (fd < 0) { set_status("No file yet"); return; }
    int n = sys_read(fd, buf, IO_BUF_SIZE);
    sys_close(fd);
    if (n < IO_BUF_SIZE || buf[0] != 'M' || buf[1] != 'C' ||
        buf[2] != 'P' || buf[3] != '1') {
        set_status("Bad file");
        return;
    }
    for (int i = 0; i < GRID_COLS * GRID_ROWS; i++) {
        uint8_t v = (uint8_t)buf[4 + i];
        cells[i] = (v < 8) ? v : 0;
    }
    set_status("Opened");
}

static const uint32_t palette[8] = {
    0xFFF5F0E7,   // 0 paper (white-ish) — doubles as the eraser
    0xFF1E1E2E,   // 1 black
    0xFF2E3440,   // 2 ink
    0xFFE06C75,   // 3 red
    0xFFE5A059,   // 4 orange
    0xFFE5C07B,   // 5 yellow
    0xFFA6E3A1,   // 6 green
    0xFF7AA2F7,   // 7 blue
};
static const char* const color_names[8] = {
    "Paper", "Black", "Ink", "Red", "Orange", "Yellow", "Green", "Blue"
};

static void grid_init(void) {
    for (int i = 0; i < GRID_COLS * GRID_ROWS; i++) cells[i] = 0;
    inited = 1;
}

static int in_grid(int gx, int gy) {
    return gx >= 0 && gx < GRID_COLS && gy >= 0 && gy < GRID_ROWS;
}

// Stamp the brush footprint centered on (gx, gy).
static void stamp(int gx, int gy) {
    int r = brush;   // radius in cells
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int x = gx + dx, y = gy + dy;
            if (in_grid(x, y)) cells[y * GRID_COLS + x] = (uint8_t)cur_color;
        }
}

static void draw_ui(int wid) {
    // Toolbar
    for (int i = 0; i < 8; i++) {
        int x = 6 + i * 26;
        sys_draw_rect(wid, x, TOOL_Y, 22, 22, palette[i]);
        if (i == cur_color) {
            // selection outline: 4 thin rects
            sys_draw_rect(wid, x - 1, TOOL_Y - 1, 24, 1,  0xFF89B4FA);
            sys_draw_rect(wid, x - 1, TOOL_Y + 22, 24, 1, 0xFF89B4FA);
            sys_draw_rect(wid, x - 1, TOOL_Y, 1, 22, 0xFF89B4FA);
            sys_draw_rect(wid, x + 22, TOOL_Y, 1, 22, 0xFF89B4FA);
        }
    }
    // Brush size buttons (B1/B2/B3)
    for (int i = 0; i < 3; i++) {
        int x = 6 + 8 * 26 + i * 30;
        uint32_t bg = (i == brush) ? 0xFF89B4FA : 0xFF313244;
        sys_draw_rect(wid, x, TOOL_Y, 26, 22, bg);
        char lbl[4]; lbl[0] = 'B'; lbl[1] = (char)('1' + i); lbl[2] = '\0';
        sys_draw_text(wid, x + 7, TOOL_Y + 6, lbl, 0xFFCDD6F4);
    }
    // Save + Open (file I/O)
    sys_draw_rect(wid, 312, TOOL_Y, 50, 22, 0xFF313244);
    sys_draw_text(wid, 326, TOOL_Y + 6, "Save", 0xFFA6E3A1);
    sys_draw_rect(wid, 370, TOOL_Y, 50, 22, 0xFF313244);
    sys_draw_text(wid, 384, TOOL_Y + 6, "Open", 0xFFF9E2AF);
    // Clear + Quit
    sys_draw_rect(wid, win_cw - 128, TOOL_Y, 58, 22, 0xFF313244);
    sys_draw_text(wid, win_cw - 122, TOOL_Y + 6, "Clear", 0xFFF9E2AF);
    sys_draw_rect(wid, win_cw - 64, TOOL_Y, 58, 22, 0xFF313244);
    sys_draw_text(wid, win_cw - 54, TOOL_Y + 6, "Quit", 0xFFF38BA8);

    // Canvas: paper + grid lines
    sys_draw_rect(wid, GRID_X, GRID_Y, CANVAS_W, CANVAS_H, palette[0]);
    for (int c = 1; c < GRID_COLS; c++)
        sys_draw_rect(wid, GRID_X + c * CELL - 1, GRID_Y, 1, CANVAS_H, 0xFFE8E2D8);
    for (int r = 1; r < GRID_ROWS; r++)
        sys_draw_rect(wid, GRID_X, GRID_Y + r * CELL - 1, CANVAS_W, 1, 0xFFE8E2D8);
    // painted cells on top
    for (int gy = 0; gy < GRID_ROWS; gy++)
        for (int gx = 0; gx < GRID_COLS; gx++) {
            uint8_t v = cells[gy * GRID_COLS + gx];
            if (v)
                sys_draw_rect(wid, GRID_X + gx * CELL, GRID_Y + gy * CELL,
                              CELL - 2, CELL - 2, palette[v]);
        }
    sys_draw_rect(wid, GRID_X - 1, GRID_Y - 1, CANVAS_W + 2, 1, 0xFF6C7086);
    sys_draw_rect(wid, GRID_X - 1, GRID_Y + CANVAS_H, CANVAS_W + 2, 1, 0xFF6C7086);
    sys_draw_rect(wid, GRID_X - 1, GRID_Y, 1, CANVAS_H, 0xFF6C7086);
    sys_draw_rect(wid, GRID_X + CANVAS_W, GRID_Y, 1, CANVAS_H, 0xFF6C7086);

    // Status bar
    sys_draw_rect(wid, 0, STATUS_Y, win_cw, win_ch - STATUS_Y, 0xFF181825);
    static char st[64];
    // fixed small messages to stay in the display-list budget
    const char* brushname = (brush == 0) ? "B1" : (brush == 1) ? "B2" : "B3";
    const char* colname = color_names[cur_color];
    int n = 0;
    const char* s1 = "Color: ";
    while (*s1) st[n++] = *s1++;
    while (*colname) st[n++] = *colname++;
    s1 = "   Brush: ";
    while (*s1) st[n++] = *s1++;
    st[n++] = brushname[0]; st[n++] = brushname[1];
    s1 = "   Drag to paint";
    while (*s1) st[n++] = *s1++;
    st[n] = '\0';
    sys_draw_text(wid, 8, STATUS_Y + 5, st, 0xFFA6E3A1);
    if (status_len > 0) sys_draw_text(wid, 300, STATUS_Y + 5, status_msg, 0xFFF9E2AF);
}

static void redraw(int wid) {
    draw_ui(wid);
    sys_update_window(wid);
}

// Entry point convention: the loader jumps to the MCT header entry offset,
// which build_mct.py derives from the `_start` symbol — apps define _start
// directly (see browser.c); a plain main() leaves _start undefined and the
// app crashes on launch (entry falls back to offset 0).
void _start(void) {
    if (!inited) grid_init();
    int wid = sys_create_window(60, 40, PW, PH, "Pixel Paint");
    if (wid < 0) sys_exit();

    int painting = 0;
    int last_gx = -1, last_gy = -1;
    int need_redraw = 1;

    while (1) {
        if (need_redraw) { redraw(wid); need_redraw = 0; }

        gui_event_t ev;
        int have = 0;
        while (sys_get_event(wid, &ev)) {
            have = 1;
            if (ev.type == 1) {                    // Paint
                need_redraw = 1;
            } else if (ev.type == 2) {             // Key
                if (ev.key == 27 || ev.key == 'q') sys_exit();
                else if (ev.key == 'c') { grid_init(); set_status("Cleared"); need_redraw = 1; }
                else if (ev.key == 's') { do_save(); need_redraw = 1; }
                else if (ev.key == 'o') { do_open(); need_redraw = 1; }
                else if (ev.key == 'b') { brush = (brush + 1) % 3; need_redraw = 1; }
                else if (ev.key >= '1' && ev.key <= '8') {
                    cur_color = ev.key - '1'; need_redraw = 1;
                }
            } else if (ev.type == 3) {             // Mouse
                int gx = (ev.x - GRID_X) / CELL;
                int gy = (ev.y - GRID_Y) / CELL;
                if (ev.y < 30) {                   // toolbar click
                    if (ev.key == 1) {
                        int sw = (ev.x - 6) / 26;
                        if (sw >= 0 && sw < 8) { cur_color = sw; need_redraw = 1; }
                        else if (ev.x >= 6 + 8 * 26 && ev.x < 6 + 8 * 26 + 3 * 30) {
                            brush = (ev.x - (6 + 8 * 26)) / 30;
                            if (brush < 0) brush = 0; if (brush > 2) brush = 2;
                            need_redraw = 1;
                        }
                        else if (ev.x >= 312 && ev.x < 362) { do_save(); need_redraw = 1; }
                        else if (ev.x >= 370 && ev.x < 420) { do_open(); need_redraw = 1; }
                        else if (ev.x >= win_cw - 128 && ev.x < win_cw - 70) { grid_init(); set_status("Cleared"); need_redraw = 1; }
                        else if (ev.x >= win_cw - 64) sys_exit();
                    }
                } else if (ev.x >= GRID_X && ev.x < GRID_X + CANVAS_W &&
                           ev.y >= GRID_Y && ev.y < GRID_Y + CANVAS_H) {
                    if (ev.key == 1) {             // press or drag with button held
                        if (in_grid(gx, gy) && (gx != last_gx || gy != last_gy)) {
                            stamp(gx, gy);
                            last_gx = gx; last_gy = gy;
                            painting = 1;
                            need_redraw = 1;
                        }
                    } else {
                        painting = 0; last_gx = -1; last_gy = -1;
                    }
                } else {
                    painting = 0; last_gx = -1; last_gy = -1;
                }
            } else if (ev.type == 5) {             // client size
                if (ev.x > 40 && ev.y > 40) {
                    win_cw = ev.x; win_ch = ev.y;
                    need_redraw = 1;
                }
            }
        }
        (void)painting; (void)have;
        sys_yield();
    }
}
