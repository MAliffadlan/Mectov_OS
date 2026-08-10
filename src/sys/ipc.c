#include "../include/ipc.h"
#include "../include/task.h"
#include "../include/serial.h"
#include "../include/utils.h"
#include "../include/spinlock.h"

// ipc_lock protects the queue table from concurrent syscalls on different
// cores (kernel locking audit v38.4). Blocking send/recv drop the lock while
// parked in task_sleep() — a killed peer must not strand the lock — and
// task_wake() is called only AFTER releasing (task_wake takes task_lock
// internally; task_lock must stay the top of the global ordering).
static spinlock_t ipc_lock = SPINLOCK_INIT;
static uint32_t ipc_eflags;
static void ipc_lock_acquire(void) { ipc_eflags = spin_lock_irqsave(&ipc_lock); }
static void ipc_lock_release(void) { spin_unlock_irqrestore(&ipc_lock, ipc_eflags); }

static ipc_queue_t queues[IPC_MAX_QUEUES];
static int ipc_initialized = 0;

void ipc_init(void) {
    if (ipc_initialized) return;
    ipc_initialized = 1;
    ipc_lock_acquire();
    memset(queues, 0, sizeof(queues));
    ipc_lock_release();
    write_serial_string("[IPC] Initialized\n");
    write_serial_string("[IPC] IPC_MSG_SIZE: ");
    write_serial_hex(IPC_MSG_SIZE);
    write_serial_string("\n[IPC] sizeof(ipc_message_t): ");
    write_serial_hex(sizeof(ipc_message_t));
    write_serial_string("\n");
}

int ipc_queue_create(uint32_t key) {
    if (key == 0) return 0;

    ipc_lock_acquire();
    // Check if queue already exists with this key
    for (int i = 0; i < IPC_MAX_QUEUES; i++) {
        if (queues[i].key == key) {
            ipc_lock_release();
            return i + 1;  // Return 1-based index
        }
    }
    
    // Find free slot
    for (int i = 0; i < IPC_MAX_QUEUES; i++) {
        if (queues[i].key == 0) {
            queues[i].key = key;
            queues[i].head = 0;
            queues[i].tail = 0;
            queues[i].count = 0;
            queues[i].waiting_send = -1;
            queues[i].waiting_recv = -1;
            write_serial_string("[IPC] Created queue key=");
            write_serial_hex(key);
            write_serial_string(" qid=");
            write_serial('0' + i + 1);
            write_serial('\n');
            ipc_lock_release();
            return i + 1;
        }
    }
    write_serial_string("[IPC] No free queue slots!\n");
    ipc_lock_release();
    return 0;
}

void ipc_queue_destroy(int qid) {
    if (qid < 1 || qid > IPC_MAX_QUEUES) return;
    ipc_lock_acquire();
    int idx = qid - 1;
    queues[idx].key = 0;
    queues[idx].count = 0;
    queues[idx].waiting_send = -1;
    queues[idx].waiting_recv = -1;
    ipc_lock_release();
}

// Push a message to queue (internal, no blocking)
static int queue_push(int idx, const ipc_message_t* msg) {
    if (queues[idx].count >= IPC_QUEUE_DEPTH) return -1;
    queues[idx].messages[queues[idx].head] = *msg;
    queues[idx].head = (queues[idx].head + 1) % IPC_QUEUE_DEPTH;
    queues[idx].count++;
    return 0;
}

// Pop a message from queue (internal, no blocking)
static int queue_pop(int idx, ipc_message_t* msg) {
    if (queues[idx].count <= 0) return -1;
    *msg = queues[idx].messages[queues[idx].tail];
    queues[idx].tail = (queues[idx].tail + 1) % IPC_QUEUE_DEPTH;
    queues[idx].count--;
    return 0;
}

