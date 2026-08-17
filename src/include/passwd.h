#ifndef PASSWD_H
#define PASSWD_H

// System account password, stored in /etc/passwd (MECTOVFS) with a
// hardcoded fallback for fresh disks and missing/empty files.
#define PASSWD_DEFAULT   "mectov123"
#define PASSWD_MAX_LEN   31   // matches the login input buffer ceiling
#define PASSWD_PATH      "/etc/passwd"

// Verify pw against /etc/passwd. The stored file is "<salt_hex>:<sha256
// hex>" since v38.52; a legacy plaintext file (no ':') and a missing/empty
// file (PASSWD_DEFAULT) are still verified so an existing disk keeps working
// until the next `passwd`. Returns 1 on match, 0 otherwise.
int sys_verify_password(const char* pw);

// Store pw in /etc/passwd as salt+sha256, creating /etc and the file if
// needed. Rejects an empty password. Returns 0 on success, -1 on failure.
int sys_set_password(const char* pw);

#endif
