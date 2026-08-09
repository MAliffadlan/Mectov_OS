// src/sys/shell/builtins/file_ops/cmd_cd.c — the `cd` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_cd(void) {
        if (strcmp(cmd_b, "cd") == 0) {
            set_current_dir(0); // Go to root
        } else {
            char* dirpath = cmd_b + 3;
            sanitize_path(dirpath);
            if (strcmp(dirpath, "-") == 0) {
                // cd - : jump back to the previous directory (OLDPWD).
                if (shell_oldpwd[0] == '\0') {
                    print("cd: OLDPWD not set\n", 0x0C);
                    extern void write_serial_string(const char*);
                    write_serial_string("[SH] cd -: OLDPWD not set\n");
                } else {
                    int node = vfs_get_node(shell_oldpwd);
                    if (node < 0 || !vfs_is_dir(node)) {
                        print("cd: previous directory no longer exists\n", 0x0C);
                    } else {
                        set_current_dir(node);
                        print(shell_oldpwd, 0x0F);
                        print("\n", 0x0F);
                        extern void write_serial_string(const char*);
                        write_serial_string("[SH] cd -: ");
                        write_serial_string(shell_oldpwd);
                        write_serial_string("\n");
                    }
                }
            } else {
                int node = vfs_get_node(dirpath);
                if (node < 0 || !vfs_is_dir(node)) {
                    print("cd: directory not found: ", 0x0C);
                    print(dirpath, 0x0C);
                    print("\n", 0x0C);
                } else {
                    // Remember where we were so `cd -` can come back.
                    vfs_get_abs_path(get_current_dir(), shell_oldpwd, MAX_PATH);
                    set_current_dir(node);
                    extern void write_serial_string(const char*);
                    write_serial_string("[SH] cd: ");
                    write_serial_string(dirpath);
                    write_serial_string(" (oldpwd saved)\n");
                }
            }
        }
}
