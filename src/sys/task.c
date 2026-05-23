#include "../include/task.h"
#include "../include/mem.h"
#include "../include/serial.h"
#include "../include/io.h"

#define MAX_TASKS 64
#define KERNEL_STACK_SIZE 16384
#define USER_STACK_SIZE   65536

// Task states
#define TASK_STATE_FREE    0
#define TASK_STATE_RUNNING 1
#define TASK_STATE_READY   2
#define TASK_STATE_SLEEP   3  // NEW: sleeping for N ticks

typedef struct {
    uint32_t esp;          // Saved stack pointer (points to register frame)
    uint8_t  kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));
    uint8_t  user_stack[USER_STACK_SIZE] __attribute__((aligned(16)));
    int      state;        // 0=free, 1=running, 2=ready, 3=sleep
    uint8_t  ring;         // 0 = kernel task, 3 = user task
    // === NEW FIELDS (add-on, safe defaults) ===
    int      priority;     // 0=background, 1=interactive, 2=realtime
    int      sleep_ticks;  // remaining ticks until wake (0 = not sleeping)
    uint32_t page_dir;     // per-process page directory (0 = global identity)
    int      fd_table[16]; // local file descriptors mapped to global FDs
    char     launch_arg[128]; // command-line argument passed at launch
} task_t;

static task_t tasks[MAX_TASKS];
static int current_task = -1;
static int num_tasks = 0;

void init_tasking() {
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_STATE_FREE;
        tasks[i].ring = 0;
        tasks[i].priority = PRIORITY_INTERACTIVE;
        tasks[i].sleep_ticks = 0;
        tasks[i].page_dir = 0;
        tasks[i].launch_arg[0] = '\0';
        for (int j = 0; j < 16; j++) tasks[i].fd_table[j] = -1;
    }
    tasks[0].state = TASK_STATE_RUNNING;
    tasks[0].ring = 0;
    tasks[0].priority = PRIORITY_INTERACTIVE;
    tasks[0].esp = 0; // Will be filled by scheduler on first preemption
    
    // Save boot CR3 to task 0
    uint32_t boot_cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(boot_cr3));
    tasks[0].page_dir = boot_cr3;
    
    current_task = 0;
    num_tasks = 1;
}

uint32_t tasks_get_boot_cr3(void) {
    return tasks[0].page_dir;
}

// Create a Ring 0 (kernel) task
int create_task(void (*entry)()) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == 0) {
            // CRITICAL: Disable interrupts to prevent scheduler from
            // picking up this half-initialized task!
            __asm__ volatile("cli");
            
            tasks[i].ring = 0;
            tasks[i].launch_arg[0] = '\0';
            for (int j = 0; j < 16; j++) tasks[i].fd_table[j] = -1;
            
            uint32_t* stack = (uint32_t*)&tasks[i].kernel_stack[KERNEL_STACK_SIZE];
            
            // Ring 0 interrupt frame
            *(--stack) = 0x202;      // EFLAGS (IF=1)
            *(--stack) = 0x08;       // CS (kernel code)
            *(--stack) = (uint32_t)(uintptr_t)entry; // EIP
            *(--stack) = 0;          // err_code
            *(--stack) = 0;          // int_no
            *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; // eax,ecx,edx,ebx
            *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; // esp,ebp,esi,edi
            *(--stack) = 0x10;       // DS (kernel data)
            
            tasks[i].esp = (uint32_t)stack;
            num_tasks++;
            
            // Set state LAST — only now is it safe for the scheduler
            tasks[i].state = 2;
            
            __asm__ volatile("sti");
            return i;
        }
    }
    return -1;
}

// Create a Ring 3 (user) task
int create_user_task(void (*entry)()) {
    write_serial_string("[TASK] create_user_task\n");
    
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == 0) {
            // CRITICAL: Disable interrupts to prevent scheduler from
            // picking up this half-initialized task!
            __asm__ volatile("cli");
            
            tasks[i].ring = 3;
            tasks[i].launch_arg[0] = '\0';
            for (int j = 0; j < 16; j++) tasks[i].fd_table[j] = -1;
            
            uint32_t* stack = (uint32_t*)&tasks[i].kernel_stack[KERNEL_STACK_SIZE];
            uint32_t user_esp = (uint32_t)&tasks[i].user_stack[USER_STACK_SIZE];
            
            // Ring 3 interrupt frame (iret pops: EIP, CS, EFLAGS, ESP, SS)
            *(--stack) = 0x23;       // SS  (user data 0x20 | RPL 3)
            *(--stack) = user_esp;   // ESP (user stack pointer)
            *(--stack) = 0x202;      // EFLAGS (IF=1, IOPL=0)
            *(--stack) = 0x1B;       // CS  (user code 0x18 | RPL 3)
            *(--stack) = (uint32_t)(uintptr_t)entry; // EIP
            *(--stack) = 0;          // err_code
            *(--stack) = 0;          // int_no
            *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; // eax,ecx,edx,ebx
            *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; // esp,ebp,esi,edi
            *(--stack) = 0x23;       // DS (user data)
            
            tasks[i].esp = (uint32_t)stack;
            tasks[i].page_dir = tasks[0].page_dir; // Default to kernel page dir
            num_tasks++;
            
            write_serial_string("User ESP: ");
            write_serial_hex(user_esp);
            write_serial_string("\n");
            
            // Set state LAST — only now is it safe for the scheduler
            tasks[i].state = 2;
            
            __asm__ volatile("sti");
            
            write_serial_string("[TASK] Ring 3 task created OK\n");
            return i;
        }
    }
    write_serial_string("[TASK] No free slots!\n");
    return -1;
}

