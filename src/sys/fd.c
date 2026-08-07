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
        // vfs_write_file() always writes from the START of the file, so a
        // second write(1, ...) from an app would clobber the first. Track an
        // offset per descriptor and append: read the existing content, splice
        // the new bytes at the descriptor's offset, write back the whole file.
        int off = global_fds[gfd].offset;
        int oldsz = 0;
        char whole[4096];
        int r = vfs_read_file(path, whole, 4095);
        if (r < 0) r = 0;
        oldsz = r;
        if (off + size > 4095) size = 4095 - off;
        if (size <= 0) return 0;
        for (int i = 0; i < size; i++) whole[off + i] = buf[i];
        int newsz = (off + size > oldsz) ? off + size : oldsz;
        whole[newsz] = '\0';
        int w = vfs_write_file(path, whole, newsz);
        if (w >= 0) global_fds[gfd].offset = off + size;
        return (w >= 0) ? size : w;
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

// Release one global FD: drop its refcount and free the slot (and any pipe it
// owns) when the last reference goes away. Used by do_sys_close and by task
// teardown (task_close_all_fds).
static void fd_release_global(int gfd) {
    global_fds[gfd].ref_count--;

    if (global_fds[gfd].ref_count == 0) {
        int p = global_fds[gfd].pipe_id;
        if (global_fds[gfd].type == FD_TYPE_PIPE_WRITE) {
            pipes[p].closed_write = 1;
            // If no read end remains open (the read side was closed first),
            // free the pipe NOW — the old code only freed it when the read end
            // closed after the write end, leaking the slot forever.
            int read_open = 0;
            for (int i = 0; i < MAX_GLOBAL_FDS; i++) {
                if (global_fds[i].in_use && global_fds[i].type == FD_TYPE_PIPE_READ &&
                    global_fds[i].pipe_id == p) { read_open = 1; break; }
            }
            if (!read_open) pipes[p].in_use = 0;
        } else if (global_fds[gfd].type == FD_TYPE_PIPE_READ && pipes[p].closed_write) {
            pipes[p].in_use = 0; // free pipe completely
        }
        global_fds[gfd].in_use = 0;
    }
}

int do_sys_close(int fd) {
    int tid = get_current_task();
    if (tid < 0) return -1;
    int gfd = task_get_fd(tid, fd);
    if (gfd < 0 || !global_fds[gfd].in_use) return -1;

    task_set_fd(tid, fd, -1);
    fd_release_global(gfd);
    return 0;
}

// POSIX dup2(oldfd, newfd) on an arbitrary task's fd table: make `newfd` a
// copy of `oldfd`, closing `newfd` first if it is already open (and freeing
// the pipe slot if that was its last reference). `tid` may be the calling
// task (do_sys_dup2) or a not-yet-running child (task_fork_exec). Returns
// the new fd number, or -1 on error.
int do_sys_dup2_tid(int tid, int oldfd, int newfd) {
    if (tid < 0) return -1;
    if (oldfd < 0 || oldfd >= MAX_FDS_PER_TASK) return -1;
    if (newfd < 0 || newfd >= MAX_FDS_PER_TASK) return -1;
    if (oldfd == newfd) return newfd;

    int ogfd = task_get_fd(tid, oldfd);
    if (ogfd < 0 || !global_fds[ogfd].in_use) return -1;

    // Close the target slot first (POSIX semantics).
    int ngfd = task_get_fd(tid, newfd);
    if (ngfd >= 0 && global_fds[ngfd].in_use) {
        task_set_fd(tid, newfd, -1);
        fd_release_global(ngfd);
    }

    // Point newfd at oldfd's global descriptor and bump its refcount so a
    // close of the original does not yank the copy away.
    global_fds[ogfd].ref_count++;
    task_set_fd(tid, newfd, ogfd);
    return newfd;
}

int do_sys_dup2(int oldfd, int newfd) {
    int tid = get_current_task();
    if (tid < 0) return -1;
    return do_sys_dup2_tid(tid, oldfd, newfd);
}

// POSIX spawn fd wiring for a child that has not run yet: dup `in_fd` onto
// its fd 0 and `out_fd` onto its fd 1 (each only when >= 0), then drop every
// OTHER descriptor — including the inherited originals of in_fd/out_fd and
// fd 2+ — so the exec'd image sees exactly its wired stdin/stdout and nothing
// else. CRITICAL: fd 0/1 are only kept when they were actually wired; a child
// that inherits the shell's pipe fds as local fd 0/1 (do_sys_pipe allocates
// the lowest free slots) must have the unwanted end CLOSED. In a pipeline
// (`a | b`) each child inherits BOTH pipe ends, and a writer that keeps a
// stray copy of the read end — or a reader keeping the write end — makes the
// peer's EOF (closed_write) never fire, hanging the reader forever.
void task_rewire_fds(int tid, int in_fd, int out_fd) {
    if (tid < 0) return;
    if (in_fd >= 0 && in_fd != 0) do_sys_dup2_tid(tid, in_fd, 0);
    if (out_fd >= 0 && out_fd != 1) do_sys_dup2_tid(tid, out_fd, 1);
    for (int j = 0; j < MAX_FDS_PER_TASK; j++) {
        if (j == 0 && in_fd >= 0) continue;    // wired stdin stays
        if (j == 1 && out_fd >= 0) continue;   // wired stdout stays
        int gfd = task_get_fd(tid, j);
        if (gfd >= 0) {
            task_set_fd(tid, j, -1);
            if (gfd < MAX_GLOBAL_FDS && global_fds[gfd].in_use) fd_release_global(gfd);
        }
    }
}

// Close every fd a task holds. Called from task_cleanup() (exit/kill) — without
// this, the task's global fd slots and pipes leaked forever, and a pipe reader
// whose writer task died blocked on a pipe that was never closed.
void task_close_all_fds(int tid) {
    for (int i = 0; i < MAX_FDS_PER_TASK; i++) {
        int gfd = task_get_fd(tid, i);
        if (gfd < 0) continue;
        task_set_fd(tid, i, -1);
        if (gfd < MAX_GLOBAL_FDS && global_fds[gfd].in_use) fd_release_global(gfd);
    }
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
        // Cleanup on error. do_sys_close() with a negative lfd is a silent no-op,
        // so the global fds and pipe slot must be released explicitly for the
        // fd(s) whose local mapping failed.
        if (pipefd[0] >= 0) do_sys_close(pipefd[0]);
        else global_fds[g_read].in_use = 0;
        if (pipefd[1] >= 0) do_sys_close(pipefd[1]);
        else global_fds[g_write].in_use = 0;
        pipes[p].in_use = 0;
        return -1;
    }
    return 0;
}
