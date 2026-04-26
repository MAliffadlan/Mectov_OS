#include "../include/task.h"
#include "../include/mem.h"

#define MAX_TASKS 8
#define STACK_SIZE 4096

typedef struct {
    uint32_t esp;
    uint8_t  stack[STACK_SIZE] __attribute__((aligned(16)));
    int      state; // 0=free, 1=running, 2=ready
} task_t;

static task_t tasks[MAX_TASKS];
static int current_task = -1;
static int num_tasks = 0;

void init_tasking() {
    for (int i = 0; i < MAX_TASKS; i++) tasks[i].state = 0;
    tasks[0].state = 1; // Current kernel thread is task 0
    current_task = 0;
    num_tasks = 1;
}

int create_task(void (*entry)()) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == 0) {
            tasks[i].state = 2; // Ready
            
            // Set up stack
            uint32_t* stack = (uint32_t*)&tasks[i].stack[STACK_SIZE];
            
            // push eflags, cs, eip
            *(--stack) = 0x202;      // EFLAGS (Interrupts enabled)
            *(--stack) = 0x08;       // CS
            *(--stack) = (uint32_t)entry; // EIP
            
            // push err_code, int_no
            *(--stack) = 0;
            *(--stack) = 0;
            
            // pushad: eax, ecx, edx, ebx, esp, ebp, esi, edi
            *(--stack) = 0; // eax
            *(--stack) = 0; // ecx
            *(--stack) = 0; // edx
            *(--stack) = 0; // ebx
            *(--stack) = 0; // esp (ignored by popad)
            *(--stack) = 0; // ebp
            *(--stack) = 0; // esi
            *(--stack) = 0; // edi
            
            // push ds
            *(--stack) = 0x10; // ds
            
            tasks[i].esp = (uint32_t)stack;
            num_tasks++;
            return i;
        }
    }
    return -1;
}

uint32_t schedule(uint32_t esp) {
    if (num_tasks <= 1 || current_task < 0) return esp;
    
    // Save current task state
    tasks[current_task].esp = esp;
    if (tasks[current_task].state == 1) tasks[current_task].state = 2;
    
    // Find next task
    do {
        current_task = (current_task + 1) % MAX_TASKS;
    } while (tasks[current_task].state != 2);
    
    tasks[current_task].state = 1;
    return tasks[current_task].esp;
}
