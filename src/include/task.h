#ifndef TASK_H
#define TASK_H

#include "types.h"

// Priority levels
#define PRIORITY_REALTIME   2
#define PRIORITY_INTERACTIVE 1
#define PRIORITY_BACKGROUND  0

void init_tasking();
int create_task(void (*entry)());
int create_user_task(void (*entry)());
uint32_t schedule(uint32_t esp);
void task_exit(void);
int get_current_task(void);

// === NEW: Thread & Priority API ===
// Create a task with explicit priority and optional page directory
// page_dir = 0 means use global identity map
int thread_create(void (*entry)(), int priority, uint32_t page_dir);

// Sleep the current task for N timer ticks
void task_sleep(int ticks);

// Get/set priority of a task
int task_set_priority(int tid, int priority);
int task_get_priority(int tid);

// Get/set page directory for a task
uint32_t task_get_page_dir(int tid);
void task_set_page_dir(int tid, uint32_t page_dir);

// Wake up a sleeping task (used by timer)
void task_wake(int tid);

// Check if a task is alive
int task_is_alive(int tid);

// Kill a specific task by ID (from kernel, e.g. Ctrl+C)
int task_kill(int tid);

// File Descriptor accessors
int task_get_fd(int tid, int local_fd);
void task_set_fd(int tid, int local_fd, int global_fd);

// === NEW: Task Info for GUI ===
typedef struct {
    int id;
    int state;
    int ring;
    int priority;
    int sleep_ticks;
} task_info_t;

int get_task_info(int tid, task_info_t* info);

#endif
