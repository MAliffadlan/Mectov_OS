#include "../include/fd.h"
#include "../include/task.h"
#include "../include/vfs.h"
#include "../include/mem.h"
#include "../include/spinlock.h"
#include "../include/timer.h"
#include "../include/net.h"

// fd_lock protects the shared descriptor tables (global_fds[], pipes[]) from
// concurrent syscalls on different cores. Process context (syscall, main
// loop, fork under task_lock) acquires with irqsave; the pipe block/yield
// points release the lock before `sti; hlt` so a waiting reader/writer never
// holds the table while parked (a killed peer could otherwise strand the
// lock). Ordering: task_lock > fd_lock > vfs_lock > ata_lock.
static spinlock_t fd_lock = SPINLOCK_INIT;
static uint32_t fd_eflags;
static void fd_lock_acquire(void) { fd_eflags = spin_lock_irqsave(&fd_lock); }
static void fd_lock_release(void) { spin_unlock_irqrestore(&fd_lock, fd_eflags); }

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
    // RLIMIT_NOFILE (v38.28): refuse a new local fd when the caller's soft
    // limit is reached. The caller (do_sys_open/do_sys_pipe) releases the
    // already-allocated global fd on the -1 return, so nothing leaks.
    if (!task_rlimit_nofile_ok()) return -1;
    for (int i = 0; i < MAX_FDS_PER_TASK; i++) {
        if (task_get_fd(tid, i) == -1) {
            task_set_fd(tid, i, global_fd);
            return i;
        }
    }
    return -1;
}

int do_sys_open(const char* path, int mode) {
    int node = vfs_get_node(path);
    if (node < 0) return -1;

    fd_lock_acquire();
    int gfd = alloc_global_fd();
    if (gfd < 0) { fd_lock_release(); return -1; }
    
    global_fds[gfd].vfs_node = node;
    global_fds[gfd].type = vfs_is_dir(node) ? FD_TYPE_NONE : FD_TYPE_FILE; // simplistic check
    global_fds[gfd].flags = mode;   // O_APPEND etc.
    
    int lfd = task_map_fd(gfd);
    if (lfd < 0) {
        global_fds[gfd].in_use = 0;
        fd_lock_release();
        return -1;
    }
    fd_lock_release();
    return lfd;
}

