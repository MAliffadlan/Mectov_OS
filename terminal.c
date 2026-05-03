#include "src/include/syscall.h"

typedef struct {
    int type;
    int x, y;
    int key;
} gui_event_t;

#define TERM_COLS 74
#define TERM_ROWS 24

static char  buf[TERM_ROWS][TERM_COLS];
static uint8_t col[TERM_ROWS][TERM_COLS];
static int cx = 0, cy = 0;

static char cmd[256];
static int cmd_len = 0;

static int ipc_qid = 0;

static void term_scroll() {
    for (int r = 0; r < TERM_ROWS - 1; r++)
        for (int c = 0; c < TERM_COLS; c++) {
            buf[r][c] = buf[r+1][c];
            col[r][c] = col[r+1][c];
        }
    for (int c = 0; c < TERM_COLS; c++) {
        buf[TERM_ROWS-1][c] = ' ';
        col[TERM_ROWS-1][c] = 0;
    }
    cy = TERM_ROWS - 1;
}

static void term_putchar(char c2, uint8_t color) {
    if (c2 == '\n') { cx = 0; cy++; }
    else if (c2 == '\r') { cx = 0; }
    else if (c2 == '\b') {
        if (cx > 0) { cx--; buf[cy][cx] = ' '; col[cy][cx] = 0; }
    } else {
        if (cx >= TERM_COLS) { cx = 0; cy++; }
        buf[cy][cx] = c2;
        col[cy][cx] = color;
        cx++;
    }
    if (cy >= TERM_ROWS) term_scroll();
}

static void term_print(const char* s, uint8_t color) {
    while (*s) term_putchar(*s++, color);
}

// VGA color to RGB (simplified Catppuccin palette)
static uint32_t vga_to_rgb(uint8_t c) {
    switch(c) {
        case 0x00: return 0x0011111B;
        case 0x01: return 0x0089B4FA; // Blue
        case 0x02: return 0x00A6E3A1; // Green
        case 0x03: return 0x0094E2D5; // Cyan
        case 0x04: return 0x00F38BA8; // Red
        case 0x05: return 0x00CBA6F7; // Purple
        case 0x06: return 0x00F9E2AF; // Yellow
        case 0x07: return 0x00BAC2DE; // Light gray
        case 0x08: return 0x00585B70; // Dark gray
        case 0x09: return 0x0089B4FA; // Light blue
        case 0x0A: return 0x00A6E3A1; // Light green
        case 0x0B: return 0x0094E2D5; // Light cyan
        case 0x0C: return 0x00F38BA8; // Light red
        case 0x0D: return 0x00F5C2E7; // Pink
        case 0x0E: return 0x00F9E2AF; // Light yellow
        case 0x0F: return 0x00CDD6F4; // White
        default:   return 0x00CDD6F4;
    }
}

static void draw_terminal(int wid) {
    int cw = 600, ch = 400;
    // Dark background
    sys_draw_rect(wid, 0, 0, cw, ch, 0x0011111B);
    
    // Render text buffer per line chunked by color
    for (int r = 0; r < TERM_ROWS; r++) {
        char line_buf[TERM_COLS + 1];
        int len = 0;
        int start_c = -1;
        uint8_t current_col = 0;

        for (int c = 0; c < TERM_COLS; c++) {
            char ch2 = buf[r][c];
            uint8_t vc = col[r][c];
            if (ch2 && vc) {
                if (start_c == -1) {
                    start_c = c;
                    current_col = vc;
                    line_buf[len++] = ch2;
                } else if (vc == current_col) {
                    int gap = c - (start_c + len);
                    for (int g = 0; g < gap; g++) line_buf[len++] = ' ';
                    line_buf[len++] = ch2;
                } else {
                    line_buf[len] = '\0';
                    sys_draw_text(wid, start_c * 8, r * 16, line_buf, vga_to_rgb(current_col));
                    start_c = c;
                    current_col = vc;
                    len = 0;
                    line_buf[len++] = ch2;
                }
            }
        }
        if (start_c != -1) {
            line_buf[len] = '\0';
            sys_draw_text(wid, start_c * 8, r * 16, line_buf, vga_to_rgb(current_col));
        }
    }
    
    // Cursor blink (simple - always show)
    sys_draw_rect(wid, cx*8, cy*16 + 14, 8, 2, 0x0000FF88);
    
    sys_update_window(wid);
}

