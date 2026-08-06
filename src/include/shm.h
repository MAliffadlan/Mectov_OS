#ifndef SHM_H
#define SHM_H

#include "types.h"

// Shared Memory — POSIX-style System V shm for this OS.
//
// shmget(key, size) creates (or finds) a segment backed by physical frames.
// shmat(id) maps those frames into the CALLING task's address space at a
// fixed VA region and returns the base pointer; two tasks that shmat the
// same segment see the same physical memory (shared memory in the true
// sense — unlike fork's COW pages). shmdt(addr) unmaps it, shmctl(id, 0)
// marks the segment for destruction (frames are freed once every attached
// task has detached or exited).
//
// Frame ownership integrates with the existing refcount machinery in vmm.c:
//   - shmget  allocates frames  (refcount 1, held by the segment)
//   - shmat   maps + bumps each frame's refcount (held by the task's AS)
//   - shmdt   unmaps + drops the refcount
//   - task exit drops refcounts via vmm_free_address_space
//   - shmctl  drops the segment's own references; frames go free at 0
// So a fork()'d child inherits shm mappings correctly and the pages stay
// shared (vmm_clone_address_space already bumps refcounts per mapping).

#define SHM_MAX_SEGMENTS 16
#define SHM_MAX_PAGES    256         // 1 MB max per segment
#define SHM_BASE         0x0A000000u // user VA region for shm (above lib base)
#define SHM_REGION       0x00100000u // 1 MB VA per segment
#define SHM_IPC_RMID     0           // shmctl command: destroy segment

// Per-task attachment bitmap: bit (shmid-1) set while the task has the
// segment mapped. The task layer consults it on exit so segments are released
// even when a task dies without shmdt().
#define SHM_ATTACH_BITS  16

// Create/find a segment by key. size is rounded up to whole pages.
// Returns 1-based shm id, or -1 on error.
int shm_get(uint32_t key, uint32_t size);

// Map segment into the current task's address space. Returns the base VA
// (SHM_BASE + id*SHM_REGION) or 0 on failure.
uint32_t shm_at(int shmid);

// Unmap a previously shmat'ed region from the current task.
int shm_dt(uint32_t addr);

// Control a segment. cmd == SHM_IPC_RMID destroys it (frames freed when the
// last attachment is gone). Returns 0 on success, -1 on error.
int shm_ctl(int shmid, int cmd);

// Called by the task layer when a task exits: release every segment this task
// still had attached (bitmap lookup). Frees segments marked destroyed once the
// last attachment drops.
void shm_task_exit(int tid);

#endif
