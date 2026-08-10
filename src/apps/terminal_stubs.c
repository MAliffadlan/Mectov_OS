// terminal_stubs.c — Terminal glue for the kernel.
// The real Terminal runs in Ring 3, but kernel code still references these
// globals. term_app_push_key/term_app_pop_key are NOT stubs: they are the
// foreground-app keyboard buffer (single-consumer keyboard model, v38.9).

#include "../include/types.h"
#include "../include/spinlock.h"

int use_term_buf = 0;
int term_app_running = 0;
int term_app_task_id = -1;
int term_open = 0;

// ---- Foreground-app keyboard buffer ----
//
// Single-consumer keyboard model: the main loop (task 0) is the ONLY reader
// of the PS/2 scancode ring (kbd_buffer). While a foreground app owns the
// terminal (term_app_running), the main loop resolves each key to a char and
// pushes it here; SYS_GET_KEY pops it. SYS_GET_KEY never reads kbd_buffer
// directly, so there is no SMP race where an app task and the main loop steal
// keys from each other nondeterministically (seen on 4-core KVM).
#define APP_KBD_BUF_SIZE 128
static unsigned char app_kbd_buf[APP_KBD_BUF_SIZE];
static volatile uint32_t app_kbd_head = 0;
static volatile uint32_t app_kbd_tail = 0;
static spinlock_t app_kbd_lock = SPINLOCK_INIT;

void term_app_push_key(unsigned char c) {
    uint32_t ef = spin_lock_irqsave(&app_kbd_lock);
    uint32_t next = (app_kbd_head + 1) % APP_KBD_BUF_SIZE;
    if (next != app_kbd_tail) {
        app_kbd_buf[app_kbd_head] = c;
        app_kbd_head = next;
    }
    spin_unlock_irqrestore(&app_kbd_lock, ef);
}

unsigned char term_app_pop_key() {
    uint32_t ef = spin_lock_irqsave(&app_kbd_lock);
    unsigned char c = 0;
    if (app_kbd_head != app_kbd_tail) {
        c = app_kbd_buf[app_kbd_tail];
        app_kbd_tail = (app_kbd_tail + 1) % APP_KBD_BUF_SIZE;
    }
    spin_unlock_irqrestore(&app_kbd_lock, ef);
    return c;
}

// Drop any queued keys — called when the foreground app changes or exits so
// stale keystrokes never leak into the next app.
void term_app_key_clear(void) {
    uint32_t ef = spin_lock_irqsave(&app_kbd_lock);
    app_kbd_head = app_kbd_tail = 0;
    spin_unlock_irqrestore(&app_kbd_lock, ef);
}

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

