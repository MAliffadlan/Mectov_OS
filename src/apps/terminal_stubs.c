// terminal_stubs.c — Stubs for terminal functions needed by kernel
// The real Terminal now runs in Ring 3, but kernel code still references these.

#include "../include/types.h"

// These are no longer used since terminal is in Ring 3,
// but kernel code (shell, syscall, wm) still references them.

int use_term_buf = 0;
int term_app_running = 0;
int term_app_task_id = -1;
int term_open = 0;

static int stub_win_id = -1;

void term_putchar(char c, unsigned char col) {
    (void)c; (void)col;
    // IPC redirect handles this now via p_char -> ipc_try_send
}

void term_print(const char* s, unsigned char col) {
    (void)s; (void)col;
}

void p_char_gui(char c, unsigned char col) {
    (void)c; (void)col;
}

int get_use_term_buf() {
    return 0; // Never redirect to old term buf
}

void term_clear() {}

void term_clear_line() {}

int term_get_cx() { return 0; }
int term_get_cy() { return 0; }

int get_term_win_id() { return stub_win_id; }

void on_terminal_close() {
    term_open = 0;
}

void term_app_push_key(unsigned char sc) { (void)sc; }
unsigned char term_app_pop_key() { return 0; }
