// src/sys/shell/builtins/text_tools/cmd_uniq.c — the `uniq` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_uniq(void) {
        int count_mode = 0;
        char* arg = cmd_b + 4;
        while (*arg == ' ') arg++;
        if (strncmp(arg, "-c", 2) == 0) { count_mode = 1; arg += 2; while (*arg == ' ') arg++; }
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
                    print("uniq: file not found: ", 0x0C);
                    print(fpath, 0x0C);
                    print("\n", 0x0C);
                    kfree(fbuf); fbuf = NULL;
                } else { src = fbuf; src_len = sz; }
            } else {
                print("uniq: out of memory\n", 0x0C);
            }
        } else {
            extern int pipe_buf_len;
            extern char pipe_buffer[];
            src = (shell_stdin_len > 0) ? shell_stdin_buf : pipe_buffer;
            src_len = (shell_stdin_len > 0) ? shell_stdin_len : pipe_buf_len;
        }
        if (src) {
            // Heap-allocated: see the sort command above (stack budget).
            sh_line_t* lines = (sh_line_t*)kmalloc(sizeof(sh_line_t) * 256);
            if (lines) {
                int n = split_lines(src, src_len, lines, 256);
                extern void write_serial_string(const char*);
                for (int i = 0; i < n; ) {
                    int j = i + 1;
                    while (j < n && line_cmp(src, &lines[i], &lines[j]) == 0) j++;
                    int cnt = j - i;
                    if (count_mode) {
                        p_int(cnt, 0x0F); print(" ", 0x0F);
                    }
                    write_serial_string("[SH] uniq ");
                    if (count_mode) { char nb[8]; int ni = 0, t = cnt; do { nb[ni++] = '0' + t % 10; t /= 10; } while (t); while (ni) write_serial(nb[--ni]); write_serial(' '); }
                    for (int k = 0; k < lines[i].len; k++) {
                        char cc = src[lines[i].off + k];
                        p_char(cc, 0x0F);
                        if (cc == '\n' || cc == '\t') write_serial_string(" ");
                        else write_serial(cc);
                    }
                    write_serial_string("\n");
                    print("\n", 0x0F);
                    i = j;
                }
                if (n == 0) print("\n", 0x0F);
                kfree(lines);
            } else {
                print("uniq: out of memory\n", 0x0C);
            }
        }
        if (fbuf) kfree(fbuf);
}
