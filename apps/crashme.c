// crashme.c — deliberately crashes a Ring 3 app to test the exception
// handler. Executes `ud2` (invalid opcode) which raises #UD (int 6): the
// kernel must log "[EXCEPTION] int_no=6", kill the task cleanly, and leave
// the OS running. Run it:  run /apps/crashme.mct
#include "src/include/syscall.h"

void _start(void) {
    sys_print("CRASHME: about to execute ud2 (invalid opcode)\n", 0x0E);
    // Executing an undefined instruction faults with #UD (vector 6). Before
    // the exception-stub fix this arrived as isr_default's fake int 255 and
    // a misaligned frame; now it must surface as a real int_no=6 crash.
    __asm__ volatile("ud2");
    // Never reached: the kernel kills this task in the #UD handler.
    sys_print("CRASHME: SURVIVED?! this should never print\n", 0x0C);
    sys_exit();
}
