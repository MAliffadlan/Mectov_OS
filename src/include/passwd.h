#ifndef PASSWD_H
#define PASSWD_H

// System account password, stored in /etc/passwd (MECTOVFS) with a
// hardcoded fallback for fresh disks and missing/empty files.
#define PASSWD_DEFAULT   "mectov123"
#define PASSWD_MAX_LEN   31   // matches the login input buffer ceiling
#define PASSWD_PATH      "/etc/passwd"

// Read the current password into out (NUL-terminated, at most max-1 chars).
// Returns 0 when it came from /etc/passwd, 1 when the file was missing or
// empty and PASSWD_DEFAULT was used instead.
int sys_get_password(char* out, int max);

// Store pw in /etc/passwd, creating /etc and the file if needed. Rejects an
// empty password. Returns 0 on success, -1 on failure.
int sys_set_password(const char* pw);

#endif
