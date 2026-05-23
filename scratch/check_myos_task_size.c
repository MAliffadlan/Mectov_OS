#include <stdio.h>
#include <stdint.h>

#define MAX_TASKS 64
#define KERNEL_STACK_SIZE 16384
#define USER_STACK_SIZE   8192

#define PRIORITY_REALTIME   2
#define PRIORITY_INTERACTIVE 1
#define PRIORITY_BACKGROUND  0

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
    printf("task_t size: %lu\n", sizeof(task_t));
    printf("64 * task_t size: %lu\n", 64 * sizeof(task_t));
    printf("8 * task_t size: %lu\n", 8 * sizeof(task_t));
    return 0;
}
