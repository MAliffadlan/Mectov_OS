// src/sys/shell/builtins/file_ops/cmd_ls_arg.c — the `ls_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
// v38.23: `ls -l` long format (mode string, uid/gid, size, name) via
// vfs_list_dir_long; `ls -l [path]` and `ls [path]` both supported.
#include "../../shell_internal.h"

void cmd_ls_arg(void) {
        char* arg = cmd_b + 3;
        sanitize_path(arg);

        int long_fmt = 0;
        if (arg[0] == '-' && arg[1] == 'l') {
            long_fmt = 1;
            write_serial_string("[SH] ls -l\n");
            // Path may follow the flag: `ls -l /ext2`
            char* p = arg + 2;
            while (*p == ' ') p++;
            if (p[0] != '\0') {
                sanitize_path(p);
                char* path = p;
                int node = vfs_get_node(path);
                if (node < 0 || !vfs_is_dir(node)) {
                    print("ls: directory not found: ", 0x0C);
                    print(path, 0x0C);
                    print("\n", 0x0C);
                } else if (long_fmt) {
                    vfs_list_dir_long(node, print);
                } else {
                    vfs_list_dir(node, print);
                }
                return;
            }
        }

        if (arg[0] == '\0' || (long_fmt && arg[2] == '\0')) {
            // `ls` / `ls -l` on the current directory
            if (long_fmt) {
                vfs_list_dir_long(get_current_dir(), print);
            } else {
                vfs_list_dir(get_current_dir(), print);
            }
            return;
        }

        char* dirpath = arg;
        if (long_fmt) dirpath = arg + 2;   // `-l <path>`
        int node = vfs_get_node(dirpath);
        if (node < 0 || !vfs_is_dir(node)) {
            print("ls: directory not found: ", 0x0C);
            print(dirpath, 0x0C);
            print("\n", 0x0C);
        } else if (long_fmt) {
            vfs_list_dir_long(node, print);
        } else {
            vfs_list_dir(node, print);
        }
}
