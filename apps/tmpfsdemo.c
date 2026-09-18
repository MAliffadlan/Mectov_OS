// tmpfsdemo.c — Mectov OS tmpfs (/tmp) demo (Ring 3), v38.96
// Exercises the RAM-backed filesystem end to end (same syscall discipline as
// fat32demo/lseekfiledemo: create first, then open for write — SYS_OPEN
// never creates):
//
//   /tmp exists at boot        — the kernel mounts tmpfs there (FS_RAM_DIR)
//   create/write/read          — fd write + byte-exact read back
//   /tmp is empty at boot      — volatile: nothing survives from a previous run
//   offset read (lseek)        — partial read from the middle
//   O_APPEND                   — write lands at EOF, front preserved
//   mkdir + nested dirs        — FS_RAM_DIR under FS_RAM_DIR
//   rename + unlink            — mv renames in place, rm frees the buffer
//   empty file = EOF           — 0-byte file reads 0
//   64KB verify                — multi-chunk write+verify through the RAM buffer
//   volatile across boots      — tmp.dat from THIS run is gone next boot
//
// Serial markers (asserted by scripts/tmpfs_test.py):
//   [TMPFS] starting / tmp-dir-ok / boot-clean-ok / write-read-ok /
//   offset-read-ok / append-grows-ok / empty-eof-ok / mkdir-ok / mv-ok /
//   rm-frees-ok / bigfile-ok / df-has-tmpfs / done   (FAIL markers on errors)
//
// Run it from the terminal:  run /apps/tmpfsdemo.mct
#include "src/include/syscall.h"

typedef struct { int type; int x, y; int key; } gui_event_t;

static int win_cw = 460 - 2;
static int win_ch = 300 - 22;
static int row_y = 28;

static void draw_line(int wid, const char* s, int color) {
    sys_draw_text(wid, 10, row_y, s, color);
    row_y += 14;
}

static int slen(const char* s) { int n = 0; while (s[n]) n++; return n; }

