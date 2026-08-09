// src/sys/shell/builtins/file_ops/cmd_head.c — the `head` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_head(void) {
        // head [FILE]      → first 10 lines
        // head -n N [FILE] → first N lines
        // head (no args)   → read from pipe / '<' redirection like cat
        int lines = 10;
        char fpath[MAX_PATH];
        int have_file = 0;
        if (strncmp(cmd_b, "head -n ", 8) == 0) {
            char* p = cmd_b + 8;
            lines = atoi(p);
            if (lines < 1) lines = 1;
            while (*p >= '0' && *p <= '9') p++;
            while (*p == ' ') p++;
            if (*p) {
                strncpy(fpath, p, MAX_PATH - 1);
                fpath[MAX_PATH - 1] = '\0';
                have_file = 1;
            }
        } else if (strncmp(cmd_b, "head ", 5) == 0) {
            strncpy(fpath, cmd_b + 5, MAX_PATH - 1);
            fpath[MAX_PATH - 1] = '\0';
            have_file = 1;
        }
        if (have_file) {
            sanitize_path(fpath);
            char buf[2048];
            int sz = vfs_read_file(fpath, buf, 2047);
            if (sz < 0) {
                print("head: file not found: ", 0x0C);
                print(fpath, 0x0C);
                print("\n", 0x0C);
            } else {
                int nl = 0;
                // Serial mirror so automated tests can verify head's output
                // (terminal output goes over IPC, not serial). Mirrors the
                // bytes actually printed (after -n truncation), like cat's.
                extern void write_serial_string(const char*);
                extern void write_serial_hex(uint32_t);
                int printed = 0;
                for (int i = 0; i < sz && nl < lines; i++) {
                    char c = buf[i];
                    if (c == '\n') nl++;
                    p_char(c, 0x0F);
                    printed++;
                }
                write_serial_string("[SH] head ");
                write_serial_hex(printed);
                write_serial_string(" bytes: ");
                for (int i = 0; i < printed; i++) {
                    char cc = buf[i];
                    if (cc == '\n') write_serial_string("\\n");
                    else write_serial(cc);
                }
                write_serial_string("\n");
                if (printed > 0 && buf[printed - 1] != '\n') print("\n", 0x0F);
            }
        } else {
            // stdin (pipe / '<' redirection)
            extern int pipe_buf_len;
            extern char pipe_buffer[];
            const char* src = (shell_stdin_len > 0) ? shell_stdin_buf : pipe_buffer;
            int src_len = (shell_stdin_len > 0) ? shell_stdin_len : pipe_buf_len;
            if (src_len > 0) {
                int nl = 0;
                extern void write_serial_string(const char*);
                write_serial_string("[SH] head stdin: ");
                for (int i = 0; i < src_len && nl < lines; i++) {
                    char c = src[i];
                    if (c == '\n') nl++;
                    p_char(c, 0x0F);
                    if (c == '\n') write_serial_string("\\n");
                    else write_serial(c);
                }
                write_serial_string("\n");
                if (src_len > 0 && src[src_len - 1] != '\n') print("\n", 0x0F);
            } else {
                print("head: no input\n", 0x0C);
            }
        }
}
