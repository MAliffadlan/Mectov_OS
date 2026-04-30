#include "../include/ipc.h"
#include "../include/task.h"
#include "../include/serial.h"
#include "../include/utils.h"

static ipc_queue_t queues[IPC_MAX_QUEUES];
static int ipc_initialized = 0;

void ipc_init(void) {
    if (ipc_initialized) return;
    ipc_initialized = 1;
    memset(queues, 0, sizeof(queues));
    write_serial_string("[IPC] Initialized\n");
}

int ipc_queue_create(uint32_t key) {
    if (key == 0) return 0;
    
    // Check if queue already exists with this key
    for (int i = 0; i < IPC_MAX_QUEUES; i++) {
        if (queues[i].key == key) {
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
            return i + 1;
        }
    }
    write_serial_string("[IPC] No free queue slots!\n");
    return 0;
}

void ipc_queue_destroy(int qid) {
    if (qid < 1 || qid > IPC_MAX_QUEUES) return;
    int idx = qid - 1;
    queues[idx].key = 0;
    queues[idx].count = 0;
    queues[idx].waiting_send = -1;
    queues[idx].waiting_recv = -1;
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
    int idx = qid - 1;
    if (queues[idx].key == 0) return -1;
    
    if (len > IPC_MSG_SIZE) len = IPC_MSG_SIZE;
    
    ipc_message_t msg;
    msg.sender_tid = get_current_task();
    msg.type = type;
    msg.data_len = len;
    memset(msg.data, 0, IPC_MSG_SIZE);
    if (data && len > 0) memcpy(msg.data, data, len);
    
    // Try push
    while (queue_push(idx, &msg) != 0) {
        // Queue full — block until someone receives
        queues[idx].waiting_send = get_current_task();
        // This task will be re-awakened by the receiver
        // For now, we busy-wait (simple approach)
        // In a real implementation, we'd yield/schedule
        __asm__ volatile("sti");
        __asm__ volatile("pause");
    }
    
    // If someone is waiting to receive, wake them up
    if (queues[idx].waiting_recv >= 0) {
        task_wake(queues[idx].waiting_recv);
        queues[idx].waiting_recv = -1;
    }
    
    return 0;
}

int ipc_receive(int qid, uint32_t* sender_tid, uint32_t* type, void* data, uint32_t* len) {
    if (qid < 1 || qid > IPC_MAX_QUEUES) return -1;
    int idx = qid - 1;
    if (queues[idx].key == 0) return -1;
    
    ipc_message_t msg;
    
    // Try pop
    while (queue_pop(idx, &msg) != 0) {
        // Queue empty — block until someone sends
        queues[idx].waiting_recv = get_current_task();
        // Busy-wait
        __asm__ volatile("sti");
        __asm__ volatile("pause");
    }
    
    // If someone is waiting to send, wake them up
    if (queues[idx].waiting_send >= 0) {
        task_wake(queues[idx].waiting_send);
        queues[idx].waiting_send = -1;
    }
    
    if (sender_tid) *sender_tid = msg.sender_tid;
    if (type) *type = msg.type;
    if (len) *len = msg.data_len;
    if (data && msg.data_len > 0) memcpy(data, msg.data, msg.data_len);
    
    return 0;
}

int ipc_try_send(int qid, uint32_t type, const void* data, uint32_t len) {
    if (qid < 1 || qid > IPC_MAX_QUEUES) return -1;
    int idx = qid - 1;
    if (queues[idx].key == 0) return -1;
    
    if (len > IPC_MSG_SIZE) len = IPC_MSG_SIZE;
    
    ipc_message_t msg;
    msg.sender_tid = get_current_task();
    msg.type = type;
    msg.data_len = len;
    memset(msg.data, 0, IPC_MSG_SIZE);
    if (data && len > 0) memcpy(msg.data, data, len);
    
    if (queue_push(idx, &msg) != 0) return -1;
    
    if (queues[idx].waiting_recv >= 0) {
        task_wake(queues[idx].waiting_recv);
        queues[idx].waiting_recv = -1;
    }
    
    return 0;
}

int ipc_try_receive(int qid, uint32_t* sender_tid, uint32_t* type, void* data, uint32_t* len) {
    if (qid < 1 || qid > IPC_MAX_QUEUES) return -1;
    int idx = qid - 1;
    if (queues[idx].key == 0) return -1;
    
    ipc_message_t msg;
    if (queue_pop(idx, &msg) != 0) return -1;
    
    if (queues[idx].waiting_send >= 0) {
        task_wake(queues[idx].waiting_send);
        queues[idx].waiting_send = -1;
    }
    
    if (sender_tid) *sender_tid = msg.sender_tid;
    if (type) *type = msg.type;
    if (len) *len = msg.data_len;
    if (data && msg.data_len > 0) memcpy(data, msg.data, msg.data_len);
    
    return 0;
}