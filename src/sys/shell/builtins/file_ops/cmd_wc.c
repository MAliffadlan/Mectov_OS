// src/sys/shell/builtins/file_ops/cmd_wc.c — the `wc` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_wc(void) {
        // wc [FILE]     → count lines/words/bytes of a file
        // wc (no args)  → count stdin (pipe / '<' redirection)
        // Like cat/head, the read buffer is a plain stack local: a forked
        // background builtin (`sh x.sh &` whose script runs wc) executes
        // run_cmd_internal on another core, so a shared static here could
        // race. Each task has its own kernel stack, so a local is safe.
        const char* src = NULL;
        int src_len = 0, file_fail = 0;
        char wpath[MAX_PATH] = "";
        if (strncmp(cmd_b, "wc ", 3) == 0) {
            char* fpath = cmd_b + 3;
            sanitize_path(fpath);
            char wbuf[2048];
            int sz = vfs_read_file(fpath, wbuf, 2047);
            if (sz < 0) {
                file_fail = 1;
                print("wc: file not found: ", 0x0C);
                print(fpath, 0x0C);
                print("\n", 0x0C);
            } else {
                src = wbuf;
                src_len = sz;
                strncpy(wpath, fpath, MAX_PATH - 1);
                wpath[MAX_PATH - 1] = '\0';
            }
        } else {
            extern int pipe_buf_len;
            extern char pipe_buffer[];
            src = (shell_stdin_len > 0) ? shell_stdin_buf : pipe_buffer;
            src_len = (shell_stdin_len > 0) ? shell_stdin_len : pipe_buf_len;
        }
        if (src) {
            int lines = 0, words = 0, in_word = 0;
            for (int i = 0; i < src_len; i++) {
                char c = src[i];
                if (c == '\n') lines++;
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    in_word = 0;
                } else if (!in_word) {
                    in_word = 1;
                    words++;
                }
            }
            // POSIX format: lines words bytes [name], 7-column right-aligned.
            print_num_field(lines, 7);
            print_num_field(words, 7);
            print_num_field(src_len, 7);
            if (wpath[0]) print(wpath, 0x0F);
            print("\n", 0x0F);
            // Serial mirror for automated tests.
            extern void write_serial_string(const char*);
            extern void write_serial_hex(uint32_t);
            write_serial_string("[SH] wc ");
            write_serial_hex(lines); write_serial_string(" ");
            write_serial_hex(words); write_serial_string(" ");
            write_serial_hex(src_len); write_serial_string(" ");
            write_serial_string(wpath);
            write_serial_string("\n");
        } else if (!file_fail) {
            print("wc: no input\n", 0x0C);
        }
}
