// hardening_test.c — entropy/CSPRNG + password-hash regression (v38.52) and
// the Ring 3 syscall-hardening regression set (Batch A).
// Headless; logs to the serial console via SYS_PRINT (VGA + serial).
//
// Checks from Ring 3:
//   1. SYS_GETRANDOM (117) succeeds — proves the kernel ChaCha8 DRBG is
//      seeded at boot (unseeded -> -1).
//   2. Two GETRANDOM calls return fresh, non-zero, different bytes.
//   3. /dev/random reads (via fd) return real bytes that differ across reads
//      (the old fixed-seed LCG returned the same sequence every boot).
//   4. After `passwd`, /etc/passwd is "<16-hex salt>:<64-hex sha256>" — 81
//      chars, valid hex, and the plaintext password is NOT in the file.
//   5. SYS_VMM_ALLOC/FREE are denied: with -m 512 the identity map covers
//      0x10000000, so the old range check let Ring 3 free kernel frames and
//      remap the kernel's own identity PTEs.
//   6. SYS_HARDLINK: linking /etc/passwd is denied (non-root), linking a
//      caller-owned file still works.
//   7. SYS_TCSETPGRP: a plain app cannot capture the terminal (scanout +
//      keystrokes); only the shell host / root may.
//   8. SYS_LOAD_LIBRARY rejects an MCT whose 32-bit size sum wraps
//      (code_size=0xFFFFFFF0 + data_size=0x40 used to slip every bound and
//      make the kernel memcpy ~4 GB at CPL 0).
//   9. Window ownership + shm teardown: a forked child opens a window and
//      attaches shm; the parent must not read the child's events or draw
//      into its window, and SIGKILL of the shm-holding child must not wedge
//      the kernel (shm_task_exit ran with IF=1 under task_lock).
//  10. SYS_EXEC rejects the same wrapped-size MCT, and an entry point
//      outside the loaded code.
//
// Run:  run /apps/hardening_test.mct   (boot with -m 512)
#include "src/include/syscall.h"

static int fails = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { sys_print(msg, 0x0C); fails++; } \
} while (0)

static int all_zero(const char* b, int n) {
    for (int i = 0; i < n; i++) if (b[i]) return 0;
    return 1;
}

static int same_bytes(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int is_hex(const char* s, int n) {
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return 0;
    }
    return 1;
}

// ---- helpers for the syscall-hardening checks ----

typedef struct {
    unsigned int magic;
    unsigned int entry;
    unsigned int code_size;
    unsigned int data_size;
} mct_hdr_t;

#define MCT_MAGIC_ 0x4D435431u
#define HARDEN_IPC_FILE "/tmp/harden_ipc.bin"

// Overwrite `path` with `len` bytes from `data` (create if missing).
static int put_bytes(const char* path, const char* data, int len) {
    sys_create_file(path);   // EEXIST is fine
    int fd = sys_open(path);
    if (fd < 0) return -1;
    int w = (len > 0) ? sys_write(fd, data, len) : 0;
    sys_close(fd);
    return (w == len) ? 0 : -1;
}

static int get_bytes(const char* path, char* buf, int len) {
    int fd = sys_open(path);
    if (fd < 0) return -1;
    int r = sys_read(fd, buf, len);
    sys_close(fd);
    return r;
}

// header + `code_bytes` NOPs of "code", with arbitrary (possibly hostile)
// header fields.
static void make_mct(char* out, unsigned int entry, unsigned int code,
                     unsigned int data, int code_bytes) {
    mct_hdr_t* h = (mct_hdr_t*)out;
    h->magic = MCT_MAGIC_;
    h->entry = entry;
    h->code_size = code;
    h->data_size = data;
    for (int i = 0; i < code_bytes; i++) out[16 + i] = (char)0x90;  // NOP
}

