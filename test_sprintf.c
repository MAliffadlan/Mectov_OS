#include "apps/lib/libc.h"

void** __mct_lib_ptr;

void _start() {
    __mct_lib_ptr = (void**)mct_load_library("apps/libc.mct");
    if (!__mct_lib_ptr) sys_exit();
    
    char buf[128];
    sprintf(buf, "Test %d %s", 123, "hello");
    
    syscall5(SYS_WRITE, 1, (int)buf, 12, 0, 0);
    sys_exit();
}
