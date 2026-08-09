// src/sys/shell/builtins/text_tools/cmd_sort.c — the `sort` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_sort(void) {
        char* arg = cmd_b + 4;
        while (*arg == ' ') arg++;
        const char* src = NULL;
        int src_len = 0;
        char* fbuf = NULL;
        if (*arg) {
            char fpath[MAX_PATH];
            strncpy(fpath, arg, MAX_PATH - 1);
            fpath[MAX_PATH - 1] = '\0';
            sanitize_path(fpath);
            fbuf = (char*)kmalloc(4096);
            if (fbuf) {
                int sz = vfs_read_file(fpath, fbuf, 4095);
                if (sz < 0) {
                    print("sort: file not found: ", 0x0C);
                    print(fpath, 0x0C);
                    print("\n", 0x0C);
                    kfree(fbuf); fbuf = NULL;
                } else { src = fbuf; src_len = sz; }
            } else {
                print("sort: out of memory\n", 0x0C);
            }
        } else {
            extern int pipe_buf_len;
            extern char pipe_buffer[];
            src = (shell_stdin_len > 0) ? shell_stdin_buf : pipe_buffer;
            src_len = (shell_stdin_len > 0) ? shell_stdin_len : pipe_buf_len;
        }
        if (src) {
            // Heap-allocated: 2KB+4KB of locals here would push run_cmd_internal's
            // frame past the 16KB kernel stack when loading an app after sort.
            sh_line_t* lines = (sh_line_t*)kmalloc(sizeof(sh_line_t) * 256);
            if (lines) {
                int n = split_lines(src, src_len, lines, 256);
                // Insertion sort (stable enough; tiny N from a 4KB pipe).
                for (int i = 1; i < n; i++) {
                    sh_line_t key = lines[i];
                    int j = i - 1;
                    while (j >= 0 && line_cmp(src, &lines[j], &key) > 0) {
                        lines[j + 1] = lines[j];
                        j--;
                    }
                    lines[j + 1] = key;
                }
                extern void write_serial_string(const char*);
                for (int i = 0; i < n; i++) {
                    write_serial_string("[SH] sort ");
                    for (int k = 0; k < lines[i].len; k++) {
                        char cc = src[lines[i].off + k];
                        p_char(cc, 0x0F);
                        if (cc == '\n' || cc == '\t') write_serial_string(" ");
                        else write_serial(cc);
                    }
                    write_serial_string("\n");
                    print("\n", 0x0F);
                }
                if (n == 0) print("\n", 0x0F);
                kfree(lines);
            } else {
                print("sort: out of memory\n", 0x0C);
            }
        }
        if (fbuf) kfree(fbuf);
}