int ipc_send(int qid, uint32_t type, const void* data, uint32_t len) {
    if (qid < 1 || qid > IPC_MAX_QUEUES) return -1;

    ipc_lock_acquire();
    int idx = qid - 1;
    if (queues[idx].key == 0) { ipc_lock_release(); return -1; }
    
    if (len > IPC_MSG_SIZE) len = IPC_MSG_SIZE;
    
    ipc_message_t msg;
    msg.sender_tid = get_current_task();
    msg.type = type;
    msg.data_len = len;
    memset(msg.data, 0, IPC_MSG_SIZE);
    if (data && len > 0) memcpy(msg.data, data, len);
    
    // Try push
    while (queue_push(idx, &msg) != 0) {
        // Queue full — block until someone receives. Put the task to sleep for a
        // tick instead of spinning: a spinning task never leaves the BSP CPU and
        // livelocks the machine if its peer never drains the queue. Drop the
        // lock while parked so a killed peer cannot strand it.
        queues[idx].waiting_send = get_current_task();
        ipc_lock_release();
        task_sleep(1);
        ipc_lock_acquire();
        // Peer may have destroyed the queue while we slept — bail out.
        if (queues[idx].key == 0) { ipc_lock_release(); return -1; }
    }
    
    // If someone is waiting to receive, wake them up — AFTER releasing the
    // lock (task_wake takes task_lock internally).
    int wake = queues[idx].waiting_recv;
    if (wake >= 0) queues[idx].waiting_recv = -1;
    ipc_lock_release();
    if (wake >= 0) task_wake(wake);
    
    return 0;
}

int ipc_receive(int qid, uint32_t* sender_tid, uint32_t* type, void* data, uint32_t* len) {
    if (qid < 1 || qid > IPC_MAX_QUEUES) return -1;

    ipc_lock_acquire();
    int idx = qid - 1;
    if (queues[idx].key == 0) { ipc_lock_release(); return -1; }
    
    ipc_message_t msg;
    
    // Try pop
    while (queue_pop(idx, &msg) != 0) {
        // Queue empty — block until someone sends. Same sleep-per-tick approach
        // as ipc_send: never spin on an empty queue. Drop the lock while parked.
        queues[idx].waiting_recv = get_current_task();
        ipc_lock_release();
        task_sleep(1);
        ipc_lock_acquire();
        if (queues[idx].key == 0) { ipc_lock_release(); return -1; }
    }
    
    // If someone is waiting to send, wake them up — AFTER releasing the lock
    // (task_wake takes task_lock internally).
    int wake = queues[idx].waiting_send;
    if (wake >= 0) queues[idx].waiting_send = -1;
    ipc_lock_release();
    if (wake >= 0) task_wake(wake);
    
    if (sender_tid) *sender_tid = msg.sender_tid;
    if (type) *type = msg.type;
    if (len) *len = msg.data_len;
    if (data && msg.data_len > 0) memcpy(data, msg.data, msg.data_len);
    
    return 0;
}

int ipc_try_send(int qid, uint32_t type, const void* data, uint32_t len) {
    if (qid < 1 || qid > IPC_MAX_QUEUES) return -1;

    ipc_lock_acquire();
    int idx = qid - 1;
    if (queues[idx].key == 0) { ipc_lock_release(); return -1; }
    
    if (len > IPC_MSG_SIZE) len = IPC_MSG_SIZE;
    
    ipc_message_t msg;
    msg.sender_tid = get_current_task();
    msg.type = type;
    msg.data_len = len;
    memset(msg.data, 0, IPC_MSG_SIZE);
    if (data && len > 0) memcpy(msg.data, data, len);
    
    if (queue_push(idx, &msg) != 0) { ipc_lock_release(); return -1; }
    
    int wake = queues[idx].waiting_recv;
    if (wake >= 0) queues[idx].waiting_recv = -1;
    ipc_lock_release();
    if (wake >= 0) task_wake(wake);
    
    return 0;
}

int ipc_try_receive(int qid, uint32_t* sender_tid, uint32_t* type, void* data, uint32_t* len) {
    if (qid < 1 || qid > IPC_MAX_QUEUES) return -1;

    ipc_lock_acquire();
    int idx = qid - 1;
    if (queues[idx].key == 0) { ipc_lock_release(); return -1; }
    
    ipc_message_t msg;
    if (queue_pop(idx, &msg) != 0) { ipc_lock_release(); return -1; }
    
    int wake = queues[idx].waiting_send;
    if (wake >= 0) queues[idx].waiting_send = -1;
    ipc_lock_release();
    if (wake >= 0) task_wake(wake);
    
    if (sender_tid) *sender_tid = msg.sender_tid;
    if (type) *type = msg.type;
    if (len) *len = msg.data_len;
    if (data && msg.data_len > 0) memcpy(data, msg.data, msg.data_len);
    
    return 0;
}