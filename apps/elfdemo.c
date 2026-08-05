#include "src/include/syscall.h"

// Simple in-app itoa for showing the launch argument length
static void app_itoa(int n, char* buf) {
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    int i = 0;
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    buf[i] = '\0';
    for (int j = 0; j < i / 2; j++) { char t = buf[j]; buf[j] = buf[i-1-j]; buf[i-1-j] = t; }
}

// gui_event_t layout mirrors the kernel's wm_event_t
typedef struct {
    int type; // 1 = paint, 2 = key, 3 = mouse
    int x, y;
    int key;
} gui_event_t;

void _start() {
    sys_print("[ELF] elfdemo starting from ELF binary in Ring 3...\n", 0x0E);

    // Read launch arg (e.g. "jalankan /apps/elfdemo.elf some_arg")
    char argbuf[64];
    int arglen = sys_get_launch_arg(argbuf, sizeof(argbuf) - 1);
    if (arglen < 0) arglen = 0;
    argbuf[arglen] = '\0';

    int wid = sys_create_window(120, 90, 320, 200, "ELF Demo");
    if (wid < 0) sys_exit();

    gui_event_t ev;
    int tick = 0;

    while (1) {
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 1) { // Paint
                sys_draw_rect(wid, 0, 0, 320, 200, 0x1B1B2B);
                sys_draw_text(wid, 40, 40, "This app is a real ELF binary!", 0x00FF88);
                sys_draw_text(wid, 40, 64, "Loaded by the in-kernel ELF loader", 0x88CCFF);
                sys_draw_text(wid, 40, 88, "Entry point from e_entry,", 0xA0A0B0);
                sys_draw_text(wid, 40, 104, "segments from PT_LOAD headers.", 0xA0A0B0);
                char aline[40];
                aline[0] = 'A'; aline[1] = 'r'; aline[2] = 'g'; aline[3] = ':'; aline[4] = ' ';
                int ai = 5;
                if (argbuf[0]) {
                    for (int i = 0; argbuf[i] && ai < 38; i++) aline[ai++] = argbuf[i];
                } else {
                    aline[ai++] = '('; aline[ai++] = 'n'; aline[ai++] = 'o'; aline[ai++] = 'n'; aline[ai++] = 'e'; aline[ai++] = ')';
                }
                aline[ai] = '\0';
                sys_draw_text(wid, 40, 128, aline, 0xFFFF66);
                sys_update_window(wid);
            } else if (ev.type == 2) { // Key: ESC exits
                if (ev.key == 0x01) sys_exit();
            }
        }
        tick++;
        if (tick % 200000 == 0) {
            sys_draw_rect(wid, 0, 0, 320, 200, (tick & 1) ? 0x1B1B2B : 0x232338);
            sys_update_window(wid);
        }
        sys_yield();
    }
}
