#include "../include/task.h"
#include "../include/mem.h"

#define MAX_TASKS 8
#define KERNEL_STACK_SIZE 16384
#define USER_STACK_SIZE   8192

typedef struct {
    uint32_t esp;          // Saved stack pointer (points to register frame)
    uint8_t  kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));
    uint8_t  user_stack[USER_STACK_SIZE] __attribute__((aligned(16)));
    int      state;        // 0=free, 1=running, 2=ready
    uint8_t  ring;         // 0 = kernel task, 3 = user task
} task_t;

static task_t tasks[MAX_TASKS];
static int current_task = -1;
static int num_tasks = 0;

void init_tasking() {
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = 0;
        tasks[i].ring = 0;
    }
    // Task 0 = kernel main loop (already running on boot stack)
    tasks[0].state = 1;
    tasks[0].ring = 0;
    tasks[0].esp = 0; // Will be filled by scheduler on first preemption
    current_task = 0;
    num_tasks = 1;
}

// Create a Ring 0 (kernel) task
int create_task(void (*entry)()) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == 0) {
            tasks[i].state = 2; // Ready
            tasks[i].ring = 0;
            
            // Build a fake interrupt frame on the kernel stack
            // This frame will be "restored" by irq_common_stub
            uint32_t* stack = (uint32_t*)&tasks[i].kernel_stack[KERNEL_STACK_SIZE];
            
            // Ring 0 interrupt frame (CPU does NOT push SS/ESP for same-ring)
            // iret will pop: EIP, CS, EFLAGS
            *(--stack) = 0x202;      // EFLAGS (IF=1)
            *(--stack) = 0x08;       // CS (kernel code)
            *(--stack) = (uint32_t)(uintptr_t)entry; // EIP
            
            // err_code, int_no (skipped by add esp,8)
            *(--stack) = 0;          // err_code
            *(--stack) = 0;          // int_no
            
            // pushad order: EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI
            *(--stack) = 0; // eax
            *(--stack) = 0; // ecx
            *(--stack) = 0; // edx
            *(--stack) = 0; // ebx
            *(--stack) = 0; // esp (ignored by popad)
            *(--stack) = 0; // ebp
            *(--stack) = 0; // esi
            *(--stack) = 0; // edi
            
            // Saved DS (kernel data segment)
            *(--stack) = 0x10;
            
            tasks[i].esp = (uint32_t)stack;
            num_tasks++;
            return i;
        }
    }
    return -1;
}

// Create a Ring 3 (user) task
int create_user_task(void (*entry)()) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == 0) {
            tasks[i].state = 2; // Ready
            tasks[i].ring = 3;
            
            // Build a fake interrupt frame on the KERNEL stack.
            // When irq_common_stub restores this, iret sees CS=0x1B (Ring 3)
            // and automatically pops SS:ESP too → jumps to user mode.
            uint32_t* stack = (uint32_t*)&tasks[i].kernel_stack[KERNEL_STACK_SIZE];
            
            // User stack top
            uint32_t user_esp = (uint32_t)&tasks[i].user_stack[USER_STACK_SIZE];
            
            // Ring 3 interrupt frame (iret pops: EIP, CS, EFLAGS, ESP, SS)
            *(--stack) = 0x23;       // SS  (user data 0x20 | RPL 3)
            *(--stack) = user_esp;   // ESP (user stack pointer)
            *(--stack) = 0x202;      // EFLAGS (IF=1, IOPL=0)
            *(--stack) = 0x1B;       // CS  (user code 0x18 | RPL 3)
            *(--stack) = (uint32_t)(uintptr_t)entry; // EIP (user entry point)
            
            // err_code, int_no (skipped by add esp,8)
            *(--stack) = 0;          // err_code
            *(--stack) = 0;          // int_no
            
            // pushad: EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI
            *(--stack) = 0; // eax
            *(--stack) = 0; // ecx
            *(--stack) = 0; // edx
            *(--stack) = 0; // ebx
            *(--stack) = 0; // esp (ignored by popad)
            *(--stack) = 0; // ebp
            *(--stack) = 0; // esi
            *(--stack) = 0; // edi
            
            // Saved DS (user data segment)
            *(--stack) = 0x23;
            
            tasks[i].esp = (uint32_t)stack;
            num_tasks++;
            return i;
        }
    }
    return -1;
}

extern void tss_set_kernel_stack(uint32_t stack);

uint32_t schedule(uint32_t esp) {
    if (num_tasks <= 1 || current_task < 0) return esp;
    
    // Save current task's register frame pointer
    tasks[current_task].esp = esp;
    if (tasks[current_task].state == 1) tasks[current_task].state = 2;
    
    // Find next ready task (round-robin)
    int next = current_task;
    do {
        next = (next + 1) % MAX_TASKS;
    } while (tasks[next].state != 2);
    
    tasks[next].state = 1;
    current_task = next;
    
    // CRITICAL: Update TSS.esp0 for the next task.
    // When a Ring 3 task gets interrupted, the CPU loads ESP from TSS.esp0.
    // It MUST point to the TOP of this task's kernel stack (empty, ready for pushes).
    // For Ring 0 tasks this is harmless (TSS.esp0 is unused for same-ring interrupts).
    tss_set_kernel_stack((uint32_t)&tasks[next].kernel_stack[KERNEL_STACK_SIZE]);
    
    return tasks[next].esp;
}
