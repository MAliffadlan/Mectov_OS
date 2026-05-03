#ifndef IPC_H
#define IPC_H

#include "types.h"

// Inter-Process Communication — Message Queues
// Simple synchronous message-passing between tasks.
//
// Each message queue is identified by a 32-bit key.
// A task can create/open a queue, send to it, and receive from it.
// Messages are fixed-size (32 bytes payload).
// If no receiver is waiting, send blocks until a receiver arrives.
// If no message is available, receive blocks until a sender arrives.

#define IPC_MAX_QUEUES   16
#define IPC_MSG_SIZE     128   // bytes of payload (increased for stdout lines)
#define IPC_QUEUE_DEPTH  256   // max pending messages (increased for terminal output)

typedef struct {
    uint32_t sender_tid;               // who sent this
    uint32_t type;                     // message type (app-defined)
    uint8_t  data[IPC_MSG_SIZE];       // payload
    uint32_t data_len;                 // actual length of data (0–32)
} ipc_message_t;

typedef struct {
    uint32_t         key;              // queue identifier (0 = free slot)
    ipc_message_t    messages[IPC_QUEUE_DEPTH];
    int              head, tail, count;
    int              waiting_send;     // TID of blocked sender, or -1
    int              waiting_recv;     // TID of blocked receiver, or -1
} ipc_queue_t;

// Initialize IPC subsystem
void ipc_init(void);

// Create/open a queue by key. Returns queue index (1..IPC_MAX_QUEUES) or 0 on fail.
int ipc_queue_create(uint32_t key);

// Destroy a queue
void ipc_queue_destroy(int qid);

// Send a message. Blocks if queue is full.
// Returns 0 on success, -1 on error.
int ipc_send(int qid, uint32_t type, const void* data, uint32_t len);

// Receive a message. Blocks if queue is empty.
// Returns 0 on success, -1 on error.
int ipc_receive(int qid, uint32_t* sender_tid, uint32_t* type, void* data, uint32_t* len);

// Non-blocking versions (returns -1 if would block)
int ipc_try_send(int qid, uint32_t type, const void* data, uint32_t len);
int ipc_try_receive(int qid, uint32_t* sender_tid, uint32_t* type, void* data, uint32_t* len);

#endif