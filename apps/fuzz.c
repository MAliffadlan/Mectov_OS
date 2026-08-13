// fuzz.c — Mectov OS Ring 3 Syscall Fuzzer
//
// Exercises the int 0x80 syscall boundary with random and hostile arguments:
// NULL / unmapped / kernel-identity addresses, absurd sizes and counts,
// invalid fds and pids. Every call must return an error cleanly — the kernel
// must NEVER panic or fault at CPL 0. Used by scripts/fuzz_test.py.
//
// Deliberately excluded (they would block or self-destruct rather than probe
// anything new): SYS_FORK (fork bomb), SYS_EXIT (used only at the end),
// SYS_IPC_RECV / SYS_SEM_WAIT / SYS_FUTEX_WAIT (block forever), SYS_PIPE /
// SYS_DUP2 (fd wiring churn), SYS_EXEC (image replacement), TCP/DNS (long
// network timeouts), and reads/writes on fds 0..2 (the terminal wiring may
// block). Signals are only ever sent as SIGCHLD/SIGCONT (default-ignored /
// benign) and handlers are only installed as default/ignore, so execution can
// never be redirected into garbage by a fuzzed signal.
//
// Run it from the terminal:  run /apps/fuzz.mct
#include "src/include/syscall.h"

// ---- Tiny PRNG (xorshift32) ----
static unsigned int rng_state = 0x2A6F20B5u;
static unsigned int rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

// Valid, mapped app memory (the MCT loader zeroes the whole data region).
static char buf[512];       // generic buffer + inert 'A' string source
static char arrbuf[16384];  // array-returning syscalls (256 entries * 44B max)
static char small[16];      // short printable string for SYS_PRINT

// A pointer the kernel MUST reject: never points into this app's memory.
static unsigned int pick_bad_ptr(void) {
    switch (rng() % 6) {
        case 0: return 0u;
        case 1: return 0xDEADBEEFu;
        case 2: return 0x41414141u;
        case 3: return 0x00100000u;   // kernel identity map (not user-accessible)
        case 4: return 0x09000000u;   // library VA, not mapped here
        default: return 0x7FFFFFFFu;
    }
}

// Mostly-bad, sometimes app memory. buf holds an inert 'A' string, so even a
// "valid" pointer can never resolve to a real path or file.
static unsigned int pick_ptr(void) {
    if (rng() % 4 == 0) return (unsigned int)(uintptr_t)buf;
    return pick_bad_ptr();
}

// fd >= 3 only: fds 0..2 are the terminal wiring (a read could block forever).
static int pick_fd(void) { return 3 + (int)(rng() % 13); }

// Signals this task will NEVER receive (it only ever kills with SIGCHLD /
// SIGCONT), so installing default/ignore handlers for them is inert.
static const int install_sigs[3] = { SIGUSR1, SIGUSR2, SIGTERM };

static void print_hex(unsigned int v) {
    char s[16];
    int i = 0;
    s[i++] = '0'; s[i++] = 'x';
    for (int sh = 28; sh >= 0; sh -= 4) {
        unsigned int d = (v >> sh) & 0xF;
        s[i++] = (char)(d < 10 ? '0' + d : 'A' + (d - 10));
    }
    s[i] = '\0';
    sys_print(s, 0x0E);
}

static void mark(const char* tag, unsigned int v) {
    sys_print(tag, 0x0E);
    print_hex(v);
    sys_print("\n", 0x0E);
}

#define ITERS 2000

