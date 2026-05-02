#include "../include/fd.h"
#include "../include/task.h"
#include "../include/vfs.h"
#include "../include/mem.h"

global_fd_t global_fds[MAX_GLOBAL_FDS];

// Simple pipe structure
#define MAX_PIPES 16
#define PIPE_BUF_SIZE 4096

typedef struct {
    int in_use;
    char buffer[PIPE_BUF_SIZE];
    int read_pos;
    int write_pos;
    int closed_write;
} pipe_t;

pipe_t pipes[MAX_PIPES];

void fd_init() {
    for (int i = 0; i < MAX_GLOBAL_FDS; i++) global_fds[i].in_use = 0;
    for (int i = 0; i < MAX_PIPES; i++) pipes[i].in_use = 0;
}

// Allocate a global FD slot
static int alloc_global_fd() {
    for (int i = 0; i < MAX_GLOBAL_FDS; i++) {
        if (!global_fds[i].in_use) {
            global_fds[i].in_use = 1;
            global_fds[i].ref_count = 1;
            global_fds[i].offset = 0;
            return i;
        }
    }
    return -1;
}

// Map global FD to task's local FD table
int task_map_fd(int global_fd) {
    int tid = get_current_task();
    if (tid < 0) return -1;
    for (int i = 0; i < MAX_FDS_PER_TASK; i++) {
        if (task_get_fd(tid, i) == -1) {
            task_set_fd(tid, i, global_fd);
            return i;
        }
    }
    return -1;
}

int do_sys_open(const char* path, int mode) {
    (void)mode;
    int node = vfs_get_node(path);
    if (node < 0) return -1;
    
    int gfd = alloc_global_fd();
    if (gfd < 0) return -1;
    
    global_fds[gfd].vfs_node = node;
    global_fds[gfd].type = vfs_is_dir(node) ? FD_TYPE_NONE : FD_TYPE_FILE; // simplistic check
    
    int lfd = task_map_fd(gfd);
    if (lfd < 0) {
        global_fds[gfd].in_use = 0;
        return -1;
    }
    return lfd;
}

int do_sys_read(int fd, char* buf, int size) {
    int tid = get_current_task();
    if (tid < 0) return -1;
    int gfd = task_get_fd(tid, fd);
    if (gfd < 0 || !global_fds[gfd].in_use) return -1;
    
    if (global_fds[gfd].type == FD_TYPE_FILE || global_fds[gfd].type == FD_TYPE_DEV) {
        // vfs_read_file doesn't take offset yet, but ideally it should
        // For simplicity we just use vfs_read_file which reads from start.
        // We need the path.
        char path[256];
        if (vfs_get_abs_path(global_fds[gfd].vfs_node, path, 256) < 0) return -1;
        return vfs_read_file(path, buf, size);
    } else if (global_fds[gfd].type == FD_TYPE_PIPE_READ) {
        int p = global_fds[gfd].pipe_id;
        int read_bytes = 0;
        while (read_bytes < size) {
            if (pipes[p].read_pos != pipes[p].write_pos) {
                buf[read_bytes++] = pipes[p].buffer[pipes[p].read_pos++];
                if (pipes[p].read_pos >= PIPE_BUF_SIZE) pipes[p].read_pos = 0;
            } else {
                if (pipes[p].closed_write) break; // EOF
                // block / yield
                __asm__ volatile("sti; hlt");
            }
        }
        return read_bytes;
    }
    return -1;
}

int do_sys_write(int fd, const char* buf, int size) {
    int tid = get_current_task();
    if (tid < 0) return -1;
    int gfd = task_get_fd(tid, fd);
    if (gfd < 0 || !global_fds[gfd].in_use) return -1;
    
    if (global_fds[gfd].type == FD_TYPE_FILE || global_fds[gfd].type == FD_TYPE_DEV) {
        char path[256];
        if (vfs_get_abs_path(global_fds[gfd].vfs_node, path, 256) < 0) return -1;
        return vfs_write_file(path, buf, size);
    } else if (global_fds[gfd].type == FD_TYPE_PIPE_WRITE) {
        int p = global_fds[gfd].pipe_id;
        int written = 0;
        while (written < size) {
            int next_write = (pipes[p].write_pos + 1) % PIPE_BUF_SIZE;
            if (next_write != pipes[p].read_pos) {
                pipes[p].buffer[pipes[p].write_pos] = buf[written++];
                pipes[p].write_pos = next_write;
            } else {
                // Pipe full, yield
                __asm__ volatile("sti; hlt");
            }
        }
        return written;
    }
    return -1;
}

int do_sys_close(int fd) {
    int tid = get_current_task();
    if (tid < 0) return -1;
    int gfd = task_get_fd(tid, fd);
    if (gfd < 0 || !global_fds[gfd].in_use) return -1;
    
    task_set_fd(tid, fd, -1);
    global_fds[gfd].ref_count--;
    
    if (global_fds[gfd].ref_count == 0) {
        if (global_fds[gfd].type == FD_TYPE_PIPE_WRITE) {
            pipes[global_fds[gfd].pipe_id].closed_write = 1;
        }
        if (global_fds[gfd].type == FD_TYPE_PIPE_READ && pipes[global_fds[gfd].pipe_id].closed_write) {
            pipes[global_fds[gfd].pipe_id].in_use = 0; // free pipe completely
        }
        global_fds[gfd].in_use = 0;
    }
    return 0;
}

int do_sys_pipe(int pipefd[2]) {
    int p = -1;
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].in_use) {
            pipes[i].in_use = 1;
            pipes[i].read_pos = 0;
            pipes[i].write_pos = 0;
            pipes[i].closed_write = 0;
            p = i;
            break;
        }
    }
    if (p < 0) return -1;
    
    int g_read = alloc_global_fd();
    if (g_read < 0) { pipes[p].in_use = 0; return -1; }
    global_fds[g_read].type = FD_TYPE_PIPE_READ;
    global_fds[g_read].pipe_id = p;
    
    int g_write = alloc_global_fd();
    if (g_write < 0) { global_fds[g_read].in_use = 0; pipes[p].in_use = 0; return -1; }
    global_fds[g_write].type = FD_TYPE_PIPE_WRITE;
    global_fds[g_write].pipe_id = p;
    
    pipefd[0] = task_map_fd(g_read);
    pipefd[1] = task_map_fd(g_write);
    
    if (pipefd[0] < 0 || pipefd[1] < 0) {
        // cleanup on error
        do_sys_close(pipefd[0]);
        do_sys_close(pipefd[1]);
        return -1;
    }
    return 0;
}
