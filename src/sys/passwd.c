// passwd.c — System account password backed by /etc/passwd (MECTOVFS).
//
// The OS is single-user, so this is one password string stored as plain
// text in a VFS file (no shadow file, no hashing — the disk image is the
// only storage and there is no threat model beyond "don't leave the
// default password"). Both the login screen (src/gui/login.c) and the
// shell `passwd` command (src/sys/shell.c) go through these helpers so
// the fallback default lives in exactly one place.
#include "../include/passwd.h"
#include "../include/vfs.h"
#include "../include/utils.h"

int sys_get_password(char* out, int max) {
    if (max <= 1) { out[0] = '\0'; return 1; }
    char buf[PASSWD_MAX_LEN + 2];
    int n = vfs_read_file(PASSWD_PATH, buf, (int)sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        // Tolerate a trailing newline (nano/echo writes) or spaces.
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
            buf[--n] = '\0';
        if (n > 0) {
            int c = (n < max - 1) ? n : max - 1;
            memcpy(out, buf, (uint32_t)c);
            out[c] = '\0';
            return 0;
        }
    }
    // Missing or empty file: fall back to the default.
    int d = 0;
    while (PASSWD_DEFAULT[d] && d < max - 1) { out[d] = PASSWD_DEFAULT[d]; d++; }
    out[d] = '\0';
    return 1;
}

int sys_set_password(const char* pw) {
    int len = 0;
    while (pw[len] && len < PASSWD_MAX_LEN) len++;
    if (len == 0) return -1;

    if (vfs_get_node("/etc") < 0) {
        if (vfs_mkdir("/etc") < 0 && vfs_get_node("/etc") < 0) return -1;
    }
    if (vfs_get_node(PASSWD_PATH) < 0) {
        if (vfs_create_file(PASSWD_PATH) < 0) return -1;
    }
    return vfs_write_file(PASSWD_PATH, pw, len);
}
