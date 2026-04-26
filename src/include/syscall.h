#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"

// Syscall numbers
#define SYS_PRINT       0
#define SYS_GETKEY      1
#define SYS_MALLOC      2
#define SYS_FREE        3
#define SYS_GET_TICKS   4
#define SYS_YIELD       5
#define SYS_EXIT        6

// Initialize syscall handler (int 0x80)
void init_syscalls(void);

// Switch current execution to Ring 3 user mode
void switch_to_user_mode(void);

// Syscall interface for user programs (called from Ring 3)
static inline int syscall(int num, int arg1, int arg2, int arg3) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3)
    );
    return ret;
}

#endif