extern void tss_set_kernel_stack(uint32_t stack);

// Shared cleanup for task termination (exit or kill)
static void task_cleanup(int tid) {
    if (tid <= 0 || tid >= MAX_TASKS) return;
    
    // 1. Clean up windows owned by this task
    extern void wm_cleanup_task(int tid);
    wm_cleanup_task(tid);
    
    // 2. Free address space (if it's not the kernel's)
    if (tasks[tid].page_dir != 0 && tasks[tid].page_dir != tasks[0].page_dir) {
        extern void vmm_free_address_space(uint32_t);
        vmm_free_address_space(tasks[tid].page_dir);
        tasks[tid].page_dir = 0;
    }
}

uint32_t schedule(uint32_t esp) {
    if (current_task < 0) return esp;
    
    // Auto-poll network on every schedule tick to process ARP/DNS/TCP packets in the background
    extern void net_poll(void);
    net_poll();
    
    // If we're the only task AND we're active, no need to switch
    if (num_tasks <= 1 && tasks[current_task].state != 0) return esp;
    
    // 1. Update sleeping tasks
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_STATE_SLEEP) {
            if (tasks[i].sleep_ticks > 0) {
                tasks[i].sleep_ticks--;
            }
            if (tasks[i].sleep_ticks <= 0) {
                tasks[i].state = TASK_STATE_READY;
            }
        }
    }

    // Save current task's register frame pointer
    tasks[current_task].esp = esp;
    if (tasks[current_task].state == TASK_STATE_RUNNING) {
        tasks[current_task].state = TASK_STATE_READY;
    }
    
    // Find next ready task (round-robin)
    int next = current_task;
    int found = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        next = (next + 1) % MAX_TASKS;
        if (tasks[next].state == TASK_STATE_READY) {
            found = 1;
            break;
        }
    }
    
    if (!found) {
        // If no other task is ready, keep running current (if it's not free/sleeping)
        if (tasks[current_task].state == TASK_STATE_READY || tasks[current_task].state == TASK_STATE_RUNNING) {
            next = current_task;
        } else {
            // Fallback to task 0 (kernel/idle)
            next = 0;
        }
    }
    
    tasks[next].state = 1;
    current_task = next;
    
    // CRITICAL: Update TSS.esp0 for the next task.
    // When a Ring 3 task gets interrupted, the CPU loads ESP from TSS.esp0.
    // It MUST point to the TOP of this task's kernel stack (empty, ready for pushes).
    // For Ring 0 tasks this is harmless (TSS.esp0 is unused for same-ring interrupts).
    tss_set_kernel_stack((uint32_t)&tasks[next].kernel_stack[KERNEL_STACK_SIZE]);
    
    // Switch page directory if different
    extern void vmm_switch_page_dir(uint32_t);
    if (tasks[next].page_dir != 0) {
        vmm_switch_page_dir(tasks[next].page_dir);
    } else {
        // Fallback to task 0's page_dir (boot cr3)
        vmm_switch_page_dir(tasks[0].page_dir);
    }
    
    return tasks[next].esp;
}

// Terminate the current task — called from SYS_EXIT syscall
void task_exit(void) {
    if (current_task <= 0) return; // Never kill task 0 (kernel)
    
    write_serial_string("[TASK] task_exit tid=");
    write_serial('0' + current_task);
    write_serial('\n');
    
    __asm__ volatile("cli");
    task_cleanup(current_task);
    tasks[current_task].state = TASK_STATE_FREE;
    num_tasks--;
    
    __asm__ volatile("sti");
    for (;;) __asm__("hlt");
}

int get_current_task(void) {
    return current_task;
}

