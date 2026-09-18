// procsysdemo.c — Mectov OS /proc/sys + nice demo (Ring 3), v38.91
// Exercises the runtime-tunable procfs layer and the nice syscalls:
//
//   /proc/sys/all         — the tunable index (name = value (min..max))
//   /proc/sys/<knob>      — a single knob, read back after writing
//   sys_write into knob   — the same path the shell's `echo N > knob`
//                           redirection takes (vfs_write_file ->
//                           vfs_proc_sys_write)
//   sys_nice/sys_getnice  — renice(1)/getpriority(2) equivalents
//   /proc/self/status     — new v38.91 Nice: / CPU%: fields
//
// Serial markers (asserted by scripts/procsys_test.py):
//   [PROCSYS] index-ok          — /proc/sys/all renders all three knobs
//   [PROCSYS] readback-ok       — write zombie_reap_ms=20000, read back
//   [PROCSYS] reject-ok         — out-of-range write rejected, knob intact
//   [PROCSYS] garbage-ok        — non-numeric write rejected, knob intact
//   [PROCSYS] loadwindow-ok     — load_window write+readback+restore
//   [PROCSYS] sweepdiv-ok       — futex_sweep_div write+readback+restore
//   [PROCSYS] nice-ok           — self nice round-trip + boundary clamps
//   [PROCSYS] cpu-ok            — /proc/self/status has Nice: and CPU%:
//   [PROCSYS] done
//
// Run it from the terminal:  run /apps/procsysdemo.mct
#include "src/include/syscall.h"

