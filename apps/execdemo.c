// execdemo.c — Mectov OS exec() Demo (Ring 3)
// Completes the Unix fork/exec/waitpid trio:
//   fork()   — parent clones itself (COW)
//   exec()   — the child REPLACES its own image with /apps/execchild.mct
//   waitpid()— the parent blocks until the exec'd child exits, then reads
//              the exit status (7) that came from a DIFFERENT program image.
//
// Run it from the terminal:  run /apps/execdemo.mct
#include "src/include/syscall.h"

typedef struct { int type; int x, y; int key; } gui_event_t;

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
    int wid = sys_create_window(150, 80, 420, 260, "Exec Demo");
    if (wid < 0) sys_exit();

    sys_draw_rect(wid, 0, 0, 420, 260, 0x001E1E2E);
    sys_draw_text(wid, 10, 8, "Mectov OS fork + exec demo", 0x00F9E2AF);
    sys_update_window(wid);

    int pid = sys_fork();
    if (pid < 0) {
        sys_draw_text(wid, 10, 40, "fork() failed: no free task slot", 0x00F38BA8);
        sys_update_window(wid);
        sys_exit();
    }

    if (pid == 0) {
        // ================= CHILD =================
        sys_draw_text(wid, 10, 96, "Child: replacing my own image via exec()...", 0x00A6E3A1);
        sys_update_window(wid);
        sys_sleep(100);

        // exec() NEVER returns on success: the child's whole identity is now
        // execchild.mct. If it returns, something went wrong — report + exit.
        int r = sys_exec("/apps/execchild.mct", "from-execdemo");
        sys_draw_text(wid, 10, 112, "exec() returned (shouldn't happen):", 0x00F38BA8);
        draw_line(wid, 128, "  r = ", r, 0x00F38BA8);
        sys_update_window(wid);
        syscall(SYS_EXIT, 99, 0, 0);   // exec failure marker
        for (;;) ;
    }

    // ================= PARENT =================
    draw_line(wid, 40, "I am the parent, pid = ", sys_getpid(), 0x00A6E3A1);
    draw_line(wid, 56, "forked child pid = ", pid, 0x00A6E3A1);
    sys_draw_text(wid, 10, 72, "Child exec'd a DIFFERENT program. Waiting...", 0x00F9E2AF);
    sys_update_window(wid);

    // Block until the child (now execchild.mct) exits, then collect its status.
    // The exit code 7 proves the parent reaped a process whose image was
    // replaced mid-flight — classic fork+exec semantics.
    int status = -1;
    int r = sys_waitpid(pid, &status, 0);
    if (r == pid) {
        draw_line(wid, 96, "waitpid OK, exec'd child exit code = ", status, 0x00F9E2AF);
        sys_draw_text(wid, 10, 112, "fork + exec + waitpid all work!", 0x00A6E3A1);
    } else {
        draw_line(wid, 96, "waitpid returned ", r, 0x00F38BA8);
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
