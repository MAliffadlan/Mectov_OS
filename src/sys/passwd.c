// passwd.c — System account password backed by /etc/passwd (MECTOVFS).
//
// Since v38.52 the file stores "<salt_hex>:<sha256(salt_hex||pw)_hex>" — an
// 8-byte random salt (kernel CSPRNG) plus the SHA-256 digest, so the disk
// image no longer contains the plaintext. Legacy plaintext files (written by
// older builds) still verify until the user runs `passwd` again, and a
// missing/empty file falls back to PASSWD_DEFAULT — the fallback default
// lives in exactly one place (passwd.h). Both the login screen
// (src/gui/login.c) and the shell `passwd` command
// (src/sys/shell/builtins/env_cmds/cmd_passwd.c) go through these helpers.
#include "../include/passwd.h"
#include "../include/vfs.h"
#include "../include/utils.h"
#include "../include/sha256.h"
#include "../include/entropy.h"
#include "../include/serial.h"

#define SALT_BYTES 8
#define SALT_HEX   (SALT_BYTES * 2)   // 16
#define HASH_HEX   64                 // sha256
#define LINE_LEN   (SALT_HEX + 1 + HASH_HEX)   // 81

// Read the passwd file into buf (NUL-terminated, trailing newline/space
// stripped). Returns the trimmed length, or 0 for missing/empty.
static int read_passwd_file(char* buf, int max) {
    int n = vfs_read_file(PASSWD_PATH, buf, max);
    if (n <= 0) { buf[0] = '\0'; return 0; }
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
        buf[--n] = '\0';
    return n;
}

// First ':' in buf, or NULL (no strchr in this libc).
static char* find_colon(char* s) {
    while (*s) { if (*s == ':') return s; s++; }
    return NULL;
}

static void to_hex(const uint8_t* in, uint32_t n, char* out) {
    static const char* hx = "0123456789abcdef";
    for (uint32_t i = 0; i < n; i++) {
        out[i * 2]     = hx[in[i] >> 4];
        out[i * 2 + 1] = hx[in[i] & 0xF];
    }
    out[n * 2] = '\0';
}

// digest = sha256(salt_hex || pw)
static void hash_with_salt(const char* salt_hex, const char* pw, uint8_t digest[32]) {
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, salt_hex, (uint32_t)strlen(salt_hex));
    sha256_update(&c, pw, (uint32_t)strlen(pw));
    sha256_final(&c, digest);
}

int sys_verify_password(const char* pw) {
    char buf[LINE_LEN + 2];
    int n = read_passwd_file(buf, (int)sizeof(buf) - 1);
    if (n <= 0) {
        // Missing or empty file: fall back to the default. Unreachable in
        // practice since passwd_ensure_initialized() writes the salted hash
        // at first login and the delete guard (v38.53) keeps non-root from
        // removing /etc/passwd afterwards — kept for fresh-disk robustness.
        return strcmp(pw, PASSWD_DEFAULT) == 0;
    }

    char* colon = find_colon(buf);
    if (colon == NULL) {
        // Legacy plaintext file from a pre-v38.52 build.
        return strcmp(pw, buf) == 0;
    }
    *colon = '\0';
    const char* salt_hex = buf;
    const char* stored   = colon + 1;
    if (strlen(salt_hex) != SALT_HEX || strlen(stored) != HASH_HEX) return 0;

    uint8_t digest[32];
    hash_with_salt(salt_hex, pw, digest);
    char hx[HASH_HEX + 1];
    to_hex(digest, 32, hx);
    return strcmp(hx, stored) == 0;
}

int sys_set_password(const char* pw) {
    int len = 0;
    while (pw[len] && len < PASSWD_MAX_LEN) len++;
    if (len == 0) return -1;

    uint8_t salt[SALT_BYTES];
    if (get_random_bytes(salt, SALT_BYTES) != 0) return -1;  // entropy seeded at boot

    char salt_hex[SALT_HEX + 1];
    to_hex(salt, SALT_BYTES, salt_hex);

    uint8_t digest[32];
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, salt_hex, SALT_HEX);
    sha256_update(&c, pw, (uint32_t)len);
    sha256_final(&c, digest);
    char hash_hex[HASH_HEX + 1];
    to_hex(digest, 32, hash_hex);

    char line[LINE_LEN + 1];
    memcpy(line, salt_hex, SALT_HEX);
    line[SALT_HEX] = ':';
    memcpy(line + SALT_HEX + 1, hash_hex, HASH_HEX);
    line[LINE_LEN] = '\0';

    if (vfs_get_node("/etc") < 0) {
        if (vfs_mkdir("/etc") < 0 && vfs_get_node("/etc") < 0) return -1;
    }
    if (vfs_get_node(PASSWD_PATH) < 0) {
        if (vfs_create_file(PASSWD_PATH) < 0) return -1;
    }
    return vfs_write_file(PASSWD_PATH, line, LINE_LEN);
}

// v38.53 auth-bypass fix: materialize the default password as a real salted
// hash file at first login. Previously a MISSING /etc/passwd silently
// re-armed the plaintext PASSWD_DEFAULT fallback — and since root-owned dirs
// are 0777 (deliberate in this single-user OS), any app could delete the
// file and log in with the hardcoded default. Now: first login writes the
// hash file, and afterwards the VFS protected-path guard blocks non-root
// from deleting/renaming it. Call once from the login screen before the
// first verification.
void passwd_ensure_initialized(void) {
    static int ensured = 0;
    if (ensured) return;
    ensured = 1;
    char buf[LINE_LEN + 2];
    if (read_passwd_file(buf, (int)sizeof(buf) - 1) > 0) return; // already there
    if (sys_set_password(PASSWD_DEFAULT) == LINE_LEN) {
        write_serial_string("[PASSWD] default salted hash written to ");
        write_serial_string(PASSWD_PATH);
        write_serial_string("\n");
    }
}
