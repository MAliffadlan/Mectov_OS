// src/sys/shell/builtins/file_ops/cmd_mount.c — the `mount` shell command.
// v38.42: `mount <path> <fstype> <drive>` mounts a filesystem at an empty
// directory (the directory is created when missing); `mount` with no args
// lists the active mounts. Runs in the shell's kernel task (uid 0), so the
// root-only SYS_MOUNT check passes.
#include "../../shell_internal.h"

void cmd_mount(void) {
        char* arg = cmd_b + 5;
        while (*arg == ' ') arg++;

        if (*arg == '\0') {
                // List the active mounts through the serial log (the table
                // dump is one line per mount, prefixed "[MOUNT] ").
                print("mount: active mounts (see serial log)\n", 0x0E);
                mount_dump();
                return;
        }

        // <path> <fstype> <drive>
        char* path = arg;
        char* fstype = arg;
        while (*fstype && *fstype != ' ') fstype++;
        if (*fstype) { *fstype = '\0'; fstype++; }
        while (*fstype == ' ') fstype++;
        char* drive_s = fstype;
        while (*drive_s && *drive_s != ' ') drive_s++;
        if (*drive_s) { *drive_s = '\0'; drive_s++; }
        while (*drive_s == ' ') drive_s++;

        if (path[0] == '\0' || fstype[0] == '\0' || drive_s[0] == '\0') {
                print("mount: usage: mount <path> <ext2|fat32> <drive>\n", 0x0C);
                return;
        }

        int drive = 0;
        for (int i = 0; drive_s[i]; i++) {
                if (drive_s[i] < '0' || drive_s[i] > '9') {
                        print("mount: bad drive number\n", 0x0C);
                        return;
                }
                drive = drive * 10 + (drive_s[i] - '0');
        }

        if (vfs_mount_path(path, fstype, drive) == 0) {
                print("mounted ", 0x0A);
                print(fstype, 0x0A);
                print(" on ", 0x0A);
                print(path, 0x0A);
                print("\n", 0x0A);
        } else {
                print("mount: failed (unknown fstype, bad drive, or not an empty dir)\n", 0x0C);
        }
}