int do_sys_read(int fd, char* buf, int size) {
    int tid = get_current_task();
    if (tid < 0) return -1;

    fd_lock_acquire();
    int gfd = task_get_fd(tid, fd);
    if (gfd < 0 || !global_fds[gfd].in_use) { fd_lock_release(); return -1; }
    
    if (global_fds[gfd].type == FD_TYPE_FILE || global_fds[gfd].type == FD_TYPE_DEV) {
        // Permission check (v38.23): reading a regular file requires read
        // access on its node. Per-call (not just at open) so a chmod while
        // the descriptor is open takes effect. Runs under fd_lock.
        if (global_fds[gfd].type == FD_TYPE_FILE &&
            !vfs_check_perm(global_fds[gfd].vfs_node, S_IRUSR)) {
            fd_lock_release();
            return -1;
        }
        int node = global_fds[gfd].vfs_node;
        int t = fs_nodes[node].type;
        if (t == FS_FILE || t == FS_EXT2_FILE || t == FS_FAT32_FILE) {
            // Offset-aware read at the descriptor's position, then advance it
            // (POSIX sequential read). vfs_read_file_offset dispatches per
            // backend (native / ext2 / FAT32 range reads, v38.53) and takes
            // only ata_lock — safe under fd_lock, matching the existing
            // pattern.
            int off = global_fds[gfd].offset;
            int r = vfs_read_file_offset(node, off, buf, size);
            if (r >= 0) global_fds[gfd].offset = off + r;
            fd_lock_release();
            return r;
        }
        // Non-FS_FILE nodes (dev/proc): legacy whole read from the start,
        // no offset semantics.
        char path[256];
        if (vfs_get_abs_path(node, path, 256) < 0) { fd_lock_release(); return -1; }
        int r = vfs_read_file(path, buf, size);
        fd_lock_release();
        return r;
    } else if (global_fds[gfd].type == FD_TYPE_PIPE_READ) {
        int p = global_fds[gfd].pipe_id;
        int read_bytes = 0;
        // The lock stays held through the loop; only the block/yield point
        // drops it (so a parked reader never strands the table) and re-acquires
        // afterwards, re-fetching the descriptor in case it was closed.
        while (read_bytes < size) {
            if (pipes[p].read_pos != pipes[p].write_pos) {
                buf[read_bytes++] = pipes[p].buffer[pipes[p].read_pos++];
                if (pipes[p].read_pos >= PIPE_BUF_SIZE) pipes[p].read_pos = 0;
            } else {
                if (pipes[p].closed_write) break; // EOF
                fd_lock_release();
                __asm__ volatile("sti; hlt");
                fd_lock_acquire();
                gfd = task_get_fd(tid, fd);
                if (gfd < 0 || !global_fds[gfd].in_use) { fd_lock_release(); return read_bytes; }
                p = global_fds[gfd].pipe_id;
            }
        }
        fd_lock_release();
        return read_bytes;
    } else if (global_fds[gfd].type == FD_TYPE_SOCKET) {
        // Sockets (v38.43). TCP: poll-style recv (0 = nothing yet, -1 = EOF).
        // UDP: the single global binding. Plain read() works on both so
        // shell-style apps can treat a socket like any fd.
        fd_lock_release();   // net layer is self-synchronised (cli/sti poll)
        if (global_fds[gfd].flags == 2 /* SOCK_DGRAM */) {
            return net_udp_recv((uint8_t*)buf, (uint32_t)size);
        }
        int r = net_tcp_recv(global_fds[gfd].sock_conn, (uint8_t*)buf, (uint32_t)size);
        return (r == -2) ? -1 : r;
    }
    fd_lock_release();
    return -1;
}

