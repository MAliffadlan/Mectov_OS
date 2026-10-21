// terminal_pty.c — Mectov OS Terminal over PTY (v38.168)
//
// Demonstrates the pseudo-terminal end-to-end in one window:
//   * becomes the PTY MASTER (sys_pty_alloc) — the "terminal emulator" role
//   * forks a SLAVE-side echo process (fork + fds rewired to the slave fd),
//     which reads its stdin and echoes it back with a prompt — the "shell" role
//   * renders the slave's output in the window and feeds typed keys to the
//     master, exactly like a real terminal emulator feeding a shell
//
// Keystroke: window key event -> write master -> slave stdin -> echo process
// Display:   echo process stdout -> slave write -> master read -> window text.

#include "../src/include/syscall.h"

#define CW 520
#define CH 320
#define TERM_BG 0x00121018   // near-black terminal background
#define TERM_FG 0x00E8E8E8   // light gray text
#define TERM_PROMPT_COL 0x0080C080
#define LINE_MAX 120
#define SCREEN_LINES 12

// Screen model: a simple ring of last N lines (terminal scrollback).
static char lines[SCREEN_LINES][LINE_MAX];
static int  line_len[SCREEN_LINES];
static int  cur_line = 0;          // index of the line being read/written

// Input line the user is typing.
static char input[LINE_MAX];
static int  input_len = 0;

static int  wid = -1;
static int  master_fd = -1;        // local fd of the PTY master (emulator side)
static int  child_tid = -1;        // the echo process on the slave

// ---- small helpers (no libc) ----
static void draw_lines(void);

// Char-cell monospace-ish layout: the GUI draws proportional text, so we just
// approximate a cell grid of 8x14 px (close to the WM's default font).
#define CELL_W 8
#define CELL_H 14

// Render the scrollback ring to the window.
static void draw_lines(void) {
    sys_draw_rect(wid, 0, 0, CW, CH, TERM_BG);
    int x = 4, y = 4;
    for (int i = 0; i < SCREEN_LINES; i++) {
        int idx = (cur_line + 1 + i) % SCREEN_LINES;
        if (line_len[idx] > 0) {
            sys_draw_text(wid, x, y, lines[idx], TERM_FG);
        }
        y += CELL_H;
    }
    // Input line + cursor block.
    int iy = CH - CELL_H - 4;
    char prompt[LINE_MAX + 2];
    int p = 0;
    prompt[p++] = '$'; prompt[p++] = ' ';
    for (int i = 0; i < input_len && i < LINE_MAX - 4; i++) prompt[p++] = input[i];
    prompt[p++] = '_'; prompt[p] = 0;
    sys_draw_text(wid, x, iy, prompt, TERM_PROMPT_COL);
}

static void append_line(const char* s) {
    // Break `s` on \n and store up to SCREEN_LINES lines.
    while (*s) {
        int idx = cur_line;
        line_len[idx] = 0;
        while (*s && *s != '\n' && line_len[idx] < LINE_MAX - 1) {
            lines[idx][line_len[idx]++] = *s++;
        }
        lines[idx][line_len[idx]] = 0;
        if (*s == '\n') s++;
        cur_line = (cur_line + 1) % SCREEN_LINES;
    }
}

// Drain everything the slave has produced so far into the screen model.
static void drain_slave_output(void) {
    char buf[512];
    for (;;) {
        int r = sys_read(master_fd, buf, sizeof(buf));
        if (r <= 0) break;
        buf[r] = 0;
        append_line(buf);
    }
    draw_lines();
}

// ---- The slave-side "shell": an echo process on the PTY slave ----
// This is exactly what a real shell does: its fds 0/1 are the slave fd; it
// reads stdin, writes stdout. Here it echoes with a prompt so the terminal
// window visibly round-trips.
static void echo_main(int slave_fd) {
    char in[256];
    for (;;) {
        int r = sys_read(slave_fd, in, sizeof(in) - 1);
        if (r <= 0) { sys_sleep(20); continue; }   // non-blocking ring poll
        in[r] = 0;
        // Echo back: prompt + the typed text + newline.
        sys_write(slave_fd, "echo> ", 6);
        sys_write(slave_fd, in, r);
        sys_write(slave_fd, "\n", 1);
        if (in[0] == 'q' && in[1] == 0) break;     // 'q' + enter exits
    }
    sys_exit();
}

// ---- Fork the slave-side process with fds rewired to the slave ----
static void spawn_slave(int slave_fd) {
    int tid = sys_fork();
    if (tid == 0) {
        // Child: rewire fds 0 (stdin) and 1 (stdout) to the slave fd, drop fds
        // inherited from the parent), then run the echo loop.
        if (slave_fd != 0) sys_dup2(slave_fd, 0);
        if (slave_fd != 1) sys_dup2(slave_fd, 1);
        if (slave_fd > 1) sys_close(slave_fd);   // keep 0/1 as the slave ends
        echo_main(0);
        sys_exit();
    }
    // Parent (this app) keeps the child's tid; unused further.
    child_tid = tid;
}

// Feed a typed key to the master (the emulator's input side).
static void feed_master(char c) {
    sys_write(master_fd, &c, 1);
    if (c == '\n') {          // Enter: the slave consumed the line
        input_len = 0;
        input[0] = 0;
    } else if (input_len < LINE_MAX - 1) {
        input[input_len++] = c;
        input[input_len] = 0;
    }
    drain_slave_output();     // keys may have already produced echo output
}

// ====== MAIN ======
typedef struct { int type; int x, y; int key; } gui_event_t;

void _start() {
    wid = sys_create_window(90, 70, CW, CH, "Terminal (PTY)");
    if (wid < 0) sys_exit();

    master_fd = sys_pty_alloc();
    if (master_fd < 0) {
        sys_draw_text(wid, 8, 8, "pty alloc failed", 0x00FF5555);
        return;
    }
    int slave_fd = sys_pty_open(master_fd);
    if (slave_fd < 0) {
        sys_draw_text(wid, 8, 8, "pty open slave failed", 0x00FF5555);
        return;
    }

    spawn_slave(slave_fd);
    // The parent no longer needs the slave fd itself (the child owns 0/1).
    sys_close(slave_fd);

    append_line("pty terminal ready — type keys; 'q' kills the echo shell");
    draw_lines();

    gui_event_t ev;
    while (1) {
        int r = sys_get_event(wid, &ev);
        if (r < 0) break;               // window closed
        if (r > 0) {
            if (ev.type == 1) {         // repaint
                draw_lines();
            } else if (ev.type == 2) {  // key
                char c = (char)(ev.key & 0xFF);
                if (ev.key == 0x01000000 + 28) { c = '\n'; } // Enter
                feed_master(c);
            }
        } else {
            drain_slave_output();          // idle: keep pulling slave output
            sys_sleep(20);
        }
    }
    if (child_tid > 0) sys_kill(child_tid, 9);
    sys_exit();
}
