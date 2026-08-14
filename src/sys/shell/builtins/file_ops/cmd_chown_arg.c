// src/sys/shell/builtins/file_ops/cmd_chown_arg.c — the `chown` shell command.
// v38.23: `chown <uid> <gid> <path>` — root-only (POSIX: only root may
// transfer ownership). The mode is unchanged; chown alone never alters
// permissions.
#include "../../shell_internal.h"

void cmd_chown_arg(void) {
        char* arg = cmd_b + 6;
        while (*arg == ' ') arg++;

        char* uid_s = arg;
        char* gid_s = arg;
        while (*gid_s && *gid_s != ' ') gid_s++;
        if (*gid_s) { *gid_s = '\0'; gid_s++; }
        while (*gid_s == ' ') gid_s++;
        char* path = gid_s;
        while (*path && *path != ' ') path++;
        if (*path) { *path = '\0'; path++; }
        while (*path == ' ') path++;

        if (uid_s[0] == '\0' || gid_s[0] == '\0' || path[0] == '\0') {
            print("chown: usage: chown <uid> <gid> <path>\n", 0x0C);
            return;
        }

        int uid = atoi(uid_s);
        int gid = atoi(gid_s);

        if (vfs_chown(path, (uint16_t)uid, (uint16_t)gid) == 0) {
            print("chown: ", 0x0A);
            print(path, 0x0A);
            print(" -> uid=", 0x0A);
            p_int(uid, 0x0A);
            print(" gid=", 0x0A);
            p_int(gid, 0x0A);
            print("\n", 0x0A);
        } else {
            print("chown: failed (file not found, or not root)\n", 0x0C);
        }
}