int do_sys_write(int fd, const char* buf, int size) {
    int tid = get_current_task();
    if (tid < 0) return -1;

    fd_lock_acquire();
    int gfd = task_get_fd(tid, fd);
    if (gfd < 0 || !global_fds[gfd].in_use) { fd_lock_release(); return -1; }
    
    if (global_fds[gfd].type == FD_TYPE_FILE || global_fds[gfd].type == FD_TYPE_DEV) {
        // Permission check (v38.23): writing a regular file requires write
        // access on its node. Per-call (not just at open) because a file can
        // be chmod'ed while a descriptor stays open. Runs under fd_lock, so
        // the node lookup is race-free. Root bypasses; /dev and /proc nodes
        // always pass.
        if (global_fds[gfd].type == FD_TYPE_FILE &&
            !vfs_check_perm(global_fds[gfd].vfs_node, S_IWUSR)) {
            fd_lock_release();
            return -1;
        }
        char path[256];
        if (vfs_get_abs_path(global_fds[gfd].vfs_node, path, 256) < 0) { fd_lock_release(); return -1; }
        // Offset-aware write (v38.53 fix). The old path spliced into a fixed
        // char whole[4096] and rewrote the file from it — every write to a
        // file larger than 4095 bytes silently DESTROYED everything past the
        // first 4KB (data loss on all backends). Now the existing content is
        // read into a right-sized dynamic buffer, the new bytes are spliced
        // at the descriptor offset, and the whole file is written back.
        // Backends only expose whole-file writes today, so this stays a
        // read-modify-write — correctness first; an in-place backend range
        // write is future optimization. VFS_FD_MAX_FILE bounds the buffer so
        // a huge ext2 file can't OOM the 24MB kernel heap.
        int off = global_fds[gfd].offset;
        int oldsz = fs_nodes[global_fds[gfd].vfs_node].size;
        if (oldsz < 0) oldsz = 0;
        // O_APPEND: every write lands at the end of the file, regardless of
        // the descriptor offset (POSIX).
        if (global_fds[gfd].flags & O_APPEND) off = oldsz;
        int newsz = (off + size > oldsz) ? off + size : oldsz;
        if (newsz > VFS_FD_MAX_FILE) {
            // Partial write up to the cap (POSIX-style short write), never
            // truncation of what already exists.
            newsz = VFS_FD_MAX_FILE;
            size = newsz - off;
            if (size <= 0) { fd_lock_release(); return -1; }   // offset beyond cap: EFBIG-ish
        }
        char* whole = (char*)kmalloc((uint32_t)newsz + 1);
        if (!whole) { fd_lock_release(); return -1; }
        if (oldsz > 0) {
            int r = vfs_read_file(path, whole, oldsz);
            if (r < 0) r = 0;
            if (r < oldsz) {
                // Sparse tail reads as zeros (short backend read)
                int z = oldsz - r;
                for (int i = 0; i < z; i++) whole[r + i] = 0;
            }
        }
        for (int i = 0; i < size; i++) whole[off + i] = buf[i];
        whole[newsz] = '\0';
        int w = vfs_write_file(path, whole, newsz);
        kfree(whole);
        if (w >= 0) global_fds[gfd].offset = off + size;
        fd_lock_release();
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
                // Pipe full: drop the lock, yield, re-acquire (writer may run
                // on another core; re-fetch the fd in case it was closed).
                fd_lock_release();
                __asm__ volatile("sti; hlt");
                fd_lock_acquire();
                gfd = task_get_fd(tid, fd);
                if (gfd < 0 || !global_fds[gfd].in_use) { fd_lock_release(); return written; }
                p = global_fds[gfd].pipe_id;
            }
        }
        fd_lock_release();
        return written;
    } else if (global_fds[gfd].type == FD_TYPE_SOCKET) {
        // Sockets (v38.43): stream write sends on the connection (it must
        // be ESTABLISHED; net_tcp_send reports -1 otherwise). UDP needs a
        // destination — use sendto/write with a bound peer via SYS_SENDTO.
        if (global_fds[gfd].flags == 2 /* SOCK_DGRAM */) {
            fd_lock_release();
            return -1;
        }
        int conn = global_fds[gfd].sock_conn;
        fd_lock_release();
        int r = net_tcp_send(conn, (uint8_t*)buf, (uint32_t)size);
        return (r == -2) ? -1 : r;
    }
    fd_lock_release();
    return -1;
}

// POSIX lseek: reposition a file descriptor's read/write offset. whence is
// SEEK_SET(0) absolute, SEEK_CUR(1) relative to the current offset,
// SEEK_END(2) relative to the end of the file. Returns the new offset, or -1
// on error (bad fd, non-file descriptor, or a negative result). Pipes are
// not seekable.
int do_sys_lseek(int fd, int offset, int whence) {
    int tid = get_current_task();
    if (tid < 0) return -1;

    fd_lock_acquire();
    int gfd = task_get_fd(tid, fd);
    if (gfd < 0 || !global_fds[gfd].in_use) { fd_lock_release(); return -1; }
    if (global_fds[gfd].type != FD_TYPE_FILE && global_fds[gfd].type != FD_TYPE_DEV) {
        fd_lock_release(); return -1;
    }

    int node = global_fds[gfd].vfs_node;
    int filesz = fs_nodes[node].in_use ? fs_nodes[node].size : 0;
    int newoff;
    switch (whence) {
        case SEEK_SET: newoff = offset; break;
        case SEEK_CUR: newoff = global_fds[gfd].offset + offset; break;
        case SEEK_END: newoff = filesz + offset; break;
        default: fd_lock_release(); return -1;
    }
    if (newoff < 0) { fd_lock_release(); return -1; }  // EINVAL

    global_fds[gfd].offset = newoff;
    fd_lock_release();
    return newoff;
}

