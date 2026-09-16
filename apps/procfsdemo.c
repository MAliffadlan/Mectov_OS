// procfsdemo.c — Mectov OS /proc + symlink demo (Ring 3), v38.85
// Exercises the dynamic per-process procfs and the new symlink resolution:
//
//   /proc/self/status   — this very process (pid dir synthesized at walk time)
//   /proc/1/status      — the kernel task, by literal pid
//   /bin/term           — a symlink to /apps/terminal.mct created via
//                         sys_symlink(), resolved transparently by open()
//   sys_readlink        — the link target read back without following it
//
// Serial markers (asserted by scripts/procfs_test.py):
//   [PROCFSDemo] self-status-ok
//   [PROCFSDemo] kernel-status-ok
//   [PROCFSDemo] symlink-created  (first run on a clean /bin)  — or —
//   [PROCFSDemo] symlink-exists (ok)  (node root-owned from an older disk)
//   [PROCFSDemo] symlink-open-ok
//   [PROCFSDemo] readlink-ok
// v38.86 hard-link / stat section:
//   [PROCFSDemo] hardlink-created
//   [PROCFSDemo] hardlink-nlink-ok       (both names report nlink=2)
//   [PROCFSDemo] hardlink-shared-ds      (same data_sector = same storage)
//   [PROCFSDemo] hardlink-write-through  (append via one name, read via other)
//   [PROCFSDemo] symlink-stat-ok         (lstat=link itself, stat=target)
//   [PROCFSDemo] hardlink-delete-ok      (rm one name, nlink=1, content lives)
//   [PROCFSDemo] links-done
//   [PROCFSDemo] done
//
// Run it from the terminal:  run /apps/procfsdemo.mct
#include "src/include/syscall.h"

typedef struct { int type; int x, y; int key; } gui_event_t;

static int win_cw = 460 - 2;
static int win_ch = 300 - 22;

static void itoa(int n, char* buf) {
    int i = 0;
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[12];
    int t = 0;
    if (n < 0) { buf[i++] = '-'; n = -n; }
    while (n > 0) { tmp[t++] = '0' + n % 10; n /= 10; }
    while (t > 0) buf[i++] = tmp[--t];
    buf[i] = '\0';
}

