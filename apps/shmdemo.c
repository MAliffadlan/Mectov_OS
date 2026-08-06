// shmdemo.c — Mectov OS Shared Memory Demo (Ring 3)
// Proves System V-style shm works across fork:
//   shmget(key, 4096) — create a 4 KB segment
//   fork()            — child inherits the address space (PAGE_SHARED pages
//                       are NOT COW'd, so both sides map the SAME frames)
//   shmat() in both   — child writes at shm_base+0, parent reads it back
//   shmdt() / shmctl(RMID)
//
// Run it from the terminal:  run /apps/shmdemo.mct
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
    while (label[i] && i < 50) { line[i] = label[i]; i++; }
    char num[16];
    itoa(value, num);
    int j = 0;
    while (num[j] && i < 63) { line[i++] = num[j]; j++; }
    line[i] = '\0';
    sys_draw_text(wid, 10, y, line, color);
}

void _start(void) {
    int wid = sys_create_window(150, 80, 420, 240, "Shm Demo");
    if (wid < 0) sys_exit();

    sys_draw_rect(wid, 0, 0, 420, 240, 0x001E1E2E);
    sys_draw_text(wid, 10, 8, "Mectov OS Shared Memory Demo", 0x00F9E2AF);
    sys_update_window(wid);

    // Create a 4 KB segment, then fork. Both parent and child will shmat the
    // same segment id, so the physical frames behind it are truly shared.
    int shmid = sys_shmget(0x51, 4096);
    if (shmid < 0) {
        sys_draw_text(wid, 10, 40, "shmget failed", 0x00F38BA8);
        sys_update_window(wid);
        sys_exit();
    }
    draw_line(wid, 40, "shmget -> id = ", shmid, 0x00A6E3A1);
    sys_update_window(wid);

    int pid = sys_fork();
    if (pid < 0) {
        sys_draw_text(wid, 10, 56, "fork() failed", 0x00F38BA8);
        sys_update_window(wid);
        sys_exit();
    }

    if (pid == 0) {
        // ================= CHILD =================
        sys_sleep(50);  // let the parent attach first
        unsigned int* shm = (unsigned int*)sys_shmat(shmid);
        if (!shm) {
            sys_draw_text(wid, 10, 96, "child: shmat failed", 0x00F38BA8);
            sys_update_window(wid);
            sys_exit();
        }
        sys_draw_text(wid, 10, 96, "child: wrote 0x51515 to shm", 0x00A6E3A1);
        sys_update_window(wid);
        shm[0] = 0x51515;   // write into the shared page
        sys_sleep(200);
        sys_shmdt((void*)shm);
        syscall(SYS_EXIT, 0, 0, 0);
        for (;;) ;
    }

    // ================= PARENT =================
    draw_line(wid, 56, "forked child pid = ", pid, 0x00A6E3A1);
    sys_update_window(wid);

    unsigned int* shm = (unsigned int*)sys_shmat(shmid);
    if (!shm) {
        sys_draw_text(wid, 10, 72, "parent: shmat failed", 0x00F38BA8);
        sys_update_window(wid);
        sys_exit();
    }
    sys_draw_text(wid, 10, 72, "parent: attached, waiting for child write...", 0x00F9E2AF);
    sys_update_window(wid);

    // Wait until the child wrote (or timeout), then read the shared value.
    int i;
    for (i = 0; i < 100; i++) {
        if (shm[0] != 0) break;
        sys_sleep(20);
    }
    if (shm[0] == 0x51515) {
        draw_line(wid, 88, "parent read 0x", 0, 0x00A6E3A1);
        sys_draw_text(wid, 10, 88, "parent read 0x51515 from SHARED memory!", 0x00A6E3A1);
        sys_draw_text(wid, 10, 104, "=> true sharing across fork (not COW)", 0x00F9E2AF);
    } else {
        sys_draw_text(wid, 10, 88, "parent: never saw child's write", 0x00F38BA8);
    }
    sys_update_window(wid);

    sys_shmdt((void*)shm);
    sys_shmctl(shmid, 0);   // IPC_RMID

    sys_waitpid(pid, &i, 0);

    // Keep the window open until ESC.
    gui_event_t ev;
    for (;;) {
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 2 && ev.key == 0x01) sys_exit();
        }
        sys_yield();
    }
}
