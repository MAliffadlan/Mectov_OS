// src/sys/shell/builtins/file_ops/cmd_grep_arg.c — the `grep_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_grep_arg(void) {
        char* pattern = cmd_b + 5;
        while (*pattern == ' ') pattern++;
        
        extern int pipe_buf_len;
        extern char pipe_buffer[];
        // With '<' redirection, grep consumes the stdin buffer instead.
        const char* src = (shell_stdin_len > 0) ? shell_stdin_buf : pipe_buffer;
        int src_len = (shell_stdin_len > 0) ? shell_stdin_len : pipe_buf_len;
        if (src_len > 0) {
            int i = 0;
            char line[256];
            int line_len = 0;
            while (i < src_len) {
                char c = src[i++];
                if (c == '\n' || c == '\r') {
                    line[line_len] = '\0';
                    if (line_len > 0) {
                        if (strstr_custom(line, pattern) >= 0) {
                            print(line, 0x0A); // Print matching lines in green
                            print("\n", 0x0F);
                            // Serial mirror for stdin-redirected grep so tests
                            // can see matches without the IPC channel.
                            if (shell_stdin_len > 0) {
                                extern void write_serial_string(const char*);
                                write_serial_string("[SH] grep: ");
                                write_serial_string(line);
                                write_serial_string("\n");
                            }
                        }
                    }
                    line_len = 0;
                } else {
                    if (line_len < 255) {
                        line[line_len++] = c;
                    }
                }
            }
            if (line_len > 0) {
                line[line_len] = '\0';
                if (strstr_custom(line, pattern) >= 0) {
                    print(line, 0x0A);
                    print("\n", 0x0F);
                }
            }
        } else {
            print("grep: no input\n", 0x0C);
        }
}