// POSIX fstat: fill a stat_t with the metadata of an open file descriptor's
// VFS node. Works for file/device descriptors; pipes are not stat-able.
int do_sys_fstat(int fd, stat_t* out) {
    int tid = get_current_task();
    if (tid < 0) return -1;

    fd_lock_acquire();
    int gfd = task_get_fd(tid, fd);
    if (gfd < 0 || !global_fds[gfd].in_use) { fd_lock_release(); return -1; }
    if (global_fds[gfd].type != FD_TYPE_FILE && global_fds[gfd].type != FD_TYPE_DEV) {
        fd_lock_release(); return -1;
    }

    int node = global_fds[gfd].vfs_node;
    if (!fs_nodes[node].in_use) { fd_lock_release(); return -1; }
    out->size = fs_nodes[node].size;
    out->type = (int)fs_nodes[node].type;
    out->node_idx = node;
    out->parent = fs_nodes[node].parent;
    out->data_sector = fs_nodes[node].data_sector;
    for (int i = 0; i < MAX_FILENAME; i++) {
        out->name[i] = fs_nodes[node].name[i];
        if (!fs_nodes[node].name[i]) break;
    }
    out->name[MAX_FILENAME - 1] = '\0';
    // Ownership & permission bits (v38.23)
    out->mode = fs_nodes[node].mode;
    out->uid = fs_nodes[node].uid;
    out->gid = fs_nodes[node].gid;
    fd_lock_release();
    return 0;
}

// Events a single fd reports RIGHT NOW (no blocking). Returns a subset of
// POLLIN/POLLOUT/POLLHUP, or POLLNVAL if the fd is not open. Must be called
// with fd_lock held (it reads the shared descriptor/pipe tables).
static int fd_poll_events(int tid, int fd) {
    int gfd = task_get_fd(tid, fd);
    if (gfd < 0 || !global_fds[gfd].in_use) return POLLNVAL;

    int rev = 0;
    switch (global_fds[gfd].type) {
        case FD_TYPE_FILE:
        case FD_TYPE_DEV:
            // Regular files and device/proc nodes never block: a read returns
            // immediately (data or EOF) and a write is always accepted.
            rev = POLLIN | POLLOUT;
            break;
        case FD_TYPE_PIPE_READ: {
            int p = global_fds[gfd].pipe_id;
            if (pipes[p].read_pos != pipes[p].write_pos) rev |= POLLIN;
            else if (pipes[p].closed_write) rev |= POLLIN | POLLHUP; // EOF
            break;
        }
        case FD_TYPE_PIPE_WRITE: {
            int p = global_fds[gfd].pipe_id;
            int next = (pipes[p].write_pos + 1) % PIPE_BUF_SIZE;
            if (next != pipes[p].read_pos) rev |= POLLOUT; // space left
            break;
        }
        case FD_TYPE_SOCKET: {
            int conn = global_fds[gfd].sock_conn;
            if (global_fds[gfd].flags == 2 /* SOCK_DGRAM */) {
                // UDP: readable whenever a datagram is buffered host-side
                // (net_udp_recv drains one per call); always writable.
                rev = POLLIN | POLLOUT;
            } else if (conn >= 0 && net_tcp_state(conn) == TCP_LISTEN) {
                // Listening socket: readable when accept() would return a
                // completed child (POSIX POLLIN-on-listener semantics).
                if (net_tcp_accept_pending(conn)) rev |= POLLIN;
            } else if (conn >= 0) {
                int st = net_tcp_state(conn);
                if (st == TCP_ESTABLISHED) {
                    rev = POLLOUT;
                    if (net_tcp_rx_pending(conn)) rev |= POLLIN;
                } else if (st == TCP_CLOSED) {
                    rev = POLLHUP;   // reset / never connected
                }
                // ESTABLISHED with EOF drained also reports POLLIN|POLLHUP
                if (st == TCP_ESTABLISHED && net_tcp_eof(conn)) rev |= POLLIN | POLLHUP;
            }
            break;
        }
        default:
            break;
    }
    return rev;
}

