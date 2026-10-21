#ifndef PTY_H
#define PTY_H

#include "types.h"

// Pseudo-terminal (PTY) support (v38.168+).
//
// A PTY pairs a MASTER and a SLAVE side connected by two ring buffers, mirroring
// the existing pipe_t pattern in fd.c:
//   * master-write  -> slave ring  (terminal window keystrokes become the
//                                    shell/process running on the slave's stdin)
//   * slave-write   -> master ring (the process's stdout/stderr returns to the
//                                    terminal window on the master)
// The master is what a GUI terminal app holds; the slave is what a shell (or any
// process) is spawned with as its stdin/stdout. This is the minimal modern
// terminal abstraction: no termios line discipline yet — raw byte streaming is
// the default, matching how pipes already behave. A process on the slave reads
// exactly what the master writes, and vice-versa.

#define PTY_BUF_SIZE 4096
#define MAX_PTYS     16

typedef struct {
    int  in_use;
    // Master->slave ring (what the terminal sends to the shell).
    char m2s[PTY_BUF_SIZE];
    int  m2s_r, m2s_w;
    // Slave->master ring (what the shell echoes/prints back to the terminal).
    char s2m[PTY_BUF_SIZE];
    int  s2m_r, s2m_w;
    int  master_fd;    // global fd of the open master (or -1)
    int  slave_count;  // open slave fds
    int  slave_closed; // slave side has no live writers
} pty_t;

void pty_init(void);

// Allocate a fresh PTY and return its master end as a global fd. Returns the
// global fd, or -1 when the table is full.
int  pty_alloc_fd(void);

// Open the slave end of an existing PTY (by master fd). Returns a new global
// fd for the slave side, or -1.
int  pty_open_slave(int master_fd);

// Read/write/close a PTY descriptor. `is_master` selects which ring: reading
// from the master consumes the slave's output; writing to the master feeds the
// slave.
int  pty_read(int gfd, char* buf, int size);
int  pty_write(int gfd, const char* buf, int size);
void pty_close(int gfd);

#endif
