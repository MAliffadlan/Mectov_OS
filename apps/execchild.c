// execchild.c — target program for exec() (headless, no window).
// The child of execdemo forks, then exec()s this image. It prints its
// launch arg to the serial log and exits with status 7, which the parent
// collects via waitpid(). Run it from the terminal:
//   run /apps/execdemo.mct
#include "src/include/syscall.h"

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

void _start(void) {
    char arg[64];
    int n = sys_get_launch_arg(arg, 64);

    // All of this goes to the serial log (SYS_PRINT writes both VGA + serial),
    // which is what the automated exec test greps for.
    sys_print("EXECCHILD: I am the exec'd image, pid=", 0x0B);
    char num[16];
    itoa(sys_getpid(), num);
    sys_print(num, 0x0B);
    sys_print(" ppid=", 0x0B);
    itoa(sys_getppid(), num);
    sys_print(num, 0x0B);
    sys_print(" arg='", 0x0B);
    if (n > 0) sys_print(arg, 0x0B);
    sys_print("'", 0x0B);
    sys_print("\n", 0x0B);

    // Give the parent's waitpid a moment to block, then exit with status 7.
    sys_sleep(50);
    syscall(SYS_EXIT, 7, 0, 0);
    for (;;) ;
}