// Convert a millisecond timeout to a tick count without 64-bit division (the
// freestanding kernel has no __udivdi3). Decompose a*b/c = (a/c)*b + (a%c)*b/c
// so every intermediate fits in 32 bits; clamp on overflow so an absurd
// timeout degrades to "essentially forever" instead of wrapping negative.
static uint32_t ms_to_ticks(uint32_t ms) {
    uint32_t tps = ticks_per_sec;
    if (tps == 0) return 0;
    uint32_t sec = ms / 1000;          // whole seconds
    uint32_t rem = ms % 1000;          // leftover milliseconds
    uint32_t part = (sec > 0xFFFFFFFFu / tps) ? 0xFFFFFFFFu : sec * tps;
    uint32_t tail = rem * tps / 1000;  // rem <= 999, always safe
    return (part > 0xFFFFFFFFu - tail) ? 0xFFFFFFFFu : part + tail;
}

// POSIX poll(): scan the fds array, fill each revents, and return the number
// of descriptors with pending events. timeout_ms semantics: 0 = return
// immediately, >0 = wait up to that long (in ms, scaled by the calibrated
// ticks_per_sec), <0 = block forever. The wait loop mirrors the pipe
// read/write pattern: drop fd_lock, `sti; hlt` for one tick, re-check.
int do_sys_poll(pollfd_t* fds, int nfds, int timeout_ms) {
    int tid = get_current_task();
    if (tid < 0) return -1;
    if (nfds < 0 || nfds > MAX_FDS_PER_TASK) return -1;

    uint32_t start = get_ticks();
    // Wrapping add is fine here: the loop compares wrapped tick counters too.
    int deadline = (timeout_ms < 0) ? -1
        : (int)(start + ms_to_ticks((uint32_t)timeout_ms));

    for (;;) {
        int ready = 0;
        fd_lock_acquire();
        for (int i = 0; i < nfds; i++) {
            int ev = fd_poll_events(tid, fds[i].fd);
            if (ev == POLLNVAL) {
                fds[i].revents = POLLNVAL;          // always reported
                ready++;
            } else {
                // POLLHUP is reported even when not requested (EOF signal).
                fds[i].revents = (ev & fds[i].events) | (ev & POLLHUP);
                if (fds[i].revents) ready++;
            }
        }
        fd_lock_release();
        if (ready > 0) return ready;
        if (timeout_ms == 0) return 0;
        if (deadline >= 0 && (int)get_ticks() >= deadline) return 0;
        __asm__ volatile("sti; hlt");
    }
}

// POSIX select(): bitmaps are uint32_t (fd < 32). readfds/writefds are
// rewritten in place to the ready fds (zeroed on timeout); exceptfds is
// always cleared (no exceptional conditions are tracked). Returns the ready
// count, 0 on timeout, -1 on error.
int do_sys_select(int nfds, uint32_t* readfds, uint32_t* writefds,
                  uint32_t* exceptfds, int timeout_ms) {
    int tid = get_current_task();
    if (tid < 0) return -1;
    if (nfds < 0 || nfds > MAX_FDS_PER_TASK) return -1;

    // Snapshot the requested sets once — the loop rewrites the live bitmaps
    // every pass, so re-reading them after a sleep would lose the fds that
    // were already cleared while nothing was ready.
    uint32_t want_r = readfds ? *readfds : 0;
    uint32_t want_w = writefds ? *writefds : 0;

    uint32_t start = get_ticks();
    int deadline = (timeout_ms < 0) ? -1
        : (int)(start + ms_to_ticks((uint32_t)timeout_ms));

    for (;;) {
        uint32_t r = want_r, w = want_w;
        int ready = 0;
        fd_lock_acquire();
        for (int fd = 0; fd < nfds; fd++) {
            int ev = fd_poll_events(tid, fd);
            if (r & (1u << fd)) {
                if (ev & POLLIN) ready++; else r &= ~(1u << fd);
            }
            if (w & (1u << fd)) {
                if (ev & POLLOUT) ready++; else w &= ~(1u << fd);
            }
        }
        fd_lock_release();
        if (ready > 0 || timeout_ms == 0 ||
            (deadline >= 0 && (int)get_ticks() >= deadline)) {
            if (readfds) *readfds = r;
            if (writefds) *writefds = w;
            if (exceptfds) *exceptfds = 0;
            return ready;
        }
        __asm__ volatile("sti; hlt");
    }
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
        } else if (global_fds[gfd].type == FD_TYPE_SOCKET &&
                   global_fds[gfd].sock_conn >= 0) {
            // Last reference to a socket: tear the connection down too.
            net_tcp_close(global_fds[gfd].sock_conn);
        }
        global_fds[gfd].in_use = 0;
    }
}