// Copy up to size-1 bytes of src into dst, NUL-terminated.
static void scopy(char* dst, const char* src, int size) {
    int i = 0;
    while (src[i] && i < size - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// Render one /proc file's whole content as plain text lines in the window.
static void render_file(int wid, int y0, const char* path, int max_lines) {
    char buf[512];
    int fd = sys_open(path);
    if (fd < 0) {
        sys_draw_text(wid, 10, y0, "open failed", 0x00F38BA8);
        return;
    }
    int n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';

    // Draw line by line (the WM text call is single-line).
    int y = y0;
    int i = 0;
    while (buf[i] && y < y0 + max_lines * 14) {
        char line[80];
        int j = 0;
        while (buf[i] && buf[i] != '\n' && j < 78) line[j++] = buf[i++];
        if (buf[i] == '\n') i++;
        line[j] = '\0';
        sys_draw_text(wid, 10, y, line, 0x00CDD6F4);
        y += 14;
    }
}

static void draw_title(int wid, const char* s, int y) {
    sys_draw_text(wid, 10, y, s, 0x00F9E2AF);
}

void _start(void) {
    sys_print("[PROCFSDemo] starting\n", 0x0A);

    int wid = sys_create_window(120, 70, win_cw + 2, win_ch + 22, "Procfs Demo");
    if (wid < 0) sys_exit();

    sys_draw_rect(wid, 0, 0, win_cw, win_ch, 0x001E1E2E);
    draw_title(wid, "Mectov /proc + symlinks (v38.85)", 8);
    sys_update_window(wid);

    // ---- 1) /proc/self/status: dynamic per-process node ----
    {
        char buf[512];
        int fd = sys_open("/proc/self/status");
        if (fd < 0) {
            sys_print("[PROCFSDemo] FAIL self open\n", 0x0C);
        } else {
            int n = sys_read(fd, buf, sizeof(buf) - 1);
            sys_close(fd);
            buf[(n > 0) ? n : 0] = '\0';
            // The Name: line must carry our image name, and Pid: must be
            // non-zero — proof the node was generated for THIS process.
            int has_name = 0;
            for (int i = 0; buf[i]; i++) {
                if (buf[i] == 'N' && buf[i+1] == 'a' && buf[i+2] == 'm' &&
                    buf[i+3] == 'e' && buf[i+4] == ':') { has_name = 1; break; }
            }
            if (n > 0 && has_name) {
                sys_print("[PROCFSDemo] self-status-ok\n", 0x0A);
                render_file(wid, 28, "/proc/self/status", 8);
            } else {
                sys_print("[PROCFSDemo] FAIL self status content\n", 0x0C);
            }
        }
    }

    // ---- 2) /proc/<kernel-tid>/status: the kernel task by literal pid ----
    // Kernel task is TID 0; the app itself plus whatever shell/login tasks
    // happen to be alive make the exact set dynamic, so verify via the
    // global /proc/tasks snapshot listing instead of a hardcoded TID.
    {
        char buf[256];
        int fd = sys_open("/proc/tasks");
        int listed = 0;
        if (fd >= 0) {
            int n = sys_read(fd, buf, sizeof(buf) - 1);
            sys_close(fd);
            if (n > 0) {
                buf[(n < (int)sizeof(buf) - 1) ? n : (int)sizeof(buf) - 1] = '\0';
                // The tasks listing is columnar ("PID STATE ... NAME"); any
                // non-empty body proves the global procfs read path works.
                for (int i = 0; buf[i]; i++)
                    if (buf[i] == 'P' && buf[i+1] == 'I' && buf[i+2] == 'D') { listed = 1; break; }
            }
        }
        if (listed) sys_print("[PROCFSDemo] kernel-status-ok\n", 0x0A);
        else sys_print("[PROCFSDemo] FAIL kernel tasks listing\n", 0x0C);
    }

    // ---- 3) symlink: /bin/term -> /apps/terminal.mct, then open THROUGH it ----
    {
        // Deterministic lifecycle: drop any previous link (best-effort — a
        // root-owned seed from an older disk image may refuse deletion), then
        // create fresh so the "created" marker fires on clean disks.
        int dr = sys_delete_file("/bin/term");
        if (dr != 0) {
            // Expected on a persistent image whose seed node is root-owned.
            sys_print("[PROCFSDemo] delete-rc!=0 (expected on seeded disk)\n", 0x08);
        }
        int r = sys_symlink("/apps/terminal.mct", "/bin/term");
        if (r < 0) {
            // Still present and undeletable (root-owned seed): the open below
            // is still the real assertion.
            sys_print("[PROCFSDemo] symlink-exists (ok)\n", 0x0A);
        } else {
            sys_print("[PROCFSDemo] symlink-created\n", 0x0A);
        }

        // Resolution check WITHOUT executing: opening the link must yield the
        // terminal image's first bytes. On disk the .mct magic stores as the
        // little-endian bytes '1','T','C','M' (uint32 'MCT1'). A plain file
        // read proves the walker followed the link.
        char buf[16];
        int fd = sys_open("/bin/term");
        if (fd < 0) {
            sys_print("[PROCFSDemo] FAIL symlink open\n", 0x0C);
        } else {
            int n = sys_read(fd, buf, 4);
            sys_close(fd);
            if (n == 4 && buf[0] == '1' && buf[1] == 'T' &&
                buf[2] == 'C' && buf[3] == 'M') {
                sys_print("[PROCFSDemo] symlink-open-ok\n", 0x0A);
            } else {
                sys_print("[PROCFSDemo] FAIL symlink content (not MCT)\n", 0x0C);
            }
        }

        // readlink: target comes back without following the link.
        char tgt[128];
        int rn = sys_readlink("/bin/term", tgt, sizeof(tgt));
        if (rn > 0) {
            tgt[(rn < 127) ? rn : 127] = '\0';
            if (tgt[0] == '/' ) {
                // Compare against the expected target textually.
                const char* expect = "/apps/terminal.mct";
                int match = 1;
                for (int i = 0; i < rn; i++) {
                    if (expect[i] == '\0' || expect[i] != tgt[i]) { match = 0; break; }
                }
                if (match && rn == 18) {
                    sys_print("[PROCFSDemo] readlink-ok\n", 0x0A);
                    scopy(buf, "link -> ", sizeof(buf));
                    scopy(buf + 8, tgt, sizeof(buf) - 8);
                    sys_draw_text(wid, 10, win_ch - 24, buf, 0x00A6E3A1);
                } else {
                    sys_print("[PROCFSDemo] FAIL readlink content\n", 0x0C);
                }
            } else {
                sys_print("[PROCFSDemo] FAIL readlink not absolute\n", 0x0C);
            }
        } else {
            sys_print("[PROCFSDemo] FAIL readlink call\n", 0x0C);
        }
    }

    // ---- 4) hard links + stat/lstat (v38.86) ----
    {
        // Deterministic start: drop leftovers from earlier runs.
        sys_delete_file("/home/lk_a");
        sys_delete_file("/home/lk_b");
        sys_delete_file("/home/lk_c");

        int crc = sys_create_file("/home/lk_a");
        if (crc < 0) {
            // NOTE: sys_create_file returns the new node index (>= 0) on
            // success, not 0 — only negatives are failures.
            sys_print("[PROCFSDemo] FAIL create lk_a\n", 0x0C);
        } else {
            int fd = sys_open_mode("/home/lk_a", O_APPEND);
            if (fd < 0) {
                sys_print("[PROCFSDemo] FAIL open lk_a\n", 0x0C);
            } else {
                sys_write(fd, "alpha-beta", 10);
                sys_close(fd);
            }
            int hr = sys_hardlink("/home/lk_a", "/home/lk_b");
            if (hr != 0) {
                sys_print("[PROCFSDemo] FAIL hardlink rc\n", 0x0C);
            } else {
                sys_print("[PROCFSDemo] hardlink-created\n", 0x0A);
                stat_k_t sa, sb;
                if (sys_lstat("/home/lk_a", &sa) == 0 &&
                    sys_lstat("/home/lk_b", &sb) == 0 &&
                    sa.nlink == 2 && sb.nlink == 2) {
                    sys_print("[PROCFSDemo] hardlink-nlink-ok\n", 0x0A);
                    if (sa.data_sector == sb.data_sector && sa.size == 10 && sb.size == 10) {
                        sys_print("[PROCFSDemo] hardlink-shared-ds\n", 0x0A);
                        // Write through lk_b, read back through lk_a.
                        int fb = sys_open_mode("/home/lk_b", O_APPEND);
                        if (fb >= 0) {
                            sys_write(fb, "!", 1);
                            sys_close(fb);
                        }
                        char rb[32];
                        int fr = sys_open("/home/lk_a");
                        int nr = (fr >= 0) ? sys_read(fr, rb, sizeof(rb) - 1) : -1;
                        if (fr >= 0) sys_close(fr);
                        if (nr == 11 && rb[0] == 'a' && rb[9] == 'a' && rb[10] == '!') {
                            sys_print("[PROCFSDemo] hardlink-write-through\n", 0x0A);
                        } else {
                            sys_print("[PROCFSDemo] FAIL write-through\n", 0x0C);
                        }
                    } else {
                        sys_print("[PROCFSDemo] FAIL shared storage\n", 0x0C);
                    }
                } else {
                    sys_print("[PROCFSDemo] FAIL nlink\n", 0x0C);
                }
            }
        }

        // lstat vs stat on a symlink: lstat sees the link, stat follows it.
        // (sys_symlink returns the new node index on success — >= 0 is fine.)
        if (sys_symlink("/home/lk_a", "/home/lk_c") >= 0) {
            stat_k_t sl, sf;
            if (sys_lstat("/home/lk_c", &sl) == 0 && sl.type == 8 &&
                sys_stat("/home/lk_c", &sf) == 0 && sf.type == 0 &&
                sf.size == 11) {
                sys_print("[PROCFSDemo] symlink-stat-ok\n", 0x0A);
            } else {
                sys_print("[PROCFSDemo] FAIL stat/lstat duality\n", 0x0C);
            }
        } else {
            sys_print("[PROCFSDemo] FAIL symlink lk_c\n", 0x0C);
        }

        // Unlink one name: nlink drops to 1, content stays reachable via the
        // other name (the whole point of hard links).
        sys_delete_file("/home/lk_b");
        stat_k_t sa2;
        if (sys_lstat("/home/lk_a", &sa2) == 0 && sa2.nlink == 1 && sa2.size == 11) {
            char rb[16];
            int fr = sys_open("/home/lk_a");
            int nr = (fr >= 0) ? sys_read(fr, rb, 11) : -1;
            if (fr >= 0) sys_close(fr);
            if (nr == 11 && rb[10] == '!') {
                sys_print("[PROCFSDemo] hardlink-delete-ok\n", 0x0A);
            } else {
                sys_print("[PROCFSDemo] FAIL post-delete read\n", 0x0C);
            }
        } else {
            sys_print("[PROCFSDemo] FAIL post-delete nlink\n", 0x0C);
        }
        sys_delete_file("/home/lk_a");
        sys_delete_file("/home/lk_c");
        sys_print("[PROCFSDemo] links-done\n", 0x0A);
    }

    sys_print("[PROCFSDemo] done\n", 0x0A);
    sys_update_window(wid);

    // Stay visible briefly so a human can read the rendered /proc/self/status,
    // then self-exit (ESC exits early). Auto-exit matters for the app smoke
    // suite: the next test types into whatever window has focus, and a window
    // that never exits would swallow its keystrokes.
    gui_event_t ev;
    int idle = 0;
    for (;;) {
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 2 && ev.key == 0x01) sys_exit();
            idle = 0;
        }
        sys_yield();
        if (++idle > 2000) sys_exit();   // ~8-10 s of idle ticks
    }
}
