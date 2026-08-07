// pipegen.c — pipeline producer: writes to fd 1 (stdout) via SYS_WRITE.
// Used to prove the real fork/exec pipeline: `run /apps/pipegen.mct | run /apps/piperead.mct`
#include "src/include/syscall.h"

void _start(void) {
    // The kernel's SYS_WRITE routes fd 1 to the pipe when the shell wired it
    // up (dup2 onto the pipe write end). Build the payload on the stack to
    // avoid any literal-section quirks.
    char msg[24];
    const char* src = "PIPEGEN: hello through the pipe!\n";
    int i = 0;
    while (src[i] && i < 23) { msg[i] = src[i]; i++; }
    msg[i] = '\0';

    int n = sys_write(1, msg, i);
    // Announce how many bytes made it into the pipe (serial + terminal).
    char num[8];
    num[0] = 'P'; num[1] = 'G'; num[2] = ':'; num[3] = ' ';
    num[4] = '0' + (n / 10); num[5] = '0' + (n % 10);
    num[6] = '\n'; num[7] = '\0';
    sys_write(1, num, 7);

    sys_exit();
    for (;;) ;
}
