#include <stdio.h>
#include <stdint.h>
#define MAX_EVENTS 64
typedef struct {
    int type;
    int x, y;
    int key;
} gui_event_t;
typedef struct {
    uint32_t esp;
    uint8_t  kernel_stack[16384] __attribute__((aligned(16)));
    uint8_t  user_stack[8192] __attribute__((aligned(16)));
    int      state;
    uint8_t  ring;
    uint32_t page_dir;
    int      priority;
    int      time_slice;
    int      sleep_ticks;
    int      wait_tid;
    char     launch_arg[128];
    int      fd_table[16];
    struct {
        int head, tail;
        gui_event_t events[MAX_EVENTS];
    } event_queue;
} task_t;
int main() {
    printf("Size: %ld\n", sizeof(task_t));
    return 0;
}
