#include "../include/task.h"
#include "../include/mem.h"
#include "../include/serial.h"
#include "../include/io.h"
#include "../include/spinlock.h"
#include "../include/apic.h"
#include "../include/vmm.h"   // vmm_setup_user_stack(), USER_STACK_* layout

static spinlock_t task_lock = SPINLOCK_INIT;

#define MAX_TASKS 64
#define KERNEL_STACK_SIZE 16384
// USER_STACK_SIZE now lives in vmm.h — the Ring 3 stack is mapped into the
// task's own address space, not carved out of this struct.

// Task states
#define TASK_STATE_FREE    0
#define TASK_STATE_RUNNING 1
#define TASK_STATE_READY   2
#define TASK_STATE_SLEEP   3  // NEW: sleeping for N ticks

typedef struct {
    uint32_t esp;          // Saved stack pointer (points to register frame)
    uint8_t  kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));
    int      state;        // 0=free, 1=running, 2=ready, 3=sleep
    uint8_t  ring;         // 0 = kernel task, 3 = user task
    // === NEW FIELDS (add-on, safe defaults) ===
    int      priority;     // 0=background, 1=interactive, 2=realtime
    int      sleep_ticks;  // remaining ticks until wake (0 = not sleeping)
    int      wait_ticks;   // consecutive ticks waiting in READY state
    uint32_t page_dir;     // per-process page directory (0 = global identity)
    int      fd_table[16]; // local file descriptors mapped to global FDs
    char     launch_arg[128]; // command-line argument passed at launch
    int      current_dir;  // per-task working directory
    uint32_t heap_ptr;     // current heap break (e.g. 0x08000000)
} task_t;

static task_t tasks[MAX_TASKS];
static int current_task[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static inline int get_cid() { extern uint32_t smp_lapic_addr; return smp_lapic_addr ? (apic_get_id() & 15) : 0; }
static int num_tasks = 0;
static int boot_current_dir = 0;

void init_tasking() {
    int cid = get_cid();
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_STATE_FREE;
        tasks[i].ring = 0;
        tasks[i].priority = PRIORITY_INTERACTIVE;
        tasks[i].sleep_ticks = 0;
        tasks[i].wait_ticks = 0;
        tasks[i].page_dir = 0;
        tasks[i].heap_ptr = 0x08000000;
        tasks[i].launch_arg[0] = '\0';
        tasks[i].current_dir = 0;
        for (int j = 0; j < 16; j++) tasks[i].fd_table[j] = -1;
    }
    tasks[0].state = TASK_STATE_RUNNING;
    tasks[0].ring = 0;
    tasks[0].priority = PRIORITY_INTERACTIVE;
    tasks[0].wait_ticks = 0;
    tasks[0].esp = 0; // Will be filled by scheduler on first preemption
    
    // Save boot CR3 to task 0
    uint32_t boot_cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(boot_cr3));
    tasks[0].page_dir = boot_cr3;
    
    current_task[cid] = 0;
    num_tasks = 1;
    task_set_launch_arg(0, "idle");
}

uint32_t tasks_get_boot_cr3(void) {
    return tasks[0].page_dir;
}

// Create a Ring 0 (kernel) task
int create_task(void (*entry)()) {
    int cid = get_cid();
    // CRITICAL: Disable interrupts BEFORE taking the lock to prevent scheduler deadlocks!
    __asm__ volatile("cli");
    spin_lock(&task_lock);
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == 0) {
            
            tasks[i].ring = 0;
            tasks[i].priority = PRIORITY_INTERACTIVE;
            tasks[i].wait_ticks = 0;
            tasks[i].current_dir = (current_task[cid] >= 0) ? tasks[current_task[cid]].current_dir : 0;
            task_set_launch_arg(i, "sys_kernel");
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
            
            spin_unlock(&task_lock);
            __asm__ volatile("sti");
            return i;
        }
    }
    spin_unlock(&task_lock);
    __asm__ volatile("sti");
    return -1;
}

// Create a Ring 3 (user) task.
//
// UNSUPPORTED — always fails. This used to hand a Ring 3 task the kernel's own
// page directory and a stack inside task_t, which only worked because the
// identity map was user-accessible. Now that it is not, a Ring 3 task needs a
// private address space with its code mapped user-readable and a stack from
// vmm_setup_user_stack(). Use load_mct_app() (which builds exactly that) or
// thread_create() with a page directory from vmm_create_address_space().
//
// It has no callers in the tree; kept so the declaration in task.h stays valid.
int create_user_task(void (*entry)()) {
    (void)entry;
    write_serial_string("[TASK] create_user_task: unsupported, use load_mct_app()\n");
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
        uint32_t active_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(active_cr3));
        if ((active_cr3 & 0xFFFFF000) == (tasks[tid].page_dir & 0xFFFFF000)) {
            extern void vmm_switch_page_dir(uint32_t);
            vmm_switch_page_dir(tasks[0].page_dir);
        }
        extern void vmm_free_address_space(uint32_t);
        vmm_free_address_space(tasks[tid].page_dir);
        tasks[tid].page_dir = 0;
    }
}

