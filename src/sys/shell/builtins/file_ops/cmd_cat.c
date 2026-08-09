// src/sys/shell/builtins/file_ops/cmd_cat.c — the `cat` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_cat(void) {
        if (strcmp(cmd_b, "cat") == 0) {
            extern int pipe_buf_len;
            extern char pipe_buffer[];
            if (shell_stdin_len > 0) {
                print(shell_stdin_buf, 0x0F);
            } else if (pipe_buf_len > 0) {
                print(pipe_buffer, 0x0F);
                print("\n", 0x0F);
            } else {
                print("cat: no input\n", 0x0C);
            }
        } else {
            char* fpath = cmd_b + 4;
            int number_lines = 0;
            // cat -n FILE : number each output line (right-aligned, 6 cols).
            if (strncmp(fpath, "-n ", 3) == 0) {
                number_lines = 1;
                fpath += 3;
                while (*fpath == ' ') fpath++;
            }
            sanitize_path(fpath);
            char buf[2048];
            int sz = vfs_read_file(fpath, buf, 2047);
            if (sz < 0) {
                print("cat: file not found\n", 0x0C);
            } else {
                buf[sz] = '\0';
                // Stream the file so `cat -n` can prefix each line. For plain
                // `cat` this is byte-identical to the old print(buf).
                int line_no = 1, at_bol = 1;
                for (int ci = 0; ci < sz; ci++) {
                    if (number_lines && at_bol) {
                        print_num_field(line_no, 6);
                        at_bol = 0;
                    }
                    p_char(buf[ci], 0x0F);
                    if (buf[ci] == '\n') { line_no++; at_bol = 1; }
                }
                // Serial mirror so automated tests can verify file contents
                // (terminal output goes over IPC, not serial). Mirrors the
                // raw file bytes regardless of -n; numbered output is verified
                // end-to-end via `cat -n F | grep N` (grep mirrors matches).
                extern void write_serial_string(const char*);
                extern void write_serial_hex(uint32_t);
                write_serial_string("[SH] cat ");
                write_serial_hex(sz);
                write_serial_string(" bytes: ");
                for (int ci = 0; ci < sz && ci < 512; ci++) {
                    char cc = buf[ci];
                    if (cc == '\n') write_serial_string("\\n");
                    else write_serial(cc);
                }
                write_serial_string("\n");
                print("\n", 0x0F);
            }
        }
}
