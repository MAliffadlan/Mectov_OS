// gcalc.c — Mectov OS GUI Calculator
// Compiled with -fno-pic -static -O0

#include "lib/libc.h"

// Define the global pointer to the shared library export table
void** __mct_lib_ptr = 0;

// GUI syscall constants are in syscall.h now.

// GUI Event already in libc.h



typedef struct {
    int type; // 1 = paint, 2 = key, 3 = mouse
    int x, y;
    int key;
} gui_event_t;



// --- App Logic ---
int win_id = -1;
char display_buf[32];
int buf_len = 0;

int operand1 = 0;
char operator = 0;
int new_number = 1;

char buttons[20] = {
    '7', '8', '9', '/',
    '4', '5', '6', '*',
    '1', '2', '3', '-',
    '0', '.', '=', '+',
    'C', ' ', ' ', ' '
};

void draw_ui() {
    // Background
    sys_draw_rect(win_id, 0, 0, 220, 300, 0x404040);
    
    // Display
    sys_draw_rect(win_id, 5, 30, 210, 40, 0x202020);
    if (buf_len == 0) {
        sys_draw_text(win_id, 10, 42, "0", 0xFFFFFF);
    } else {
        sys_draw_text(win_id, 10, 42, display_buf, 0xFFFFFF);
    }
    
    // Grid
    for (int i = 0; i < 20; i++) {
        if (buttons[i] == ' ') continue;
        int row = i / 4;
        int col = i % 4;
        int bx = 5 + col * 52;
        int by = 80 + row * 42;
        sys_draw_rect(win_id, bx, by, 48, 38, 0x606060);
        char lbl[2] = { buttons[i], '\0' };
        sys_draw_text(win_id, bx + 20, by + 12, lbl, 0xFFFFFF);
    }
    
    sys_update_window(win_id);
}

void handle_click(int mx, int my) {
    if (mx < 5 || mx > 215 || my < 80 || my > 290) return;
    int col = (mx - 5) / 52;
    int row = (my - 80) / 42;
    int idx = row * 4 + col;
    if (idx >= 0 && idx < 20) {
        char c = buttons[idx];
        if (c >= '0' && c <= '9') {
            if (new_number) { buf_len = 0; new_number = 0; }
            if (buf_len < 15) {
                display_buf[buf_len++] = c;
                display_buf[buf_len] = '\0';
            }
        } else if (c == 'C') {
            buf_len = 0; operand1 = 0; operator = 0; new_number = 1;
            display_buf[0] = '\0';
        } else if (c == '+' || c == '-' || c == '*' || c == '/') {
            if (buf_len > 0) {
                int val = 0;
                for (int i = 0; i < buf_len; i++) val = val * 10 + (display_buf[i] - '0');
                operand1 = val;
                operator = c;
                new_number = 1;
            }
        } else if (c == '=') {
            if (buf_len > 0 && operator != 0) {
                int val = 0;
                for (int i = 0; i < buf_len; i++) val = val * 10 + (display_buf[i] - '0');
                int res = 0;
                if (operator == '+') res = operand1 + val;
                if (operator == '-') res = operand1 - val;
                if (operator == '*') res = operand1 * val;
                if (operator == '/') res = val != 0 ? operand1 / val : 0;
                itoa(res, display_buf);
                buf_len = strlen(display_buf);
                operator = 0;
                new_number = 1;
            }
        }
        draw_ui();
    }
}

void _start() {
    __mct_lib_ptr = (void**)mct_load_library("apps/libc.mct");
    if (!__mct_lib_ptr) {
        syscall5(SYS_WRITE, 1, (int)"[GCALC] Failed to load libc.mct\n", 32, 0, 0); // fallback if sys_print fails
        sys_exit();
    }

    win_id = sys_create_window(100, 100, 220, 300, "Calculator");
    if (win_id < 0) sys_exit();

    draw_ui();

    gui_event_t ev;
    while (1) {
        int res = sys_get_event(win_id, &ev);
        if (res < 0) {
            sys_exit();
        } else if (res > 0) {
            if (ev.type == 1) {
                draw_ui();
            } else if (ev.type == 3 && (ev.key & 1)) {
                handle_click(ev.x, ev.y);
            }
        } else {
            sys_yield(); // YIELD
        }
    }
}
