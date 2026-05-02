#ifndef FD_H
#define FD_H

#include "types.h"

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
    int ref_count;
} global_fd_t;

void fd_init();
int do_sys_open(const char* path, int mode);
int do_sys_read(int fd, char* buf, int size);
int do_sys_write(int fd, const char* buf, int size);
int do_sys_close(int fd);
int do_sys_pipe(int pipefd[2]);

#endif