void _start(void) {
    // Inert string: any path built from buf is one long "A" — it can never
    // match a real VFS file, so fuzzed SYS_OPEN/SYS_CHDIR etc. are inert.
    for (int i = 0; i < 511; i++) buf[i] = 'A';
    buf[511] = '\0';
    for (int i = 0; i < 15; i++) small[i] = 'A';
    small[15] = '\0';

    sys_print("[FUZZ] start\n", 0x0E);

    // ---- Targeted probes (regressions for past hardening) ----
    // Absurd max_count must be rejected by the 64-bit size math in
    // validate_user_array_ptr (SYS_LIST_DIR / SYS_GET_TASKS overflow fixes),
    // never written past the caller's mapping at CPL 0.
    int r = sys_list_dir((dir_entry_t*)arrbuf, 0x7FFFFFFF, 0);
    mark(r == -1 ? "[FUZZ] probe list_dir_huge OK ret="
                 : "[FUZZ] probe list_dir_huge BAD ret=", (unsigned int)r);

    r = sys_get_tasks((sys_task_info_t*)arrbuf, 0x7FFFFFFF);
    mark(r == -1 ? "[FUZZ] probe get_tasks_huge OK ret="
                 : "[FUZZ] probe get_tasks_huge BAD ret=", (unsigned int)r);

    r = sys_get_windows((sys_win_info_t*)arrbuf, 0x7FFFFFFF);
    mark(r == -1 ? "[FUZZ] probe get_windows_huge OK ret="
                 : "[FUZZ] probe get_windows_huge BAD ret=", (unsigned int)r);

    // SYS_SIGRETURN with no handler in flight: clean -1, never a crafted-
    // frame iret (CS/SS selectors are validated on the sigreturn path).
    r = syscall(SYS_SIGRETURN, 0, 0, 0);
    mark(r == -1 ? "[FUZZ] probe sigreturn_bare OK ret="
                 : "[FUZZ] probe sigreturn_bare BAD ret=", (unsigned int)r);

    // SYS_KILL to an out-of-range pid: -1, and the pgrp/descendant
    // authorization must never escalate this into anything worse.
    r = sys_kill(0x7F, SIGCHLD);
    mark(r == -1 ? "[FUZZ] probe kill_oob OK ret="
                 : "[FUZZ] probe kill_oob BAD ret=", (unsigned int)r);

    // A single GUI probe: garbage window id must fail lookup cleanly (the
    // window table has no window with this id, so this is a plain -1).
    r = syscall(SYS_GET_EVENT, 0x7FFF, (int)buf, 0);
    mark(r == -1 || r == 0 ? "[FUZZ] probe gui_badwid OK ret="
                           : "[FUZZ] probe gui_badwid BAD ret=", (unsigned int)r);

    // ---- Random fuzz loop ----
    unsigned int ok = 0;
    for (int iter = 0; iter < ITERS; iter++) {
        switch (rng() % 40) {
            case 0:  sys_read(pick_fd(), (char*)pick_ptr(), (int)(rng() % 100)); break;
            case 1:  sys_write(pick_fd(), (const char*)pick_ptr(), (int)(rng() % 100)); break;
            case 2:  syscall(SYS_OPEN, (int)pick_ptr(), (int)(rng() & 15), 0); break;
            case 3:  sys_close(pick_fd()); break;
            case 4:  sys_malloc((int)(rng() % 4096)); break;
            case 5:  sys_free((void*)pick_ptr()); break;
            case 6:  sys_get_ticks(); break;
            case 7:  sys_yield(); break;
            case 8:  sys_print((rng() % 4 == 0) ? (const char*)small
                                                : (const char*)pick_bad_ptr(),
                               (int)(rng() & 0xFF)); break;
            case 9:  syscall(SYS_GET_TIME, (int)pick_ptr(), 0, 0); break;
            case 10: syscall(SYS_GET_SYSINFO, (int)pick_ptr(), 0, 0); break;
            case 11: syscall(SYS_GET_PCI_INFO, (int)arrbuf, (int)rng(), 0); break;
            case 12: sys_list_dir((dir_entry_t*)arrbuf, (int)rng(), (int)(rng() % 300)); break;
            case 13: sys_get_tasks((sys_task_info_t*)arrbuf, (int)rng()); break;
            case 14: sys_get_windows((sys_win_info_t*)arrbuf, (int)rng()); break;
            case 15: sys_stat_file((const char*)pick_ptr()); break;
            case 16: /* file ops */
                switch (rng() % 4) {
                    case 0: syscall(SYS_CREATE_FILE, (int)pick_ptr(), 0, 0); break;
                    case 1: syscall(SYS_DELETE_FILE, (int)pick_ptr(), 0, 0); break;
                    case 2: syscall(SYS_MKDIR, (int)pick_ptr(), 0, 0); break;
                    default: syscall(SYS_RENAME_FILE, (int)pick_ptr(), (int)pick_ptr(), 0); break;
                }
                break;
            case 17: syscall(SYS_GET_LAUNCH_ARG, (int)buf, (int)(rng() % 300), 0); break;
            case 18: syscall(SYS_LOAD_LIBRARY, (int)pick_ptr(), 0, 0); break;
            case 19: /* clipboard */
                if (rng() & 1) syscall(SYS_CLIPBOARD_COPY, (int)pick_ptr(), (int)(rng() % 5000), 0);
                else syscall(SYS_CLIPBOARD_PASTE, (int)buf, (int)(rng() % 5000), 0);
                break;
            case 20: sys_sleep((int)(rng() % 8)); break;
            case 21: sys_getpid(); break;
            case 22: syscall(SYS_SET_PRIORITY, (int)(rng() % 70), (int)(rng() % 8), 0); break;
            case 23: syscall(SYS_GET_PRIORITY, (int)(rng() % 70), 0, 0); break;
            case 24: sys_waitpid((int)(rng() % 70), (int*)buf, 1); break;  // WNOHANG only
            case 25: sys_kill((int)(rng() % 70), (rng() & 1) ? SIGCHLD : SIGCONT); break;
            case 26: sys_signal(install_sigs[rng() % 3], (void*)(int)(rng() % 2)); break;
            case 27: /* sigaction: controlled handler (default/ignore only) */
                { uint32_t* a = (uint32_t*)buf;
                  a[0] = (uint32_t)(rng() % 2); a[1] = rng(); a[2] = (rng() & 3);
                  syscall(SYS_SIGACTION, install_sigs[rng() % 3], (int)buf, (int)buf); }
                break;
            case 28: syscall(SYS_SIGPROCMASK, (int)(rng() % 3), (int)buf, (int)buf); break;
            case 29: syscall(SYS_SIGRETURN, 0, 0, 0); break;
            case 30: /* process groups / sessions */
                switch (rng() % 4) {
                    case 0: syscall(SYS_SETPGID, (int)(rng() % 70), (int)(rng() % 70), 0); break;
                    case 1: syscall(SYS_SETSID, 0, 0, 0); break;
                    case 2: syscall(SYS_TCSETPGRP, 0, (int)(rng() % 70), 0); break;
                    default: syscall(SYS_TCGETPGRP, 0, 0, 0); break;
                }
                break;
            case 31: /* VMM */
                if (rng() & 1) syscall(SYS_VMM_ALLOC, (int)(pick_bad_ptr() & ~0xFFFu), 0, 0);
                else syscall(SYS_VMM_FREE, (int)(pick_bad_ptr() & ~0xFFFu), 0, 0);
                break;
            case 32: /* mmap/munmap */
                { unsigned int a = (unsigned int)sys_mmap((int)(rng() % 65536));
                  if (a) sys_munmap((void*)a); }
                break;
            case 33: /* shm: create + immediately remove */
                { int id = syscall(SYS_SHMGET, (int)rng(), (int)(rng() % 8192), 0);
                  if (id > 0) syscall(SYS_SHMCTL, id, 0, 0); }
                break;
            case 34: /* sem: create + post + destroy */
                { int id = syscall(SYS_SEM_CREATE, (int)(rng() % 4), 0, 0);
                  if (id >= 0) { syscall(SYS_SEM_POST, id, 0, 0); syscall(SYS_SEM_DESTROY, id, 0, 0); } }
                break;
            case 35: syscall(SYS_FUTEX_WAKE, (int)pick_ptr(), (int)(rng() % 8), 0); break;
            case 36: /* poll, timeout 0 (never blocks) */
                { pollfd_t* pf = (pollfd_t*)buf;
                  int n = 1 + (int)(rng() % 8);
                  for (int i = 0; i < n; i++) {
                      pf[i].fd = pick_fd();
                      pf[i].events = (int)(rng() & 7);
                      pf[i].revents = 0;
                  }
                  sys_poll(pf, n, 0); }
                break;
            case 37: sys_select((int)(rng() % 32), (uint32_t*)buf, (uint32_t*)buf, 0, 0); break;
            case 38: /* cwd */
                if (rng() & 1) sys_getcwd(buf, (int)(rng() % 300));
                else sys_chdir((const char*)pick_ptr());
                break;
            case 39: /* lseek / fstat / clock_gettime / net status */
                switch (rng() % 4) {
                    case 0: sys_lseek(pick_fd(), (int)rng(), (int)(rng() % 5)); break;
                    case 1: syscall(SYS_FSTAT, pick_fd(), (int)buf, 0); break;
                    case 2: sys_clock_gettime((int)(rng() % 3), (timespec_t*)buf); break;
                    default: syscall(SYS_NET_STATUS, (int)pick_ptr(), 0, 0); break;
                }
                break;
        }
        ok++;
        if ((iter % 250) == 249) {
            sys_print("[FUZZ] iter=", 0x0E);
            print_hex((unsigned int)iter);
            sys_print("\n", 0x0E);
        }
    }

    sys_print("[FUZZ] DONE ok=", 0x0E);
    print_hex(ok);
    sys_print("\n", 0x0E);
    sys_exit();
}