int do_sys_close(int fd) {
    int tid = get_current_task();
    if (tid < 0) return -1;

    fd_lock_acquire();
    int gfd = task_get_fd(tid, fd);
    if (gfd < 0 || !global_fds[gfd].in_use) { fd_lock_release(); return -1; }

    task_set_fd(tid, fd, -1);
    fd_release_global(gfd);
    fd_lock_release();
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

    fd_lock_acquire();
    int ogfd = task_get_fd(tid, oldfd);
    if (ogfd < 0 || !global_fds[ogfd].in_use) { fd_lock_release(); return -1; }

    // RLIMIT_NOFILE (v38.28): dup2 onto a slot that is currently EMPTY opens
    // a new local fd, so it counts against the limit (only when dup2 targets
    // the calling task itself — child rewiring in task_rewire_fds runs on a
    // not-yet-running task whose caller is the shell, and must not be limited
    // by the shell's own fd budget).
    if (tid == get_current_task() && task_get_fd(tid, newfd) < 0 &&
        !task_rlimit_nofile_ok()) { fd_lock_release(); return -1; }

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
    fd_lock_release();
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
        fd_lock_acquire();
        int gfd = task_get_fd(tid, j);
        if (gfd >= 0) {
            task_set_fd(tid, j, -1);
            if (gfd < MAX_GLOBAL_FDS && global_fds[gfd].in_use) fd_release_global(gfd);
        }
        fd_lock_release();
    }
}

// Close every fd a task holds. Called from task_cleanup() (exit/kill) — without
// this, the task's global fd slots and pipes leaked forever, and a pipe reader
// whose writer task died blocked on a pipe that was never closed.
void task_close_all_fds(int tid) {
    for (int i = 0; i < MAX_FDS_PER_TASK; i++) {
        fd_lock_acquire();
        int gfd = task_get_fd(tid, i);
        if (gfd < 0) { fd_lock_release(); continue; }
        task_set_fd(tid, i, -1);
        if (gfd < MAX_GLOBAL_FDS && global_fds[gfd].in_use) fd_release_global(gfd);
        fd_lock_release();
    }
}

// ============================================================
// POSIX socket descriptors (v38.43)
// ============================================================
// socket()/accept() allocate ordinary file descriptors whose global entry
// is FD_TYPE_SOCKET. flags stores SOCK_STREAM/SOCK_DGRAM, offset stashes
// the bind() port until listen()/connect() consume it, and sock_conn holds
// the TCP conn id (-1 until then; UDP sockets keep -1 forever — the UDP
// path uses the stack's single global binding, documented limitation).

// socket(type): allocate an unattached socket descriptor. Returns the local
// fd or -1 (no global slot / NOFILE limit reached).
int sock_socket(int type) {
    fd_lock_acquire();
    int gfd = alloc_global_fd();
    if (gfd < 0) { fd_lock_release(); return -1; }
    global_fds[gfd].type = FD_TYPE_SOCKET;
    global_fds[gfd].flags = type;
    global_fds[gfd].sock_conn = -1;
    global_fds[gfd].offset = 0;   // bind() port goes here
    global_fds[gfd].vfs_node = -1;
    int lfd = task_map_fd(gfd);
    if (lfd < 0) { global_fds[gfd].in_use = 0; fd_lock_release(); return -1; }
    fd_lock_release();
    return lfd;
}

// Record the local port for a later listen()/connect(). UDP also arms the
// stack's global binding. Returns 0 or -1 (bad fd / not a socket).
int sock_bind(int lfd, uint16_t port) {
    int tid = get_current_task();
    if (tid < 0) return -1;
    fd_lock_acquire();
    int gfd = task_get_fd(tid, lfd);
    if (gfd < 0 || !global_fds[gfd].in_use ||
        global_fds[gfd].type != FD_TYPE_SOCKET) { fd_lock_release(); return -1; }
    global_fds[gfd].offset = (int)port;
    int is_dgram = (global_fds[gfd].flags == 2 /* SOCK_DGRAM */);
    fd_lock_release();
    if (is_dgram) net_udp_bind(port);
    return 0;
}

