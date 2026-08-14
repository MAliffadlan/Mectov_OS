// src/sys/shell/builtins/process_ops/cmd_ulimit.c — the `ulimit` shell command.
// v38.28: display or change the current task's POSIX resource limits
// (RLIMIT_NPROC/RLIMIT_AS/RLIMIT_NOFILE) through SYS_GETRLIMIT/SYS_SETRLIMIT
// semantics (task_getrlimit/task_setrlimit). Bash-style flags:
//
//   ulimit            show all soft limits
//   ulimit -a         show all soft limits (verbose)
//   ulimit -n         show soft RLIMIT_NOFILE (open fds)
//   ulimit -u         show soft RLIMIT_NPROC (processes/threads)
//   ulimit -v         show soft RLIMIT_AS (address space, bytes)
//   ulimit -H <opt>   use the HARD limit instead of the soft one
//   ulimit -n 32      set the soft NOFILE limit to 32
//   ulimit -Hn 32     set the hard NOFILE limit to 32 (root only)
//
// Values are bytes for -v, counts for -n/-u. A non-root caller may lower a
// soft limit and raise it up to the hard limit, but may never raise the hard
// limit itself (POSIX — enforced by task_setrlimit). The limits are per-task
// and inherited by fork/exec/thread-create, so a lowered `ulimit -u` protects
// the whole process tree from fork bombs.
#include "../../shell_internal.h"

static const char* rlimit_name(int res) {
    switch (res) {
        case RLIMIT_NPROC:  return "nproc";
        case RLIMIT_AS:     return "as";
        case RLIMIT_NOFILE: return "nofile";
        default:            return "?";
    }
}

// Print one line to the terminal AND the serial log (so scripts/tests can
// assert on it). Values print raw: counts for -n/-u, bytes for -v.
static void ulimit_print_line(int res, uint32_t cur, uint32_t max) {
    print("ulimit: ", 0x0F);
    print(rlimit_name(res), 0x0F);
    print(" soft=", 0x0F);
    p_int((int)cur, 0x0A);
    print(" hard=", 0x0F);
    p_int((int)max, 0x0A);
    print("\n", 0x0F);

    extern void write_serial_string(const char*);
    char b[64];
    int n = 0;
    const char* s = "[ULIMIT] get ";
    while (*s && n < 62) b[n++] = *s++;
    s = rlimit_name(res);
    while (*s && n < 62) b[n++] = *s++;
    b[n++] = ' '; b[n++] = 'c'; b[n++] = 'u'; b[n++] = 'r'; b[n++] = '=';
    unsigned v = cur;
    char tmp[12]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v > 0) { tmp[t++] = '0' + (v % 10); v /= 10; }
    while (t > 0 && n < 62) b[n++] = tmp[--t];
    s = " max=";
    while (*s && n < 62) b[n++] = *s++;
    v = max; t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v > 0) { tmp[t++] = '0' + (v % 10); v /= 10; }
    while (t > 0 && n < 62) b[n++] = tmp[--t];
    b[n++] = '\n'; b[n] = '\0';
    write_serial_string(b);
}

void cmd_ulimit(void) {
    char* arg = cmd_b + 7;   // skip "ulimit "
    if (arg > cmd_b && arg[-1] != ' ') arg = cmd_b + 6; // bare "ulimit"
    while (*arg == ' ') arg++;

    int hard = 0;
    int all = 0;
    int res = -1;             // -1 = none selected yet
    int have_value = 0;
    uint32_t value = 0;

    // Bash-style flag parsing: -S soft (default), -H hard, -a all, then one
    // of -n/-u/-v, then an optional numeric value (possibly on the same or a
    // separate token, e.g. "ulimit -n 32" or "ulimit -un 32" is not valid —
    // flags and the value are space-separated like bash).
    char* p = arg;
    while (*p) {
        while (*p == ' ') p++;
        if (*p == '-') {
            p++;
            while (*p && *p != ' ') {
                if (*p == 'S') { hard = 0; }
                else if (*p == 'H') { hard = 1; }
                else if (*p == 'a') { all = 1; }
                else if (*p == 'n') { res = RLIMIT_NOFILE; all = 0; }
                else if (*p == 'u') { res = RLIMIT_NPROC;  all = 0; }
                else if (*p == 'v') { res = RLIMIT_AS;     all = 0; }
                else {
                    print("ulimit: unknown option -", 0x0C);
                    p_char(*p, 0x0C);
                    print(" (use -n -u -v -a -S -H)\\n", 0x0C);
                    return;
                }
                p++;
            }
        } else {
            // Numeric value (only meaningful when a single resource is set).
            if (*p < '0' || *p > '9') {
                print("ulimit: usage: ulimit [-SH] [-a] [-n|-u|-v] [value]\\n", 0x0C);
                return;
            }
            have_value = 1;
            value = 0;
            while (*p >= '0' && *p <= '9' && value <= 0x7FFFFFFFu) {
                value = value * 10 + (uint32_t)(*p - '0');
                p++;
            }
            while (*p && *p != ' ') p++;
        }
    }

    rlimit_t rl;

    // No resource selected -> show every limit (bash `ulimit` / `ulimit -a`).
    // The hard flag is irrelevant here: print soft AND hard for each resource
    // so one command shows the whole picture.
    if (res < 0 || all) {
        for (int r = 0; r < RLIM_NLIMITS; r++) {
            if (task_getrlimit(r, &rl) != 0) continue;
            ulimit_print_line(r, rl.cur, rl.max);
        }
        return;
    }

    // Set mode: a single resource plus a value.
    if (have_value) {
        if (task_getrlimit(res, &rl) != 0) {
            print("ulimit: bad resource\n", 0x0C);
            return;
        }
        if (hard) rl.max = value; else rl.cur = value;
        if (task_setrlimit(res, &rl) != 0) {
            print("ulimit: cannot set ", 0x0C);
            print(rlimit_name(res), 0x0C);
            print(" to ", 0x0C);
            p_int((int)value, 0x0C);
            print(" (non-root may only lower soft limits up to the hard limit)\\n", 0x0C);
            extern void write_serial_string(const char*);
            write_serial_string("[ULIMIT] set failed\n");
            return;
        }
        print("ulimit: set ", 0x0A);
        print(rlimit_name(res), 0x0A);
        print(hard ? " hard" : " soft", 0x0A);
        print(" = ", 0x0A);
        p_int((int)value, 0x0A);
        print("\n", 0x0A);
        extern void write_serial_string(const char*);
        write_serial_string("[ULIMIT] set ");
        write_serial_string(rlimit_name(res));
        write_serial_string("\n");
        return;
    }

    // Query mode: show the selected resource.
    if (task_getrlimit(res, &rl) != 0) {
        print("ulimit: bad resource\n", 0x0C);
        return;
    }
    if (hard) {
        p_int((int)rl.max, 0x0F);
        print("\n", 0x0F);
    } else {
        p_int((int)rl.cur, 0x0F);
        print("\n", 0x0F);
    }
    // Serial echo for the test harness (query result).
    ulimit_print_line(res, rl.cur, rl.max);
}