void _start(void) {
    sys_print("hardening_test: start\n", 0x0F);

    // ---- 1+2. SYS_GETRANDOM: seeded, fresh, non-zero ----
    char a[32], b[32];
    int r1 = syscall(SYS_GETRANDOM, (int)(uintptr_t)a, 32, 0);
    CHECK(r1 == 0, "hardening_test: [FAIL] SYS_GETRANDOM returned error (unseeded?)\n");
    if (r1 == 0) {
        CHECK(!all_zero(a, 32), "hardening_test: [FAIL] GETRANDOM output all zero\n");
        int r2 = syscall(SYS_GETRANDOM, (int)(uintptr_t)b, 32, 0);
        CHECK(r2 == 0, "hardening_test: [FAIL] second SYS_GETRANDOM error\n");
        CHECK(r2 != 0 || !same_bytes(a, b, 32),
              "hardening_test: [FAIL] GETRANDOM repeated the same bytes\n");
        sys_print("hardening_test: [OK] SYS_GETRANDOM fresh random bytes\n", 0x0A);
    }

    // ---- 3. /dev/random via fd ----
    int fd = sys_open("/dev/random");
    CHECK(fd >= 0, "hardening_test: [FAIL] open /dev/random\n");
    if (fd >= 0) {
        char c1[16], c2[16];
        int n1 = sys_read(fd, c1, 16);
        int n2 = sys_read(fd, c2, 16);
        sys_close(fd);
        CHECK(n1 == 16 && n2 == 16,
              "hardening_test: [FAIL] /dev/random short read\n");
        if (n1 == 16 && n2 == 16) {
            CHECK(!all_zero(c1, 16) && !same_bytes(c1, c2, 16),
                  "hardening_test: [FAIL] /dev/random not random across reads\n");
            sys_print("hardening_test: [OK] /dev/random fresh reads\n", 0x0A);
        }
    }

    // ---- 4. /etc/passwd is salted + hashed (no plaintext) ----
    int pfd = sys_open("/etc/passwd");
    CHECK(pfd >= 0, "hardening_test: [FAIL] open /etc/passwd\n");
    if (pfd >= 0) {
        char pw[128];
        int n = sys_read(pfd, pw, (int)sizeof(pw) - 1);
        sys_close(pfd);
        if (n >= 0) pw[n] = '\0';
        CHECK(n == 81, "hardening_test: [FAIL] passwd file length != 81\n");
        if (n == 81) {
            CHECK(pw[16] == ':', "hardening_test: [FAIL] no ':' after salt hex\n");
            CHECK(is_hex(pw, 16) && is_hex(pw + 17, 64),
                  "hardening_test: [FAIL] salt/hash not hex\n");
            // The plaintext password must NOT appear anywhere in the file.
            const char* pl = "hunter2";
            int found = 0;
            for (int i = 0; i < n - 7; i++) {
                int m = 1;
                for (int j = 0; j < 7; j++) if (pw[i + j] != pl[j]) { m = 0; break; }
                if (m) { found = 1; break; }
            }
            CHECK(!found, "hardening_test: [FAIL] plaintext password in /etc/passwd\n");
            sys_print("hardening_test: [OK] /etc/passwd salted+hashed, no plaintext\n", 0x0A);
        }
    }

    // ---- 5. SYS_VMM_FREE / SYS_VMM_ALLOC must be denied (Batch A, C1) ----
    // The suite boots -m 512, so the boot identity map covers 0x10000000
    // (256 MB): the PTE there is kernel-owned. Freeing it drops a kernel
    // frame; ALLOC remaps the kernel's own identity view of that physical
    // page from this task's context. Both must be denied for every address
    // (no in-tree app uses these two numbers; only the syscall fuzzer pokes
    // them).
    {
        int v1 = syscall(SYS_VMM_FREE, 0x10000000, 0, 0);
        int v2 = syscall(SYS_VMM_ALLOC, 0x10000000, 0, 0);
        CHECK(v1 != 0,
              "hardening_test: [FAIL] SYS_VMM_FREE freed a kernel (identity) frame\n");
        CHECK(v2 == 0,
              "hardening_test: [FAIL] SYS_VMM_ALLOC remapped a kernel (identity) VA\n");
        if (v1 != 0 && v2 == 0)
            sys_print("hardening_test: [OK] SYS_VMM_FREE/ALLOC denied\n", 0x0A);
    }

    // ---- 6. Hard links: ownership + protected paths (Batch A, C4) ----
    {
        int denied = syscall(SYS_HARDLINK, (int)(uintptr_t)"/etc/passwd",
                             (int)(uintptr_t)"/hl_evil", 0);
        CHECK(denied != 0,
              "hardening_test: [FAIL] hardlink(/etc/passwd) succeeded as user\n");

        // Positive control: a file the caller owns must still link.
        int ok = -1;
        if (put_bytes("/hl_src", "HL", 2) == 0) {
            ok = syscall(SYS_HARDLINK, (int)(uintptr_t)"/hl_src",
                         (int)(uintptr_t)"/hl_dst", 0);
        }
        CHECK(ok == 0,
              "hardening_test: [FAIL] hardlink of caller-owned file denied (regression)\n");
        if (denied != 0 && ok == 0)
            sys_print("hardening_test: [OK] hardlink: /etc/passwd denied, own file ok\n", 0x0A);
    }

    // ---- 7. tcsetpgrp: plain apps cannot capture the terminal (C3) ----
    {
        int own = sys_getpgrp();
        int r = sys_tcsetpgrp(1, own);
        CHECK(r != 0,
              "hardening_test: [FAIL] tcsetpgrp let a plain app claim the terminal\n");
        if (r != 0)
            sys_print("hardening_test: [OK] tcsetpgrp denied for non-shell app\n", 0x0A);
    }

    // ---- 8. Loader: wrapped MCT size sum via SYS_LOAD_LIBRARY (C2) ----
    // code_size=0xFFFFFFF0 + data_size=0x40 wraps the 32-bit sum to 0x30,
    // slipping past every bound; the kernel then memcpy'd ~4 GB at CPL 0.
    {
        char blob[16 + 32];
        make_mct(blob, 0, 0xFFFFFFF0u, 0x40, 32);
        int wrote = put_bytes("/ll_bad.mct", blob, (int)sizeof(blob));
        CHECK(wrote == 0, "hardening_test: [FAIL] could not write /ll_bad.mct\n");
        if (wrote == 0) {
            int base = syscall(SYS_LOAD_LIBRARY, (int)(uintptr_t)"/ll_bad.mct", 0, 0);
            CHECK(base == 0,
                  "hardening_test: [FAIL] SYS_LOAD_LIBRARY accepted a wrapped-size MCT\n");
            if (base == 0)
                sys_print("hardening_test: [OK] wrapped-size MCT refused by the lib loader\n", 0x0A);
        }
    }

    // ---- 9. Window ownership + shm-holding child killed (C5/C7) ----
    // A forked child opens a window and attaches shm, then parks. The parent
    // must not read the child's events or draw into its window; SIGKILLing
    // the child must run the full teardown (including shm_task_exit, which
    // used to `sti` under task_lock) and still reap it.
    {
        int pid = sys_fork();
        if (pid == 0) {
            int cwid = sys_create_window(420, 120, 60, 40, "harden_child");
            int sid = sys_shmget(0x48415244u, 4096);
            void* sva = (void*)(uintptr_t)0;
            if (sid > 0) {
                sva = sys_shmat(sid);
                sys_shmat(sid);   // double attach must be idempotent
            }
            int rec[3];
            rec[0] = cwid;
            rec[1] = sid;
            rec[2] = (int)(uintptr_t)sva;
            put_bytes(HARDEN_IPC_FILE, (const char*)rec, (int)sizeof(rec));
            for (int i = 0; i < 600; i++) sys_sleep(1);   // park ~6 s
            sys_exit();
        }
        if (pid < 0) {
            CHECK(0, "hardening_test: [FAIL] fork failed\n");
        } else {
            int rec[3] = { -1, -1, 0 };
            for (int tries = 0; tries < 300; tries++) {
                int n = get_bytes(HARDEN_IPC_FILE, (char*)rec, (int)sizeof(rec));
                if (n == (int)sizeof(rec) && rec[0] > 0) break;
                rec[0] = -1;
                sys_sleep(1);
            }
            CHECK(rec[0] > 0, "hardening_test: [FAIL] child window never reported\n");
            if (rec[0] > 0) {
                char ev[64];
                int foreign = sys_get_event(rec[0], ev);
                CHECK(foreign == -1,
                      "hardening_test: [FAIL] GET_EVENT read a foreign window's queue\n");
                int mywid = sys_create_window(500, 120, 60, 40, "harden_parent");
                CHECK(mywid > 0, "hardening_test: [FAIL] own window creation failed\n");
                if (mywid > 0) {
                    int ownev = sys_get_event(mywid, ev);
                    CHECK(ownev >= 0,
                          "hardening_test: [FAIL] GET_EVENT refused the caller's own window\n");
                }
                int dr = syscall5(SYS_DRAW_RECT, rec[0], 1, 1, (8 << 16) | 8, 0xFF0000);
                CHECK(dr != 0,
                      "hardening_test: [FAIL] DRAW_RECT into a foreign window accepted\n");
                if (foreign == -1 && dr != 0)
                    sys_print("hardening_test: [OK] window ownership enforced (events + draw)\n", 0x0A);
            }
            sys_kill(pid, 9);
            int st = 0;
            int reaped = sys_waitpid(pid, &st, 0);
            CHECK(reaped == pid,
                  "hardening_test: [FAIL] SIGKILLed shm-holding child not reaped\n");
            if (reaped == pid)
                sys_print("hardening_test: [OK] shm-holding child killed + reaped\n", 0x0A);
        }
    }

    // ---- 10. Loader: wrapped MCT refused by SYS_EXEC (C2) ----
    {
        char blob[16 + 32];
        make_mct(blob, 0, 0xFFFFFFF0u, 0x40, 32);
        int wrote = put_bytes("/ll_wrapexec.mct", blob, (int)sizeof(blob));
        CHECK(wrote == 0, "hardening_test: [FAIL] could not write /ll_wrapexec.mct\n");
        if (wrote == 0) {
            sys_chmod("/ll_wrapexec.mct", 0755);
            int r = sys_exec("/ll_wrapexec.mct", "");
            CHECK(r != 0, "hardening_test: [FAIL] SYS_EXEC accepted a wrapped-size MCT\n");
            if (r != 0)
                sys_print("hardening_test: [OK] wrapped-size MCT refused by SYS_EXEC\n", 0x0A);
        }
    }

    // ---- 11. Loader: entry outside the code refused by SYS_EXEC (C2) ----
    {
        char blob[16 + 256];
        make_mct(blob, 0x00100000u, 0x100, 0x100, 256);
        int wrote = put_bytes("/ll_entry.mct", blob, (int)sizeof(blob));
        CHECK(wrote == 0, "hardening_test: [FAIL] could not write /ll_entry.mct\n");
        if (wrote == 0) {
            sys_chmod("/ll_entry.mct", 0755);
            int r = sys_exec("/ll_entry.mct", "");
            CHECK(r != 0, "hardening_test: [FAIL] SYS_EXEC accepted an out-of-range entry\n");
            if (r != 0)
                sys_print("hardening_test: [OK] out-of-range MCT entry refused by SYS_EXEC\n", 0x0A);
        }
    }

    if (fails == 0) sys_print("ALL TESTS PASSED\n", 0x0A);
    else            sys_print("hardening_test: TESTS FAILED\n", 0x0C);
    sys_exit();
}
