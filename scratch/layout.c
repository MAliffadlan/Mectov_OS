#include <stdio.h>
#include <stdint.h>

#define KERNEL_STACK_SIZE 16384
#define USER_STACK_SIZE   8192

typedef struct {
    uint32_t esp;          // Saved stack pointer (points to register frame)
    uint8_t  kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));
    uint8_t  user_stack[USER_STACK_SIZE] __attribute__((aligned(16)));
    int      state;        // 0=free, 1=running, 2=ready, 3=sleep
    uint8_t  ring;         // 0 = kernel task, 3 = user task
    int      priority;     // 0=background, 1=interactive, 2=realtime
    int      sleep_ticks;  // remaining ticks until wake (0 = not sleeping)
    uint32_t page_dir;     // per-process page directory (0 = global identity)
    int      fd_table[16]; // local file descriptors mapped to global FDs
    char     launch_arg[128]; // command-line argument passed at launch
} task_t;

int main() {
    printf("sizeof(task_t) = %zu (0x%zx)\n", sizeof(task_t), sizeof(task_t));
    task_t dummy;
    printf("offset of kernel_stack = %zu (0x%zx)\n", (char*)&dummy.kernel_stack - (char*)&dummy, (char*)&dummy.kernel_stack - (char*)&dummy);
    printf("offset of user_stack = %zu (0x%zx)\n", (char*)&dummy.user_stack - (char*)&dummy, (char*)&dummy.user_stack - (char*)&dummy);
    printf("offset of state = %zu (0x%zx)\n", (char*)&dummy.state - (char*)&dummy, (char*)&dummy.state - (char*)&dummy);
    return 0;
}
