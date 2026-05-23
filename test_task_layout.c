#include <stdio.h>
#include <stdint.h>
#define KERNEL_STACK_SIZE 16384
#define USER_STACK_SIZE   8192
#define MAX_TASKS 16
typedef struct {
    uint32_t esp;          
    uint8_t  kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));
    uint8_t  user_stack[USER_STACK_SIZE] __attribute__((aligned(16)));
    int      state;        
    uint8_t  ring;         
    uint32_t page_dir;     
    int      priority;     
    int      time_slice;   
    int      sleep_ticks;  
    int      wait_tid;     
} task_t;
task_t tasks[MAX_TASKS];
int main() {
    printf("Task size: %ld\n", sizeof(task_t));
    for (int i=0; i<4; i++) {
        printf("Task %d user_stack start: %p end: %p\n", i, tasks[i].user_stack, tasks[i].user_stack + USER_STACK_SIZE);
    }
    return 0;
}
