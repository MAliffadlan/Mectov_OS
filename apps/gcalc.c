// gcalc.c — Mectov OS GUI Calculator
// Compiled with -fno-pic -static -O0

typedef unsigned char uint8_t;
typedef unsigned int uint32_t;

// --- Syscall Wrappers ---
static inline int syscall3(int num, int a, int b, int c) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a), "c"(b), "d"(c));
    return ret;
}

static inline int syscall5(int num, int a, int b, int c, int d, int e) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e));
    return ret;
}

#define SYS_EXIT           10
#define SYS_DRAW_RECT      11
#define SYS_DRAW_TEXT      12
#define SYS_CREATE_WINDOW  15
#define SYS_GET_EVENT      16
#define SYS_UPDATE_WINDOW  17

typedef struct {
    int type; // 1 = paint, 2 = key, 3 = mouse
    int x, y;
    int key;
} gui_event_t;

int gui_create_window(int x, int y, int w, int h, const char* title) {
    return syscall5(SYS_CREATE_WINDOW, x, y, w, h, (int)title);
}

void gui_draw_rect(int wid, int x, int y, int w, int h, uint32_t color) {
    int wh = (w << 16) | (h & 0xFFFF);
    syscall5(SYS_DRAW_RECT, wid, x, y, wh, color);
}

void gui_draw_text(int wid, int x, int y, const char* text, uint32_t color) {
    syscall5(SYS_DRAW_TEXT, wid, x, y, (int)text, color);
}

int gui_get_event(int wid, gui_event_t* ev) {
    return syscall3(SYS_GET_EVENT, wid, (int)ev, 0);
}

void gui_update_window(int wid) {
    syscall3(SYS_UPDATE_WINDOW, wid, 0, 0);
}

void sys_exit() {
    syscall3(1, (int)"[GCALC] Exiting...\n", 0, 0);
    syscall3(SYS_EXIT, 0, 0, 0);
    for(;;);
}

// --- App Logic ---
int my_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

void my_itoa(int n, char* buf) {
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    int i = 0, sign = 0;
    if (n < 0) { sign = 1; n = -n; }
    while (n > 0) { buf[i++] = (n % 10) + '0'; n /= 10; }
    if (sign) buf[i++] = '-';
    buf[i] = '\0';
    for (int j = 0; j < i / 2; j++) {
        char t = buf[j]; buf[j] = buf[i - 1 - j]; buf[i - 1 - j] = t;
    }
}

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
    gui_draw_rect(win_id, 0, 0, 220, 300, 0x404040);
    
    // Display
    gui_draw_rect(win_id, 5, 30, 210, 40, 0x202020);
    if (buf_len == 0) {
        gui_draw_text(win_id, 10, 42, "0", 0xFFFFFF);
    } else {
        gui_draw_text(win_id, 10, 42, display_buf, 0xFFFFFF);
    }
    
    // Grid
    for (int i = 0; i < 20; i++) {
        if (buttons[i] == ' ') continue;
        int row = i / 4;
        int col = i % 4;
        int bx = 5 + col * 52;
        int by = 80 + row * 42;
        gui_draw_rect(win_id, bx, by, 48, 38, 0x606060);
        char lbl[2] = { buttons[i], '\0' };
        gui_draw_text(win_id, bx + 20, by + 12, lbl, 0xFFFFFF);
    }
    
    gui_update_window(win_id);
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
                my_itoa(res, display_buf);
                buf_len = my_strlen(display_buf);
                operator = 0;
                new_number = 1;
            }
        }
        draw_ui();
    }
}

void _start() {
    win_id = gui_create_window(100, 100, 220, 300, "Calculator");
    if (win_id < 0) sys_exit();

    draw_ui();

    gui_event_t ev;
    while (1) {
        int res = gui_get_event(win_id, &ev);
        if (res < 0) {
            sys_exit();
        } else if (res > 0) {
            if (ev.type == 1) {
                draw_ui();
            } else if (ev.type == 3 && (ev.key & 1)) {
                handle_click(ev.x, ev.y);
            }
        } else {
            syscall3(9, 0, 0, 0); // YIELD
        }
    }
}
