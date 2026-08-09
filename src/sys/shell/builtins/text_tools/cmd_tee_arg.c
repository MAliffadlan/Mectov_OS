// src/sys/shell/builtins/text_tools/cmd_tee_arg.c — the `tee_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_tee_arg(void) {
        char* fpath = cmd_b + 4;
        sanitize_path(fpath);
        extern int pipe_buf_len;
        extern char pipe_buffer[];
        const char* src = (shell_stdin_len > 0) ? shell_stdin_buf : pipe_buffer;
        int src_len = (shell_stdin_len > 0) ? shell_stdin_len : pipe_buf_len;
        if (fpath[0] == '\0') {
            print("tee: usage: tee FILE (reads stdin)\n", 0x0C);
        } else if (src_len <= 0) {
            print("tee: no input\n", 0x0C);
        } else {
            // Ensure the target exists, then write (like '>' redirection).
            if (vfs_get_node(fpath) < 0) vfs_create_file(fpath);
            int wr = vfs_write_file(fpath, src, src_len);
            // Mirror to stdout too.
            for (int i = 0; i < src_len; i++) p_char(src[i], 0x0F);
            if (src_len > 0 && src[src_len - 1] != '\n') print("\n", 0x0F);
            extern void write_serial_string(const char*);
            extern void write_serial_hex(uint32_t);
            write_serial_string("[SH] tee ");
            write_serial_hex((uint32_t)wr);
            write_serial_string(" bytes -> ");
            write_serial_string(fpath);
            write_serial_string("\n");
        }
}
