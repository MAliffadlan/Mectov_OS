#include <stdio.h>
typedef unsigned int uint32_t;
typedef unsigned char uint8_t;
#define KERNEL_STACK_SIZE 16384
#define USER_STACK_SIZE   8192
typedef struct {
    uint32_t esp;
    uint8_t  kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));
    uint8_t  user_stack[USER_STACK_SIZE] __attribute__((aligned(16)));
    int      state;
    uint8_t  ring;
    int      priority;
    int      sleep_ticks;
    uint32_t page_dir;
    int      fd_table[16];
    char     launch_arg[128];
} task_t;
task_t tasks[16];
int main() {
    printf("tasks start = 0x%x\n", 0x004623a0);
    printf("sizeof(task_t) = 0x%x\n", (unsigned int)sizeof(task_t));
    printf("tasks[1] start = 0x%x\n", (unsigned int)(0x004623a0 + sizeof(task_t)));
    printf("tasks[1].user_stack start = 0x%x\n", (unsigned int)(0x004623a0 + sizeof(task_t) + __builtin_offsetof(task_t, user_stack)));
    printf("tasks[1].user_stack end = 0x%x\n", (unsigned int)(0x004623a0 + sizeof(task_t) + __builtin_offsetof(task_t, user_stack) + USER_STACK_SIZE));
    return 0;
}