static void drain_ipc() {
    // Read all IPC messages (shell output chars)
    char msg[128];
    int count = 0;
    while (count < 4096) { // drain as much as possible
        int ret = sys_ipc_try_recv(ipc_qid, msg, 128);
        if (ret <= 0) break;
        // msg contains [char1, col1, char2, col2, ...]
        for (int i = 0; i < ret; i += 2) {
            term_putchar(msg[i], (uint8_t)msg[i+1]);
        }
        count++;
    }
}

static void print_prompt() {
    term_print("root@mectov", 0x0A);
    term_print(" ~$ ", 0x0F);
}

void _start() {
    int wid = sys_create_window(60, 40, 600, 400, "Terminal (Ring 3)");
    if (wid < 0) sys_exit();
    
    // Create IPC queue for stdout
    ipc_qid = sys_ipc_create(0xDEAD);
    if (ipc_qid > 0) {
        sys_set_stdout_ipc(ipc_qid);
    }
    
    // Clear buffer
    for (int r = 0; r < TERM_ROWS; r++)
        for (int c = 0; c < TERM_COLS; c++) {
            buf[r][c] = 0; col[r][c] = 0;
        }
    
    term_print("Mectov OS v18.0 Terminal [Ring 3]\n", 0x0B);
    term_print("Welcome Bos Alif! System ready.\n\n", 0x0D);
    print_prompt();
    cmd_len = 0;
    
    draw_terminal(wid);
    
    gui_event_t ev;
    int frame = 0;
    
    while (1) {
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 1) { // Paint
                draw_terminal(wid);
            } else if (ev.type == 2) { // Key
                if (ev.key == 27) {
                    sys_set_stdout_ipc(0); // Disable redirect
                    sys_exit();
                }
                
                if (ev.key == '\n') {
                    term_putchar('\n', 0x0F);
                    cmd[cmd_len] = '\0';
                    
                    if (cmd_len > 0) {
                        // Check for local commands
                        if (cmd[0] == 'c' && cmd[1] == 'l' && cmd[2] == 'e' &&
                            cmd[3] == 'a' && cmd[4] == 'r' && cmd[5] == '\0') {
                            // Clear screen locally
                            for (int r = 0; r < TERM_ROWS; r++)
                                for (int c = 0; c < TERM_COLS; c++) {
                                    buf[r][c] = 0; col[r][c] = 0;
                                }
                            cx = 0; cy = 0;
                        } else {
                            // Execute command via kernel
                            sys_exec_cmd(cmd);
                            // Drain any IPC output
                            drain_ipc();
                        }
                    }
                    
                    print_prompt();
                    cmd_len = 0;
                    draw_terminal(wid);
                } else if (ev.key == '\b') {
                    if (cmd_len > 0) {
                        cmd_len--;
                        term_putchar('\b', 0x0F);
                        draw_terminal(wid);
                    }
                } else if (ev.key >= ' ' && ev.key <= '~' && cmd_len < 255) {
                    cmd[cmd_len++] = (char)ev.key;
                    term_putchar((char)ev.key, 0x0A);
                    draw_terminal(wid);
                }
            }
        }
        
        // Periodically drain IPC (for async output)
        frame++;
        if (frame % 50 == 0) {
            int old_cx = cx, old_cy = cy;
            drain_ipc();
            if (cx != old_cx || cy != old_cy) {
                draw_terminal(wid);
            }
        }
        
        sys_yield();
    }
}