// Get the attached conn id of a socket fd: >= 0 the conn id, -1 a valid
// socket with nothing attached yet, -2 not a socket fd at all.
int sock_get_conn(int lfd) {
    int tid = get_current_task();
    if (tid < 0) return -2;
    fd_lock_acquire();
    int gfd = task_get_fd(tid, lfd);
    if (gfd < 0 || !global_fds[gfd].in_use ||
        global_fds[gfd].type != FD_TYPE_SOCKET) { fd_lock_release(); return -2; }
    int conn = global_fds[gfd].sock_conn;
    fd_lock_release();
    return conn;
}

int sock_set_conn(int lfd, int conn) {
    int tid = get_current_task();
    if (tid < 0) return -1;
    fd_lock_acquire();
    int gfd = task_get_fd(tid, lfd);
    if (gfd < 0 || !global_fds[gfd].in_use ||
        global_fds[gfd].type != FD_TYPE_SOCKET) { fd_lock_release(); return -1; }
    global_fds[gfd].sock_conn = conn;
    fd_lock_release();
    return 0;
}

// accept(listener_fd): attach one completed inbound connection to a fresh
// socket fd. Returns the new local fd or -1 (nothing pending / no slot).
int sock_accept(int lfd) {
    int conn = sock_get_conn(lfd);
    if (conn < 0) return -1;
    int child = net_tcp_accept(conn);
    if (child < 0) return -1;

    int tid = get_current_task();
    if (tid < 0) return -1;
    fd_lock_acquire();
    int gfd = alloc_global_fd();
    if (gfd < 0) { fd_lock_release(); return -1; }
    global_fds[gfd].type = FD_TYPE_SOCKET;
    global_fds[gfd].flags = SOCK_STREAM;
    global_fds[gfd].sock_conn = child;
    global_fds[gfd].vfs_node = -1;
    global_fds[gfd].offset = 0;
    int nfd = task_map_fd(gfd);
    if (nfd < 0) { global_fds[gfd].in_use = 0; fd_lock_release(); return -1; }
    fd_lock_release();
    return nfd;
}

int do_sys_pipe(int pipefd[2]) {
    fd_lock_acquire();
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
    if (p < 0) { fd_lock_release(); return -1; }
    
    int g_read = alloc_global_fd();
    if (g_read < 0) { pipes[p].in_use = 0; fd_lock_release(); return -1; }
    global_fds[g_read].type = FD_TYPE_PIPE_READ;
    global_fds[g_read].pipe_id = p;
    
    int g_write = alloc_global_fd();
    if (g_write < 0) { global_fds[g_read].in_use = 0; pipes[p].in_use = 0; fd_lock_release(); return -1; }
    global_fds[g_write].type = FD_TYPE_PIPE_WRITE;
    global_fds[g_write].pipe_id = p;
    
    pipefd[0] = task_map_fd(g_read);
    pipefd[1] = task_map_fd(g_write);
    
    if (pipefd[0] < 0 || pipefd[1] < 0) {
        // Cleanup on error — outside the lock so the do_sys_close() calls can
        // acquire fd_lock themselves. A failed mapping must release the global
        // fds and pipe slot explicitly for the fd(s) that never got mapped.
        int r0 = pipefd[0], r1 = pipefd[1];
        fd_lock_release();
        if (r0 >= 0) do_sys_close(r0);
        else { fd_lock_acquire(); global_fds[g_read].in_use = 0; fd_lock_release(); }
        if (r1 >= 0) do_sys_close(r1);
        else { fd_lock_acquire(); global_fds[g_write].in_use = 0; fd_lock_release(); }
        fd_lock_acquire();
        pipes[p].in_use = 0;
        fd_lock_release();
        return -1;
    }
    fd_lock_release();
    return 0;
}
