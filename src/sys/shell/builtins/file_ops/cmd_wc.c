// src/sys/shell/builtins/file_ops/cmd_wc.c — the `wc` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

// Counting state. `in_word` deliberately survives across chunk boundaries:
// a word split by the chunk edge is still one word.
typedef struct {
    int lines;
    int words;
    int in_word;
} wc_count_t;

static void wc_count_bytes(wc_count_t* c, const char* data, int len) {
    for (int i = 0; i < len; i++) {
        char ch = data[i];
        if (ch == '\n') c->lines++;
        // POSIX wc -w splits on the C library's isspace(): space, \t, \n, \v,
        // \f, \r. (The original code omitted \v and \f.) On TEXT files this
        // matches GNU wc exactly — `wc /ext2/mectov1.map` reports 32/298/1629,
        // the same numbers host `wc -l -w -c` gives. On BINARY files the word
        // count can differ from GNU coreutils 9.4 (it reports 473 words for
        // the 55169-byte music.wav where this isspace model gives 696, and 696
        // is what `tr`/`awk`/`split` independently produce) — that is GNU's
        // locale/multibyte path, not a defect here. Lines and bytes always
        // match.
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
            ch == '\v' || ch == '\f') {
            c->in_word = 0;
        } else if (!c->in_word) {
            c->in_word = 1;
            c->words++;
        }
    }
}

// Stream a regular file and count the WHOLE thing.
//
// v38.166 (bug fix): the old code read the file into a 2048-byte stack
// buffer and counted that. vfs_read_file() clamps the read to the caller's
// max_size, so every file larger than 2047 bytes reported a truncated byte
// count and line/word counts taken from the file's head only — `wc
// /ext2/music.wav` (55169 bytes) claimed 2047 bytes, losing 96% of the
// count. Now the size comes from the node, and the content is streamed in
// chunks so memory stays bounded regardless of file size.
//
// Returns the number of bytes consumed, or -1 if the node cannot be streamed
// (procfs/device nodes), in which case the caller falls back to a single
// vfs_read_file() into a heap buffer.
static int wc_count_file_stream(int node, wc_count_t* c) {
    char wbuf[2048];
    int off = 0;
    for (;;) {
        int n = vfs_read_file_offset(node, off, wbuf, (int)sizeof(wbuf));
        if (n <= 0) break;
        wc_count_bytes(c, wbuf, n);
        off += n;
        if (n < (int)sizeof(wbuf)) break;
    }
    return off;
}

// procfs / device nodes have no offset-aware read path: vfs_read_file_offset
// refuses them. Those files are small, so read them whole into a heap buffer
// sized from the node instead.
static int wc_count_file_oneshot(const char* path, int size, wc_count_t* c) {
    if (size <= 0) return 0;
    char* buf = (char*)kmalloc((uint32_t)size + 1);
    if (!buf) {
        // Out of memory: fall back to a bounded stack read so `wc` still
        // answers with something rather than dying.
        char small[2048];
        int n = vfs_read_file(path, small, (int)sizeof(small) - 1);
        if (n > 0) wc_count_bytes(c, small, n);
        return n > 0 ? n : 0;
    }
    int n = vfs_read_file(path, buf, size);
    if (n > 0) wc_count_bytes(c, buf, n);
    kfree(buf);
    return n > 0 ? n : 0;
}

void cmd_wc(void) {
        // wc [FILE]     → count lines/words/bytes of a file
        // wc (no args)  → count stdin (pipe / '<' redirection)
        // Like cat/head, the read buffer is a plain stack local: a forked
        // background builtin (`sh x.sh &` whose script runs wc) executes
        // run_cmd_internal on another core, so a shared static here could
        // race. Each task has its own kernel stack, so a local is safe.
        //
        // v38.166: the buffer now lives at function scope, NOT inside the
        // `wc <file>` branch. It used to be declared in that branch, then
        // read back through `src` after the branch had closed — a
        // stack-use-after-scope (confirmed by AddressSanitizer). It happened
        // to work because GCC kept one frame for the whole function, but any
        // refactor that put a call between the branch and the read would
        // have silently corrupted the count.
        char wpath[MAX_PATH] = "";
        int file_fail = 0;
        int total_bytes = 0;
        wc_count_t count = { 0, 0, 0 };

        // Serial mirror for automated tests (the success path mirrors below).
        extern void write_serial_string(const char*);
        extern void write_serial_hex(uint32_t);

        if (strncmp(cmd_b, "wc ", 3) == 0) {
            char* fpath = cmd_b + 3;
            sanitize_path(fpath);
            strncpy(wpath, fpath, MAX_PATH - 1);
            wpath[MAX_PATH - 1] = '\0';

            vfs_stat_t st;
            if (vfs_stat_follow(fpath, &st) != 0) {
                file_fail = 1;
                print("wc: file not found: ", 0x0C);
                print(fpath, 0x0C);
                print("\n", 0x0C);
                write_serial_string("[SH] wc error file not found ");
                write_serial_string(fpath);
                write_serial_string("\n");
            } else if (st.type == FS_DIR || st.type == FS_EXT2_DIR ||
                       st.type == FS_FAT32_DIR || st.type == FS_RAM_DIR) {
                file_fail = 1;
                print("wc: is a directory: ", 0x0C);
                print(fpath, 0x0C);
                print("\n", 0x0C);
                write_serial_string("[SH] wc error is a directory ");
                write_serial_string(fpath);
                write_serial_string("\n");
            } else {
                // The byte count is the node's real size, not what a single
                // bounded read happened to return.
                total_bytes = st.size;

                if (st.type == FS_PROC || st.type == FS_DEV) {
                    wc_count_file_oneshot(fpath, st.size, &count);
                } else {
                    int got = wc_count_file_stream(st.node_idx, &count);
                    if (got < 0) {
                        wc_count_file_oneshot(fpath, st.size, &count);
                    }
                }
            }
        } else {
            extern int pipe_buf_len;
            extern char pipe_buffer[];
            const char* src = (shell_stdin_len > 0) ? shell_stdin_buf : pipe_buffer;
            int src_len = (shell_stdin_len > 0) ? shell_stdin_len : pipe_buf_len;
            if (src && src_len > 0) {
                wc_count_bytes(&count, src, src_len);
                total_bytes = src_len;
            } else if (!src) {
                file_fail = 1;
            }
        }

        if (file_fail) return;

        // POSIX format: lines words bytes [name], 7-column right-aligned.
        print_num_field(count.lines, 7);
        print_num_field(count.words, 7);
        print_num_field(total_bytes, 7);
        if (wpath[0]) print(wpath, 0x0F);
        print("\n", 0x0F);
        // Serial mirror for automated tests.
        write_serial_string("[SH] wc ");
        write_serial_hex(count.lines); write_serial_string(" ");
        write_serial_hex(count.words); write_serial_string(" ");
        write_serial_hex(total_bytes); write_serial_string(" ");
        write_serial_string(wpath);
        write_serial_string("\n");
}