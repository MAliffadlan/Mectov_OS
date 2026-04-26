#include "../include/security.h"
#include "../include/vga.h"
#include "../include/speaker.h"

int is_locked = 0;

void lock_screen() {
    is_locked = 1;
    beep(); c_work();
    d_win(WIN_X, WIN_Y, WIN_W, WIN_H, " Mectov Secure Lock ");
    print("\nSystem Locked. Please enter password.\nPassword: ", 0x0E);
}
