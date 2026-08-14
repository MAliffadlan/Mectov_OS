// src/sys/shell/builtins/file_ops/cmd_chmod_arg.c — the `chmod` shell command.
// v38.23: `chmod <octal> <path>` — the octal mode is parsed POSIX-style
// (e.g. 755, 644, 600) and enforced by vfs_check_perm on every Ring 3
// open/read/write/delete. Only the file's owner (or root) may change it.
#include "../../shell_internal.h"

// Parse a 1-4 digit octal string ("755", "644", "0", "7777") into a mode.
static int parse_octal(const char* s) {
    int v = 0;
    for (int i = 0; s[i] && i < 4; i++) {
        char c = s[i];
        if (c < '0' || c > '7') return -1;
        v = (v << 3) | (c - '0');
    }
    return v;
}

void cmd_chmod_arg(void) {
        char* arg = cmd_b + 6;
        while (*arg == ' ') arg++;

        char* mode_s = arg;
        char* path = arg;
        while (*path && *path != ' ') path++;
        if (*path) { *path = '\0'; path++; }
        while (*path == ' ') path++;

        if (mode_s[0] == '\0' || path[0] == '\0') {
            print("chmod: usage: chmod <octal-mode> <path>\n", 0x0C);
            return;
        }

        int mode = parse_octal(mode_s);
        if (mode < 0) {
            print("chmod: invalid mode: ", 0x0C);
            print(mode_s, 0x0C);
            print(" (use octal, e.g. 755 or 644)\n", 0x0C);
            return;
        }

        if (vfs_chmod(path, (uint16_t)mode) == 0) {
            print("chmod: ", 0x0A);
            print(path, 0x0A);
            print(" -> ", 0x0A);
            print(mode_s, 0x0A);
            print("\n", 0x0A);
        } else {
            print("chmod: failed (file not found, or not owner)\n", 0x0C);
        }
}