// === NEW: Thread creation with priority + page_dir ===
int thread_create(void (*entry)(), int priority, uint32_t page_dir) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_STATE_FREE) {
            __asm__ volatile("cli");
            
            tasks[i].ring = 3;  // Threads are user tasks by default
            tasks[i].priority = priority;
            tasks[i].page_dir = page_dir;
            tasks[i].sleep_ticks = 0;
            for (int j = 0; j < 16; j++) tasks[i].fd_table[j] = -1;
            
            uint32_t* stack = (uint32_t*)&tasks[i].kernel_stack[KERNEL_STACK_SIZE];
            uint32_t user_esp = (uint32_t)&tasks[i].user_stack[USER_STACK_SIZE];
            
            // Ring 3 interrupt frame
            *(--stack) = 0x23;       // SS
            *(--stack) = user_esp;   // ESP
            *(--stack) = 0x202;      // EFLAGS
            *(--stack) = 0x1B;       // CS
            *(--stack) = (uint32_t)(uintptr_t)entry; // EIP
            *(--stack) = 0;          // err_code
            *(--stack) = 0;          // int_no
            *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; *(--stack) = 0;
            *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; *(--stack) = 0;
            *(--stack) = 0x23;       // DS
            
            tasks[i].esp = (uint32_t)stack;
            num_tasks++;
            tasks[i].state = TASK_STATE_READY;
            
            // NOTE: Interrupts stay DISABLED. Caller must call sti after
            // finishing any post-create setup (e.g. setting launch_arg).
            return i;
        }
    }
    return -1;
}

// Sleep the current task for N timer ticks
void task_sleep(int ticks) {
    if (current_task < 0 || ticks <= 0) return;
    
    __asm__ volatile("cli");
    tasks[current_task].sleep_ticks = ticks;
    tasks[current_task].state = TASK_STATE_SLEEP;
    __asm__ volatile("sti");
    
    // Yield — wait for scheduler to skip us until wake
    __asm__ volatile("sti");
    for (int i = 0; i < 100000; i++) {
        __asm__ volatile("pause");
        if (tasks[current_task].state != TASK_STATE_SLEEP) break;
    }
}

// Wake up a sleeping task
void task_wake(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return;
    if (tasks[tid].state == TASK_STATE_SLEEP) {
        tasks[tid].sleep_ticks = 0;
        tasks[tid].state = TASK_STATE_READY;
    }
}

// Get/set priority
int task_set_priority(int tid, int priority) {
    if (tid < 0 || tid >= MAX_TASKS) return -1;
    if (tasks[tid].state == TASK_STATE_FREE) return -1;
    if (priority < PRIORITY_BACKGROUND || priority > PRIORITY_REALTIME) return -1;
    tasks[tid].priority = priority;
    return 0;
}

int task_get_priority(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return -1;
    if (tasks[tid].state == TASK_STATE_FREE) return -1;
    return tasks[tid].priority;
}

// Get/set page directory
uint32_t task_get_page_dir(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return 0;
    return tasks[tid].page_dir;
}

void task_set_page_dir(int tid, uint32_t page_dir) {
    if (tid < 0 || tid >= MAX_TASKS) return;
    tasks[tid].page_dir = page_dir;
}

// Check if a task is alive
int task_is_alive(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return 0;
    return (tasks[tid].state != TASK_STATE_FREE);
}

int task_get_fd(int tid, int local_fd) {
    if (tid < 0 || tid >= MAX_TASKS) return -1;
    if (local_fd < 0 || local_fd >= 16) return -1;
    return tasks[tid].fd_table[local_fd];
}

void task_set_fd(int tid, int local_fd, int global_fd) {
    if (tid < 0 || tid >= MAX_TASKS) return;
    if (local_fd < 0 || local_fd >= 16) return;
    tasks[tid].fd_table[local_fd] = global_fd;
}

// Kill a specific task by ID (called from kernel or syscall)
int task_kill(int tid) {
    if (tid <= 0 || tid >= MAX_TASKS) return -1;
    if (tasks[tid].state == TASK_STATE_FREE) return -1;
    
    __asm__ volatile("cli");
    task_cleanup(tid);
    tasks[tid].state = TASK_STATE_FREE;
    num_tasks--;
    __asm__ volatile("sti");
    
    write_serial_string("[TASK] task_kill: killed task ");
    write_serial('0' + tid);
    write_serial('\n');
    return 0;
}

int get_task_info(int tid, task_info_t* info) {
    if (tid < 0 || tid >= MAX_TASKS) return 0;
    if (tasks[tid].state == TASK_STATE_FREE) return 0;
    
    info->id = tid;
    info->state = tasks[tid].state;
    info->ring = tasks[tid].ring;
    info->priority = tasks[tid].priority;
    info->sleep_ticks = tasks[tid].sleep_ticks;
    return 1;
}

void task_set_launch_arg(int tid, const char* arg) {
    if (tid < 0 || tid >= MAX_TASKS) return;
    int i = 0;
    if (arg) {
        for (; i < 127 && arg[i]; i++) tasks[tid].launch_arg[i] = arg[i];
    }
    tasks[tid].launch_arg[i] = '\0';
}

const char* task_get_launch_arg(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return "";
    return tasks[tid].launch_arg;
}