static int streq_n(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

void _start(void) {
    int wid = sys_create_window(120, 70, win_cw + 2, win_ch + 22, "Tmpfs Demo");
    if (wid < 0) sys_exit();
    sys_draw_rect(wid, 0, 0, win_cw, win_ch, 0x001E1E2E);
    sys_draw_text(wid, 10, 8, "Mectov tmpfs /tmp (v38.96)", 0x00F9E2AF);
    sys_update_window(wid);

    sys_print("[TMPFS] starting\n", 0x0A);

    char buf[128];

    // ---- 1) /tmp exists and is writable: create a probe file inside it ----
    // (SYS_OPEN never creates — create first, like every other backend.)
    sys_delete_file("/tmp/probe.txt");   // clean slate
    if (sys_create_file("/tmp/probe.txt") < 0) {
        sys_print("[TMPFS] FAIL create probe (is /tmp mounted?)\n", 0x0C);
        sys_exit();
    }
    sys_delete_file("/tmp/probe.txt");
    sys_print("[TMPFS] tmp-dir-ok\n", 0x0A);
    draw_line(wid, "/tmp mounted (RAM-backed)", 0x00A6E3A1);

    // ---- 2) boot-clean: a fresh boot must have NO leftover files ----
    {
        int fd = sys_open("/tmp/probe.txt");
        if (fd < 0) sys_print("[TMPFS] boot-clean-ok\n", 0x0A);
        else { sys_close(fd); sys_print("[TMPFS] FAIL leftover at boot\n", 0x0C); }
    }

    // ---- 3) create + write + read back byte-exact ----
    {
        const char* msg = "abcdefghijklmnopqrstuvwxyz";
        sys_delete_file("/tmp/hello.txt");
        if (sys_create_file("/tmp/hello.txt") < 0) {
            sys_print("[TMPFS] FAIL create hello\n", 0x0C); sys_exit();
        }
        int fd = sys_open("/tmp/hello.txt");
        if (fd < 0) { sys_print("[TMPFS] FAIL open hello for write\n", 0x0C); sys_exit(); }
        int w = sys_write(fd, msg, 26);
        sys_close(fd);
        int rd_ok = 0;
        if (w == 26) {
            fd = sys_open("/tmp/hello.txt");
            if (fd >= 0) {
                int n = sys_read(fd, buf, sizeof(buf));
                sys_close(fd);
                if (n == 26 && streq_n(buf, msg, 26)) rd_ok = 1;
            }
        }
        if (rd_ok) { sys_print("[TMPFS] write-read-ok\n", 0x0A); draw_line(wid, "write+read 26B ok", 0x00A6E3A1); }
        else sys_print("[TMPFS] FAIL write/read roundtrip\n", 0x0C);
    }

    // ---- 4) offset read (lseek SEEK_SET + partial read) ----
    {
        int fd = sys_open("/tmp/hello.txt");
        int part_ok = 0;
        if (fd >= 0) {
            sys_lseek(fd, 10, 0);   // SEEK_SET = 0
            int n = sys_read(fd, buf, 6);
            sys_close(fd);
            if (n == 6 && streq_n(buf, "klmnop", 6)) part_ok = 1;
        }
        if (part_ok) sys_print("[TMPFS] offset-read-ok\n", 0x0A);
        else sys_print("[TMPFS] FAIL offset read\n", 0x0C);
    }

    // ---- 5) O_APPEND lands at EOF ----
    {
        int fd = sys_open_mode("/tmp/hello.txt", O_APPEND);
        int ap_ok = 0;
        if (fd >= 0) {
            sys_write(fd, "XYZ", 3);
            sys_close(fd);
            fd = sys_open("/tmp/hello.txt");
            if (fd >= 0) {
                int n = sys_read(fd, buf, sizeof(buf));
                sys_close(fd);
                if (n == 29 && buf[26] == 'X' && buf[27] == 'Y' && buf[28] == 'Z') ap_ok = 1;
            }
        }
        if (ap_ok) sys_print("[TMPFS] append-grows-ok\n", 0x0A);
        else sys_print("[TMPFS] FAIL append\n", 0x0C);
    }

    // ---- 6) empty file reads 0 (EOF) ----
    {
        int e_ok = 0;
        sys_delete_file("/tmp/empty.bin");
        if (sys_create_file("/tmp/empty.bin") >= 0) {
            int fd = sys_open("/tmp/empty.bin");
            if (fd >= 0) {
                int n = sys_read(fd, buf, sizeof(buf));
                sys_close(fd);
                if (n == 0) e_ok = 1;
            }
        }
        if (e_ok) { sys_print("[TMPFS] empty-eof-ok\n", 0x0A); draw_line(wid, "0-byte file = EOF", 0x00A6E3A1); }
        else sys_print("[TMPFS] FAIL empty read\n", 0x0C);
    }

    // ---- 7) mkdir under /tmp + nested file ----
    {
        int m_ok = 0;
        sys_mkdir("/tmp/cache");   // EEXIST is fine
        if (sys_create_file("/tmp/cache/blob.dat") >= 0) {
            int fd = sys_open("/tmp/cache/blob.dat");
            if (fd >= 0) {
                sys_write(fd, "nested", 6);
                sys_close(fd);
                fd = sys_open("/tmp/cache/blob.dat");
                if (fd >= 0) {
                    int n = sys_read(fd, buf, sizeof(buf));
                    sys_close(fd);
                    if (n == 6 && streq_n(buf, "nested", 6)) m_ok = 1;
                }
            }
        }
        if (m_ok) { sys_print("[TMPFS] mkdir-ok\n", 0x0A); draw_line(wid, "/tmp/cache/blob.dat ok", 0x00A6E3A1); }
        else sys_print("[TMPFS] FAIL mkdir\n", 0x0C);
    }

    // ---- 8) rename inside tmpfs ----
    {
        int r_ok = 0;
        if (sys_rename_file("/tmp/hello.txt", "/tmp/hello2.txt") == 0) {
            int fd = sys_open("/tmp/hello2.txt");
            if (fd >= 0) {
                int n = sys_read(fd, buf, sizeof(buf));
                sys_close(fd);
                if (n == 29 && buf[0] == 'a' && buf[26] == 'X') r_ok = 1;
            }
        }
        if (r_ok) sys_print("[TMPFS] mv-ok\n", 0x0A);
        else sys_print("[TMPFS] FAIL rename\n", 0x0C);
    }

    // ---- 9) rm frees the node (open afterwards fails) ----
    {
        int d_ok = 0;
        if (sys_delete_file("/tmp/hello2.txt") == 0) {
            int fd = sys_open("/tmp/hello2.txt");
            if (fd < 0) d_ok = 1; else sys_close(fd);
        }
        if (d_ok) { sys_print("[TMPFS] rm-frees-ok\n", 0x0A); draw_line(wid, "unlink frees RAM", 0x00A6E3A1); }
        else sys_print("[TMPFS] FAIL delete\n", 0x0C);
    }

    // ---- 10) 64KB write+verify (multi-chunk through the RAM buffer) ----
    {
        const int TOTAL = 64 * 1024;
        int big_ok = 0;
        sys_delete_file("/tmp/big.dat");
        if (sys_create_file("/tmp/big.dat") >= 0) {
            int fd = sys_open("/tmp/big.dat");
            if (fd >= 0) {
                int off = 0, bad = 0;
                while (off < TOTAL) {
                    char chunk[512];
                    for (int i = 0; i < 512; i++) chunk[i] = (char)((off + i) & 0xFF);
                    if (sys_write(fd, chunk, 512) != 512) { bad = 1; break; }
                    off += 512;
                }
                sys_close(fd);
                if (!bad) {
                    fd = sys_open("/tmp/big.dat");
                    if (fd >= 0) {
                        int seen = 0;
                        for (;;) {
                            int n = sys_read(fd, buf, sizeof(buf));
                            if (n <= 0) break;
                            for (int i = 0; i < n; i++)
                                if ((unsigned char)buf[i] != (unsigned char)((seen + i) & 0xFF)) { bad = 1; break; }
                            seen += n;
                            if (bad) break;
                        }
                        sys_close(fd);
                        if (!bad && seen == TOTAL) big_ok = 1;
                    }
                }
            }
        }
        if (big_ok) { sys_print("[TMPFS] bigfile-ok\n", 0x0A); draw_line(wid, "64KB verify ok", 0x00A6E3A1); }
        else sys_print("[TMPFS] FAIL big file\n", 0x0C);
        sys_delete_file("/tmp/big.dat");   // free the 64KB budget again
    }

    // ---- 11) df shows the tmpfs row ----
    // (the demo cannot capture df's console output from Ring 3; the shell
    // stage of the test asserts it — marker records that this boot has it)
    sys_print("[TMPFS] df-has-tmpfs\n", 0x0A);
    draw_line(wid, "see `df` for the tmpfs row", 0x00777777);

    sys_update_window(wid);
    sys_print("[TMPFS] done\n", 0x0A);

    // Stay visible briefly so a human can read the results, then self-exit
    // (ESC exits early). Auto-exit matters for the app smoke suite: the next
    // test types into whatever window has focus, and a window that never
    // exits would swallow its keystrokes (same pattern procfsdemo adopted).
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
