// Pseudo-terminal (PTY) support. See include/pty.h for the model.
//
// Integration: a PTY is just another descriptor type (FD_TYPE_PTY) living in
// the SAME global_fds[] table as files/pipes/sockets. Each PTY fd records its
// pty index in `pipe_id` and whether it is the master in `flags` bit 0. That
// makes fd.c's existing read/write/close dispatch on global_fds[].type and the
// per-task fd->global mapping work unchanged — a PTY descriptor maps to a task
// like any other, and do_sys_read/write close route straight into pty_read/
// pty_write.
//
// Locking: fd.c holds the global fd lock across every do_sys_* call, same as
// pipes, so the ring access here is already serialized. No separate lock.
#include "../include/pty.h"
#include "../include/fd.h"
#include "../include/utils.h"   // memset

extern global_fd_t global_fds[];   // fd.c owns the table; we read/write slots
// MAX_GLOBAL_FDS comes from fd.h (256). We claim slots from the SAME global
// table so per-task fd mapping and the fd read/write dispatch work unchanged.

static pty_t ptys[MAX_PTYS];

void pty_init(void) {
    for (int i = 0; i < MAX_PTYS; i++) ptys[i].in_use = 0;
}

static int pty_slot_is_master(int gfd) {
    if (gfd < 0 || gfd >= MAX_GLOBAL_FDS || !global_fds[gfd].in_use)
        return -1;
    if (global_fds[gfd].type != FD_TYPE_PTY) return -1;
    return (global_fds[gfd].flags & 1);
}

// Claim a global fd slot as a PTY. Returns the gfd or -1.
static int pty_claim_gfd(int pty_idx, int is_master) {
    if (pty_idx < 0 || pty_idx >= MAX_PTYS || !ptys[pty_idx].in_use) return -1;
    for (int i = 0; i < MAX_GLOBAL_FDS; i++) {
        if (!global_fds[i].in_use) {
            global_fds[i].in_use = 1;
            global_fds[i].type = FD_TYPE_PTY;
            global_fds[i].pipe_id = pty_idx;
            global_fds[i].flags = is_master ? 1 : 0;
            global_fds[i].ref_count = 1;
            global_fds[i].offset = 0;
            global_fds[i].sock_conn = -1;
            return i;
        }
    }
    return -1;
}

// Allocate a fresh PTY, return its master end as a global fd.
int pty_alloc_fd(void) {
    int p = -1;
    for (int i = 0; i < MAX_PTYS; i++) {
        if (!ptys[i].in_use) { p = i; break; }
    }
    if (p < 0) return -1;

    pty_t* t = &ptys[p];
    memset(t, 0, sizeof(*t));
    t->in_use = 1;
    t->master_fd = -1;

    int gfd = pty_claim_gfd(p, 1);
    if (gfd < 0) { t->in_use = 0; return -1; }
    t->master_fd = gfd;
    return gfd;
}

// Open the slave end of an existing PTY (by the master's global fd).
int pty_open_slave(int master_fd) {
    int p = -1;
    int ism = pty_slot_is_master(master_fd);
    if (ism != 1) return -1;
    p = global_fds[master_fd].pipe_id;
    if (p < 0 || !ptys[p].in_use) return -1;

    int gfd = pty_claim_gfd(p, 0);
    if (gfd < 0) return -1;
    ptys[p].slave_count++;
    return gfd;
}

// Read from a PTY descriptor. The master reads the slave's output (s2m); the
// slave reads the master's (m2s). Returns bytes read (0 when empty).
int pty_read(int gfd, char* buf, int size) {
    int ism = pty_slot_is_master(gfd);
    if (ism < 0) return -1;
    int p = global_fds[gfd].pipe_id;
    pty_t* t = &ptys[p];

    char* ring = ism ? t->s2m : t->m2s;
    int*  rp   = ism ? &t->s2m_r : &t->m2s_r;
    int*  wp   = ism ? &t->s2m_w : &t->m2s_w;

    int n = 0;
    while (n < size && *rp != *wp) {
        buf[n++] = ring[*rp];
        (*rp)++;
        if (*rp >= PTY_BUF_SIZE) *rp = 0;
    }
    return n;
}

// Write to a PTY descriptor. The master feeds the slave (m2s); the slave
// feeds the master (s2m). Returns bytes written (0 when the ring is full).
int pty_write(int gfd, const char* buf, int size) {
    int ism = pty_slot_is_master(gfd);
    if (ism < 0) return -1;
    int p = global_fds[gfd].pipe_id;
    pty_t* t = &ptys[p];

    char* ring = ism ? t->m2s : t->s2m;
    int*  rp   = ism ? &t->m2s_r : &t->s2m_r;
    int*  wp   = ism ? &t->m2s_w : &t->s2m_w;

    int n = 0;
    while (n < size) {
        int next_w = (*wp + 1) % PTY_BUF_SIZE;
        if (next_w == *rp) break;
        ring[*wp] = buf[n++];
        *wp = next_w;
    }
    return n;
}