// ---- tiny local string helpers (MCT apps have no libc) ----
static int s_len(const char* s) { int n = 0; while (s[n]) n++; return n; }
static const char* s_str(const char* hay, const char* needle) {
    if (!*needle) return hay;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return &hay[i];
    }
    return 0;
}
static int s_has_prefix(const char* s, const char* pre) {
    while (*pre) { if (*s++ != *pre++) return 0; }
    return 1;
}
// atoi over the leading (possibly negative) decimal run; garbage -> deflt.
static int s_atoi(const char* s, int deflt) {
    int sign = 1, i = 0;
    if (s[i] == '-') { sign = -1; i++; }
    else if (s[i] == '+') i++;
    if (!(s[i] >= '0' && s[i] <= '9')) return deflt;   // no digits at all
    int v = 0;
    for (; s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (s[i] - '0');
    return sign * v;
}

// Read the whole knob file. Returns byte count or -1.
static int read_all(const char* path, char* buf, int cap) {
    int fd = sys_open(path);
    if (fd < 0) return -1;
    int n = sys_read(fd, buf, cap - 1);
    sys_close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return n;
}

// Write through the fd layer — the same kernel path the shell's
// `echo N > /proc/sys/x` redirection takes (vfs_write_file -> dispatcher).
// sys_write on a fresh fd (offset 0) replaces the file content wholesale.
static int write_all(const char* path, const char* data) {
    int fd = sys_open_mode(path, 0);
    if (fd < 0) return -1;
    int w = sys_write(fd, data, s_len(data));
    sys_close(fd);
    return w;
}

// Pull "name = value (min ..max)" out of an index render; -999999 if absent.
static int knob_value(const char* body, const char* name) {
    int nl = s_len(name);
    for (const char* p = body; *p; ) {
        if (s_has_prefix(p, name) && (p[nl] == ' ' || p[nl] == '\n') ) {
            while (*p && *p != '=') p++;
            if (*p == '=') {
                p++;
                while (*p == ' ') p++;
                // Distinguish "no digits" from a real value: garbage renders
                // as "0" from the kernel's atoi, which is a legal render —
                // callers compare against known-good values, not absence.
                return s_atoi(p, -999999);
            }
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return -999999;
}

static void report(const char* marker, int ok) {
    if (ok) {
        sys_print("[PROCSYS] ", 0x0A);
        sys_print(marker, 0x0A);
        sys_print("\n", 0x0A);
    } else {
        sys_print("[PROCSYS] FAIL ", 0x0C);
        sys_print(marker, 0x0C);
        sys_print("\n", 0x0C);
    }
}

void _start(void) {
    sys_print("[PROCSYS] start\n", 0x0E);

    char buf[1024];

    // ---- 1. /proc/sys/all: the index must list every knob with ranges ----
    {
        int n = read_all("/proc/sys/all", buf, sizeof(buf));
        int ok = n > 0 &&
                 s_str(buf, "zombie_reap_ms") != 0 &&
                 s_str(buf, "load_window") != 0 &&
                 s_str(buf, "futex_sweep_div") != 0 &&
                 s_str(buf, "(min") != 0;
        report("index-ok", ok);
    }

    // ---- 2. zombie_reap_ms: default 15000, write 20000, read back, restore ----
    {
        int ok = 1;
        read_all("/proc/sys/zombie_reap_ms", buf, sizeof(buf));
        if (knob_value(buf, "zombie_reap_ms") != 15000) ok = 0;
        if (write_all("/proc/sys/zombie_reap_ms", "20000") < 0) ok = 0;
        read_all("/proc/sys/zombie_reap_ms", buf, sizeof(buf));
        if (knob_value(buf, "zombie_reap_ms") != 20000) ok = 0;
        if (write_all("/proc/sys/zombie_reap_ms", "15000") < 0) ok = 0;
        read_all("/proc/sys/zombie_reap_ms", buf, sizeof(buf));
        if (knob_value(buf, "zombie_reap_ms") != 15000) ok = 0;
        report("readback-ok", ok);
    }

    // ---- 3. Out-of-range write must be rejected, knob unchanged ----
    {
        int ok = 1;
        // 999 < min (1000): the handler must refuse and leave the knob at
        // 15000. atoi("999") = 999 -> range check fails -> write returns -1.
        if (write_all("/proc/sys/zombie_reap_ms", "999") >= 0) ok = 0;
        read_all("/proc/sys/zombie_reap_ms", buf, sizeof(buf));
        if (knob_value(buf, "zombie_reap_ms") != 15000) ok = 0;
        // 200000 > max (120000): refused as well.
        if (write_all("/proc/sys/zombie_reap_ms", "200000") >= 0) ok = 0;
        read_all("/proc/sys/zombie_reap_ms", buf, sizeof(buf));
        if (knob_value(buf, "zombie_reap_ms") != 15000) ok = 0;
        report("reject-ok", ok);
    }

    // ---- 4. Garbage (non-numeric) write must be rejected ----
    {
        int ok = 1;
        // atoi("hello") = 0 -> below min -> refused (a reject is the ONLY
        // safe outcome: clamping garbage to the min would silently retune
        // the kernel on a typo).
        if (write_all("/proc/sys/load_window", "hello") >= 0) ok = 0;
        read_all("/proc/sys/load_window", buf, sizeof(buf));
        if (knob_value(buf, "load_window") != 50) ok = 0;
        report("garbage-ok", ok);
    }

    // ---- 5. load_window: legal change + restore ----
    {
        int ok = 1;
        if (write_all("/proc/sys/load_window", "120") < 0) ok = 0;
        read_all("/proc/sys/load_window", buf, sizeof(buf));
        if (knob_value(buf, "load_window") != 120) ok = 0;
        if (write_all("/proc/sys/load_window", "50") < 0) ok = 0;
        read_all("/proc/sys/load_window", buf, sizeof(buf));
        if (knob_value(buf, "load_window") != 50) ok = 0;
        report("loadwindow-ok", ok);
    }

    // ---- 6. futex_sweep_div: legal change + restore ----
    {
        int ok = 1;
        if (write_all("/proc/sys/futex_sweep_div", "5") < 0) ok = 0;
        read_all("/proc/sys/futex_sweep_div", buf, sizeof(buf));
        if (knob_value(buf, "futex_sweep_div") != 5) ok = 0;
        if (write_all("/proc/sys/futex_sweep_div", "1") < 0) ok = 0;
        read_all("/proc/sys/futex_sweep_div", buf, sizeof(buf));
        if (knob_value(buf, "futex_sweep_div") != 1) ok = 0;
        report("sweepdiv-ok", ok);
    }

    // ---- 7. nice: default 0, raise denied (EPERM), lower allowed, clamp ----
    // POSIX setpriority semantics: an unprivileged caller may LOWER its own
    // priority (raise the nice value) but never RAISE it (lower the value) —
    // not even on itself. The app runs as uid 1000, so this also asserts the
    // kernel's EPERM path end-to-end.
    {
        int ok = 1;
        int me = sys_getpid();
        if (sys_getnice(me) != 0) ok = 0;          // fresh default
        if (sys_nice(me, -5) != -2) ok = 0;        // raise DENIED (-2 EPERM)
        if (sys_getnice(me) != 0) ok = 0;          // unchanged by the denial
        if (sys_nice(me, 5) != 0) ok = 0;          // lower prio: allowed
        if (sys_getnice(me) != 5) ok = 0;
        if (sys_nice(me, 25) != 0) ok = 0;         // clamps to 19, lowering
        if (sys_getnice(me) != 19) ok = 0;
        if (sys_nice(me, 3) != -2) ok = 0;         // raise again: denied
        if (sys_getnice(me) != 19) ok = 0;
        report("nice-ok", ok);
    }

    // ---- 8. /proc/self/status exposes Nice: and CPU%: ----
    {
        int n = read_all("/proc/self/status", buf, sizeof(buf));
        int ok = n > 0 &&
                 s_str(buf, "Nice:") != 0 &&
                 s_str(buf, "CPU%:") != 0;
        report("cpu-ok", ok);
    }

    sys_print("[PROCSYS] done\n", 0x0A);
    sys_exit();
}
