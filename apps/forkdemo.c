// forkdemo.c — Mectov OS Process Model Demo (Ring 3)
// Proves the new Unix-style process API end to end:
//   fork()      — COW clone of this task
//   waitpid()   — parent blocks until the child exits
//   exit(code)  — child exits with status 42
//   getppid()   — child asks who its parent is
//   signal()    — SIGUSR1 handler delivered asynchronously to the child
//
// Run it from the terminal:  run /apps/forkdemo.mct
#include "src/include/syscall.h"

typedef struct { int type; int x, y; int key; } gui_event_t;

static int caught = 0;   // set by the SIGUSR1 handler

static void itoa(int n, char* buf) {
    int i = 0;
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[12];
    int t = 0;
    if (n < 0) { buf[i++] = '-'; n = -n; }
    while (n > 0) { tmp[t++] = '0' + n % 10; n /= 10; }
    while (t > 0) buf[i++] = tmp[--t];
    buf[i] = '\0';
}

// Signal handler: runs in the child's user context when the parent sends
// SIGUSR1. The kernel pushes a saved frame on the user stack, calls
// handler(sig), and the return-address trampoline performs the sigreturn.
static void on_sigusr1(int sig) {
    (void)sig;
    caught = 1;
}

static void draw_line(int wid, int y, const char* label, int value, uint32_t color) {
    char line[64];
    int i = 0;
    while (label[i] && i < 55) { line[i] = label[i]; i++; }
    char num[16];
    itoa(value, num);
    int j = 0;
    while (num[j] && i < 63) { line[i++] = num[j]; j++; }
    line[i] = '\0';
    sys_draw_text(wid, 10, y, line, color);
}

void _start(void) {
    int wid = sys_create_window(150, 80, 420, 280, "Fork Demo");
    if (wid < 0) sys_exit();

    sys_draw_rect(wid, 0, 0, 420, 280, 0x001E1E2E);
    sys_draw_text(wid, 10, 8, "Mectov OS Process Model Demo", 0x00F9E2AF);
    sys_update_window(wid);

    // Registered before fork(), so the child inherits the handler.
    sys_signal(SIGUSR1, (void*)on_sigusr1);

    int pid = sys_fork();
    if (pid < 0) {
        sys_draw_text(wid, 10, 40, "fork() failed: no free task slot", 0x00F38BA8);
        sys_update_window(wid);
        sys_exit();
    }

    if (pid == 0) {
        // ================= CHILD =================
        sys_draw_text(wid, 10, 96, "I am the CHILD (COW clone)", 0x00A6E3A1);
        draw_line(wid, 112, "child pid  = ", sys_getpid(), 0x00A6E3A1);
        draw_line(wid, 128, "parent pid = ", sys_getppid(), 0x00A6E3A1);
        sys_update_window(wid);

        // Wait for SIGUSR1 from the parent (100ms sleep slices keep the
        // delivery point reachable), then exit with status 42.
        int i;
        for (i = 0; i < 40; i++) {
            if (caught) break;
            sys_sleep(100);
        }
        if (caught) {
            sys_draw_text(wid, 10, 144, "Child caught SIGUSR1!", 0x00F9E2AF);
            sys_update_window(wid);
        }
        for (i = 0; i < 15; i++) sys_sleep(100);
        syscall(SYS_EXIT, 42, 0, 0);   // exit(42)
        for (;;) ;
    }

    // ================= PARENT =================
    draw_line(wid, 40, "I am the parent, pid = ", sys_getpid(), 0x00A6E3A1);
    draw_line(wid, 56, "forked child pid = ", pid, 0x00A6E3A1);
    sys_update_window(wid);

    // Ping the child, then block until it exits and collect its status.
    sys_kill(pid, SIGUSR1);
    int status = -1;
    int r = sys_waitpid(pid, &status, 0);
    if (r == pid) {
        draw_line(wid, 80, "waitpid OK, child exit code = ", status, 0x00F9E2AF);
    } else {
        draw_line(wid, 80, "waitpid returned ", r, 0x00F38BA8);
    }
    sys_update_window(wid);

    // Keep the window open until ESC.
    gui_event_t ev;
    for (;;) {
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 2 && ev.key == 0x01) sys_exit();
        }
        sys_yield();
    }
}
