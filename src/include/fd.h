#ifndef FD_H
#define FD_H

#include "types.h"
#include "syscall.h"  // stat_t

#define MAX_FDS_PER_TASK 16
#define MAX_GLOBAL_FDS   128

typedef enum {
    FD_TYPE_NONE,
    FD_TYPE_FILE,
    FD_TYPE_PIPE_READ,
    FD_TYPE_PIPE_WRITE,
    FD_TYPE_DEV
} fd_type_t;

typedef struct {
    int in_use;
    fd_type_t type;
    int vfs_node;       // For files and devices
    int pipe_id;        // For pipes
    int offset;         // Read/write offset
    int flags;          // Open flags (O_APPEND)
    int ref_count;
} global_fd_t;

void fd_init();
int do_sys_open(const char* path, int mode);
int do_sys_read(int fd, char* buf, int size);
int do_sys_write(int fd, const char* buf, int size);
int do_sys_close(int fd);
int do_sys_dup2(int oldfd, int newfd);
int do_sys_dup2_tid(int tid, int oldfd, int newfd);
int do_sys_pipe(int pipefd[2]);
// POSIX lseek: reposition the descriptor's read/write offset. whence is
// SEEK_SET(0)/SEEK_CUR(1)/SEEK_END(2); returns the new offset or -1.
int do_sys_lseek(int fd, int offset, int whence);
// POSIX fstat: fill a stat_t for an open file descriptor. Returns 0 or -1.
int do_sys_fstat(int fd, stat_t* out);
// I/O multiplexing (POSIX poll/select). poll() fills fds[i].revents and
// returns the number of ready descriptors (0 on timeout, -1 on error).
// select() rewrites the read/write bitmaps (uint32_t, fd < 32) to the ready
// fds and returns the count. timeout_ms < 0 blocks forever.
int do_sys_poll(pollfd_t* fds, int nfds, int timeout_ms);
int do_sys_select(int nfds, uint32_t* readfds, uint32_t* writefds,
                  uint32_t* exceptfds, int timeout_ms);
void task_close_all_fds(int tid);
void task_rewire_fds(int tid, int in_fd, int out_fd);

#endif