// Close a PTY descriptor (master or slave). Reclaims the pty when the last
// reference is gone.
void pty_close(int gfd) {
    int ism = pty_slot_is_master(gfd);
    if (ism < 0) return;
    int p = global_fds[gfd].pipe_id;
    pty_t* t = &ptys[p];

    if (ism) {
        t->master_fd = -1;
        if (t->slave_count <= 0) t->in_use = 0;   // no slaves: reclaim now
        // else: slaves orphaned, reclaimed when the last one closes
    } else {
        if (t->slave_count > 0) t->slave_count--;
        if (t->slave_count <= 0 && t->master_fd < 0) t->in_use = 0;
    }
    global_fds[gfd].in_use = 0;   // free the fd slot for reuse
    global_fds[gfd].type = FD_TYPE_NONE;
}

// ---- Boot-time self-test (v38.168) ----
// Round-trips one PTY entirely in the kernel (global fds, no task mapping):
//   1. alloc master, open slave
//   2. master -> slave: "HI" must arrive intact
//   3. slave -> master: "OK" must arrive intact
//   4. close both; the pty slot must be reclaimed (leak check)
// Logs PASS/FAIL to serial. Called once from init_syscalls (after pty_init).
void pty_selftest(void) {
    extern void write_serial_string(const char*);
    write_serial_string("[PTY] selftest: alloc master\n");
    int mg = pty_alloc_fd();
    if (mg < 0) { write_serial_string("[PTY] selftest FAIL (alloc)\n"); return; }

    int sl = pty_open_slave(mg);
    if (sl < 0) { write_serial_string("[PTY] selftest FAIL (open slave)\n"); pty_close(mg); return; }

    // 2. master write -> slave read
    write_serial_string("[PTY] selftest: master->slave\n");
    if (pty_write(mg, "HI", 2) != 2) {
        write_serial_string("[PTY] selftest FAIL (m2s short write)\n");
        pty_close(sl); pty_close(mg); return;
    }
    char got[4] = {0};
    int r = pty_read(sl, got, sizeof(got));
    if (r != 2 || got[0] != 'H' || got[1] != 'I') {
        write_serial_string("[PTY] selftest FAIL (m2s data)\n");
        pty_close(sl); pty_close(mg); return;
    }

    // 3. slave write -> master read
    write_serial_string("[PTY] selftest: slave->master\n");
    if (pty_write(sl, "OK", 2) != 2) {
        write_serial_string("[PTY] selftest FAIL (s2m short write)\n");
        pty_close(sl); pty_close(mg); return;
    }
    char got2[4] = {0};
    r = pty_read(mg, got2, sizeof(got2));
    if (r != 2 || got2[0] != 'O' || got2[1] != 'K') {
        write_serial_string("[PTY] selftest FAIL (s2m data)\n");
        pty_close(sl); pty_close(mg); return;
    }

    // 4. close both, slot must be reclaimed
    write_serial_string("[PTY] selftest: close + reclaim\n");
    pty_close(sl);
    pty_close(mg);
    int reclaimed = 1;
    for (int i = 0; i < MAX_PTYS; i++) {
        if (ptys[i].in_use) { reclaimed = 0; break; }
    }
    if (!reclaimed) { write_serial_string("[PTY] selftest FAIL (leak: pty still in_use)\n"); return; }

    write_serial_string("[PTY] selftest PASS (m2s ok, s2m ok, reclaim ok)\n");
}

// poll/select participation (v38.170): same predicate family the pipe cases
// use in fd.c — readable when the ring we read from is non-empty, writable
// when the ring we write to has space. Caller holds fd_lock.
int pty_poll_events(int pty_idx, int is_master) {
    if (pty_idx < 0 || pty_idx >= MAX_PTYS || !ptys[pty_idx].in_use) return 0;
    const pty_t* t = &ptys[pty_idx];
    int rev = 0;
    if (is_master) {
        if (t->s2m_r != t->s2m_w) rev |= 0x0001;          // POLLIN
        int nw = (t->m2s_w + 1) % PTY_BUF_SIZE;
        if (nw != t->m2s_r) rev |= 0x0004;                // POLLOUT
    } else {
        if (t->m2s_r != t->m2s_w) rev |= 0x0001;          // POLLIN
        int nw = (t->s2m_w + 1) % PTY_BUF_SIZE;
        if (nw != t->s2m_r) rev |= 0x0004;                // POLLOUT
    }
    return rev;
}
