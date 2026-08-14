#ifndef TYPES_H
#define TYPES_H

typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef unsigned long long uint64_t;

typedef char  int8_t;
typedef short int16_t;
typedef int   int32_t;
typedef long long int64_t;

typedef unsigned int uintptr_t;

#define NULL ((void*)0)

// Resource limits (POSIX RLIMIT_* subset, v38.28). Shared by the kernel
// (task.h: task_t.rlimits[]) and the Ring 3 syscall ABI (syscall.h: SYS_GET/
// SETRLIMIT), so the type and resource numbers live here — the common base
// both headers include. RLIMIT_NPROC is enforced at fork/clone/thread-create
// (counts live tasks with the caller's uid, like Linux), RLIMIT_NOFILE at fd
// allocation (open/pipe/dup2), RLIMIT_AS at heap/mmap growth. Each limit has
// a soft (cur, enforced) and hard (max, ceiling) value; a non-root caller
// may lower cur and raise cur only up to max, never raise max (POSIX).
#define RLIMIT_NPROC   0
#define RLIMIT_AS      1
#define RLIMIT_NOFILE  2
#define RLIM_NLIMITS   3
typedef struct {
    uint32_t cur;   // soft limit (enforced by the kernel)
    uint32_t max;   // hard limit (ceiling for raising cur; root may raise max)
} rlimit_t;

#endif