uint32_t schedule(uint32_t esp) {
    int cid = get_cid();
    if (current_task[cid] < 0) return esp;
    // NOTE: No spinlock needed here. schedule() is called from the timer IRQ
    // handler, which runs with interrupts disabled (IDT gate 0x8E auto-clears IF).
    // Using a spinlock would cause self-deadlock if any other code path
    // (e.g. create_task) holds task_lock when the timer interrupt fires.
    
    // Auto-poll network on every schedule tick to process ARP/DNS/TCP packets in the background
    extern void net_poll(void);
    net_poll();
    
    // If we're the only task AND we're active, no need to switch
    if (num_tasks <= 1 && tasks[current_task[cid]].state != 0) {
        return esp;
    }
    
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
    tasks[current_task[cid]].esp = esp;
    if (tasks[current_task[cid]].state == TASK_STATE_RUNNING) {
        tasks[current_task[cid]].state = TASK_STATE_READY;
    }
    
    // Increment wait_ticks for all READY tasks to prevent starvation (aging)
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_STATE_READY) {
            tasks[i].wait_ticks++;
        }
    }
    
    // Find next ready task using priority + aging score
    int next = -1;
    int max_score = -1;
    int start = (current_task[cid] + 1) % MAX_TASKS;
    for (int i = 0; i < MAX_TASKS; i++) {
        int idx = (start + i) % MAX_TASKS;
        if (tasks[idx].state == TASK_STATE_READY) {
            int score = (tasks[idx].priority * 10) + tasks[idx].wait_ticks;
            if (score > max_score) {
                max_score = score;
                next = idx;
            }
        }
    }
    
    if (next >= 0) {
        tasks[next].wait_ticks = 0;
    } else {
        // If no other task is ready, keep running current (if it's not free/sleeping)
        if (tasks[current_task[cid]].state == TASK_STATE_READY || tasks[current_task[cid]].state == TASK_STATE_RUNNING) {
            next = current_task[cid];
            tasks[next].wait_ticks = 0;
        } else {
            // Fallback to task 0 (kernel/idle)
            next = 0;
            tasks[next].wait_ticks = 0;
        }
    }
    
    tasks[next].state = 1;
    current_task[cid] = next;
    
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
    int cid = get_cid();
    if (current_task[cid] <= 0) return; // Never kill task 0 (kernel)
    
    write_serial_string("[TASK] task_exit tid=");
    write_serial('0' + current_task[cid]);
    write_serial('\n');
    
    __asm__ volatile("cli");
    task_cleanup(current_task[cid]);
    tasks[current_task[cid]].state = TASK_STATE_FREE;
    num_tasks--;
    
    __asm__ volatile("sti");
    for (;;) __asm__("hlt");
}

int get_current_task(void) {
    int cid = get_cid();
    return current_task[cid];
}

int get_current_dir(void) {
    int cid = get_cid();
    if (current_task[cid] >= 0 && current_task[cid] < MAX_TASKS) {
        return tasks[current_task[cid]].current_dir;
    }
    return boot_current_dir;
}

void set_current_dir(int dir) {
    int cid = get_cid();
    if (current_task[cid] >= 0 && current_task[cid] < MAX_TASKS) {
        tasks[current_task[cid]].current_dir = dir;
    } else {
        boot_current_dir = dir;
    }
}

// === NEW: Thread creation with priority + page_dir ===
int thread_create(void (*entry)(), int priority, uint32_t page_dir) {
    int cid = get_cid();

    // Map the Ring 3 stack into the task's own address space. Done BEFORE
    // taking task_lock so we are not allocating physical frames (which takes
    // vmm_lock) while holding the scheduler lock.
    uint32_t user_esp = vmm_setup_user_stack(page_dir);
    if (user_esp == 0) {
        write_serial_string("[TASK] thread_create: could not map user stack\n");
        return -1;
    }

    __asm__ volatile("cli");
    spin_lock(&task_lock);
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_STATE_FREE) {
            
            tasks[i].ring = 3;  // Threads are user tasks by default
            tasks[i].heap_ptr = 0x08000000;
            tasks[i].current_dir = (current_task[cid] >= 0) ? tasks[current_task[cid]].current_dir : 0;
            tasks[i].priority = priority;
            tasks[i].page_dir = page_dir;
            tasks[i].sleep_ticks = 0;
            tasks[i].wait_ticks = 0;
            for (int j = 0; j < 16; j++) tasks[i].fd_table[j] = -1;
            
            uint32_t* stack = (uint32_t*)&tasks[i].kernel_stack[KERNEL_STACK_SIZE];

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
            // finishing any post-create setup
            spin_unlock(&task_lock);
            return i;
        }
    }
    spin_unlock(&task_lock);
    return -1;
}

// Sleep the current task for N timer ticks
void task_sleep(int ticks) {
    int cid = get_cid();
    if (current_task[cid] < 0 || ticks <= 0) return;
    
    __asm__ volatile("cli");
    tasks[current_task[cid]].sleep_ticks = ticks;
    tasks[current_task[cid]].state = TASK_STATE_SLEEP;
    __asm__ volatile("sti");
    
    // Yield — wait for scheduler to skip us until wake
    __asm__ volatile("sti");
    for (int i = 0; i < 100000; i++) {
        __asm__ volatile("pause");
        if (tasks[current_task[cid]].state != TASK_STATE_SLEEP) break;
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

// Does this task run in the kernel's global address space rather than a private
// one? Beware: task 0 stores the boot CR3, NOT 0, so a bare `page_dir == 0`
// test does not recognise the kernel task. Anything deciding "is this caller
// Ring 3 with its own address space?" must go through here.
int task_in_kernel_space(int tid) {
    uint32_t pd = task_get_page_dir(tid);
    if (pd == 0) return 1;
    return (pd & 0xFFFFF000) == (tasks[0].page_dir & 0xFFFFF000);
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

uint32_t task_get_heap_ptr(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return 0x08000000;
    return tasks[tid].heap_ptr;
}

void task_set_heap_ptr(int tid, uint32_t ptr) {
    if (tid < 0 || tid >= MAX_TASKS) return;
    tasks[tid].heap_ptr = ptr;
}
