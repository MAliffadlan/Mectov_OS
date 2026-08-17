// hardening_test.c — entropy/CSPRNG + password-hash regression (v38.52).
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
//
// Run:  run /apps/hardening_test.mct
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

    if (fails == 0) sys_print("ALL TESTS PASSED\n", 0x0A);
    else            sys_print("hardening_test: TESTS FAILED\n", 0x0C);
    sys_exit();
}
