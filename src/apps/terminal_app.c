#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/wm.h"
#include "../include/shell.h"
#include "../include/keyboard.h"
#include "../include/mem.h"
#include "../include/timer.h"

// Forward declaration
void ex_cmd_term(void);

// ---- Terminal App ----
// Each terminal has its own text buffer (80 cols x 28 rows)
#define TERM_COLS 74
#define TERM_ROWS 24

typedef struct {
    char  buf[TERM_ROWS][TERM_COLS];
    uint8_t col[TERM_ROWS][TERM_COLS];
    int cx, cy;
    int win_id;
} TermState;

static TermState term;
int use_term_buf = 0; // used by ex_cmd() internally
int term_open = 0;
int term_app_running = 0;
int term_app_task_id = -1;

#define APP_KBD_BUF_SIZE 256
static uint8_t app_kbd_buf[APP_KBD_BUF_SIZE];
static int app_kbd_head = 0;
static int app_kbd_tail = 0;

void term_app_push_key(uint8_t sc) {
    int next = (app_kbd_head + 1) % APP_KBD_BUF_SIZE;
    if (next != app_kbd_tail) {
        app_kbd_buf[app_kbd_head] = sc;
        app_kbd_head = next;
    }
}

uint8_t term_app_pop_key() {
    if (app_kbd_head == app_kbd_tail) return 0;
    uint8_t sc = app_kbd_buf[app_kbd_tail];
    app_kbd_tail = (app_kbd_tail + 1) % APP_KBD_BUF_SIZE;
    return sc;
}

// Window state
static unsigned char term_cur_col = 0x0A;

// Redirect print() calls to terminal buffer
void term_putchar(char c, unsigned char color) {
    if (c == '\n') { term.cx = 0; term.cy++; }
    else if (c == '\b') {
        if (term.cx > 0) { term.cx--; term.buf[term.cy][term.cx] = ' '; }
    } else {
        if (term.cx >= TERM_COLS) { term.cx = 0; term.cy++; }
        term.buf[term.cy][term.cx] = c;
        term.col[term.cy][term.cx] = color;
        term.cx++;
    }
    // Scroll
    if (term.cy >= TERM_ROWS) {
        for (int r = 0; r < TERM_ROWS - 1; r++) {
            for (int c2 = 0; c2 < TERM_COLS; c2++) {
                term.buf[r][c2] = term.buf[r+1][c2];
                term.col[r][c2] = term.col[r+1][c2];
            }
        }
        for (int c2 = 0; c2 < TERM_COLS; c2++) { term.buf[TERM_ROWS-1][c2] = ' '; }
        term.cy = TERM_ROWS - 1;
    }
}

void term_print(const char* s, unsigned char color) {
    while (*s) term_putchar(*s++, color);
}

static void term_draw(int id, int cx2, int cy2, int cw, int ch) {
    (void)id; (void)cw; (void)ch;
    // Modern dark terminal background (Catppuccin Crust)
    draw_rect(cx2, cy2, cw, ch, 0x0011111B);

    // Render text buffer
    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c2 = 0; c2 < TERM_COLS; c2++) {
            char ch2 = term.buf[r][c2];
            uint8_t vc = term.col[r][c2];
            uint32_t fg = (ch2 && vc) ? vga_to_rgb(vc) : 0x00A6E3A1;
            draw_char_px(cx2 + c2*8, cy2 + r*16, ch2 ? ch2 : ' ', fg, 0x0011111B);
        }
    }
    // Draw cursor blink
    if ((get_ticks() / 500) & 1)
        draw_rect(cx2 + term.cx*8, cy2 + term.cy*16 + 14, 8, 2, 0x0000FF88);
}

// Extended scancode state (0xE0 prefix)
static int ext_sc_pending = 0;

static void term_key(int id, char c, uint8_t sc) {
    (void)id;
    if (term_app_running) {
        term_app_push_key(sc);
        return;
    }
    
    // Handle extended scancode prefix (arrow keys etc)
    if (sc == 0xE0) {
        ext_sc_pending = 1;
        return;
    }
    if (ext_sc_pending) {
        ext_sc_pending = 0;
        // Arrow keys after 0xE0 prefix
        if (sc == 0x48) { // Up arrow
            if (shell_history_up()) {
                // Redraw the line
                for (int i = 0; i < b_idx + 16; i++) term_putchar('\b', 0x00);
                term_print("root@mectov", 0x0A);
                term_print(" ~$ ", 0x0F);
                term_print(cmd_b, 0x0F);
            }
            return;
        }
        if (sc == 0x50) { // Down arrow
            if (shell_history_down()) {
                for (int i = 0; i < b_idx + 16; i++) term_putchar('\b', 0x00);
                term_print("root@mectov", 0x0A);
                term_print(" ~$ ", 0x0F);
                term_print(cmd_b, 0x0F);
            }
            return;
        }
        // Left/Right ignored for now
        return;
    }
    
    if (c == '\n') {
        term_putchar('\n', 0x0F);
        cmd_b[b_idx] = '\0';
        // Execute shell command — but redirect print to term_print
        // We swap print pointer via our wrapper approach
        ex_cmd_term(); // defined below
    } else if (c == '\b' && b_idx > 0) {
        b_idx--;
        term_putchar('\b', 0x0F);
    } else if (c == '\t') {
        // Tab completion
        shell_apply_tab();
    } else if (c != 0 && b_idx < 255) {
        cmd_b[b_idx++] = c;
        term_putchar(c, term_cur_col);
    }
}

static void term_tick(int id) { (void)id; }

// Override print to go to terminal buffer
// We set a flag so shell.c's print() calls go here
// This is called by our patched print() in shell — see below

void open_terminal_app() {
    if (term_open && wm_is_open(term.win_id)) { wm_raise(term.win_id); return; }
    // Clear buffer
    for (int r = 0; r < TERM_ROWS; r++)
        for (int c2 = 0; c2 < TERM_COLS; c2++) { term.buf[r][c2] = 0; term.col[r][c2] = 0; }
    term.cx = 0; term.cy = 0;
    term_print("Mectov OS v18.0 Terminal [GUI]\n", 0x0B);
    term_print("Welcome Bos Alif! System ready.\n\n", 0x0D);
    term_print("root@mectov", 0x0A);
    term_print(" ~$ ", 0x0F);
    b_idx = 0;
    term.win_id = wm_open(60, 40, 630, 430, "Terminal",
                          term_draw, term_key, term_tick, 0);
    term_open = (term.win_id >= 0);
}

// Called when wm_close fires for terminal window
void on_terminal_close() { term_open = 0; }

void term_clear() {
    for (int r = 0; r < TERM_ROWS; r++)
        for (int c = 0; c < TERM_COLS; c++) {
            term.buf[r][c] = 0;
            term.col[r][c] = 0;
        }
    term.cx = 0; term.cy = 0;
}

// ex_cmd_term: run shell command, output to terminal buffer
void ex_cmd_term() {
    use_term_buf = 1;
    ex_cmd();
    use_term_buf = 0;
}

// p_char_gui: called by patched vga print when get_use_term_buf is true
void p_char_gui(char c, unsigned char col) {
    term_putchar(c, col);
}

int get_use_term_buf() { 
    return term_open && (use_term_buf || term_app_running); 
}
