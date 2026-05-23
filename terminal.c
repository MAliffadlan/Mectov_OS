#include "src/include/syscall.h"

typedef struct {
    int type;
    int x, y;
    int key;
} gui_event_t;

// --- Forward Declarations for Static Functions ---
static int my_strlen(const char* s);
static void my_strcpy(char* d, const char* s);
static int my_strcmp(const char* a, const char* b);
static int my_strncmp(const char* a, const char* b, int n);
static void term_putchar(char c2, uint8_t color);
static void term_print(const char* s, uint8_t color);
static void draw_terminal(int wid);
static void print_prompt(void);

#define TERM_COLS 74
#define TERM_ROWS 24

static char  buf[TERM_ROWS][TERM_COLS];
static uint8_t col[TERM_ROWS][TERM_COLS];
static int cx = 0, cy = 0;

static char cmd[256];
static int cmd_len = 0;

static int ipc_qid = 0;

// --- Current working directory tracking ---
static char cwd_path[256] = "/";

// --- Command history circular buffer ---
#define HIST_MAX 16
static char history[HIST_MAX][256];
static int hist_count = 0;
static int hist_pos = -1;
static int hist_next_slot = 0;

static void history_add(const char* c) {
    if (!c || c[0] == '\0') return;
    if (hist_count > 0) {
        int last = (hist_next_slot == 0) ? HIST_MAX - 1 : hist_next_slot - 1;
        if (my_strcmp(history[last], c) == 0) return;
    }
    my_strcpy(history[hist_next_slot], c);
    hist_next_slot = (hist_next_slot + 1) % HIST_MAX;
    if (hist_count < HIST_MAX) hist_count++;
    hist_pos = -1;
}

// --- Built-in commands list for tab completion ---
static const char* builtins[] = {
    "help", "clear", "mfetch", "mem", "kmemstats", "vfsinfo",
    "ls", "cd", "pwd", "mkdir", "touch", "cat", "tree", "rm",
    "buat", "tulis", "edit", "baca", "hapus",
    "echo", "beep", "nada", "tunggu", "waktu", "warna", "kunci",
    "jalankan", "ular", "taskmgr", "lspci", "man",
    "ping", "host", "shutdown", "reboot", 0
};

static int my_strlen(const char* s) {
    int n = 0; while(s[n]) n++; return n;
}
static void my_strcpy(char* d, const char* s) {
    while(*s) *d++ = *s++; *d = '\0';
}
static int my_strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}
static int my_strncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] - b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

// --- Update cwd_path based on cd argument ---
static void update_cwd(const char* arg) {
    if (!arg || arg[0] == '\0') {
        // cd with no argument → go to root
        cwd_path[0] = '/';
        cwd_path[1] = '\0';
        return;
    }

    if (my_strcmp(arg, "/") == 0) {
        cwd_path[0] = '/';
        cwd_path[1] = '\0';
        return;
    }

    if (my_strcmp(arg, "..") == 0) {
        // Go up one level: trim last component
        int plen = my_strlen(cwd_path);
        if (plen <= 1) return; // Already at root
        
        // Find last '/'
        int last_slash = 0;
        for (int i = plen - 1; i > 0; i--) {
            if (cwd_path[i] == '/') { last_slash = i; break; }
        }
        if (last_slash == 0) {
            cwd_path[0] = '/';
            cwd_path[1] = '\0';
        } else {
            cwd_path[last_slash] = '\0';
        }
        return;
    }

    if (arg[0] == '/') {
        // Absolute path
        my_strcpy(cwd_path, arg);
    } else {
        // Relative path: append to current
        int plen = my_strlen(cwd_path);
        if (plen > 1) { // Not just "/"
            cwd_path[plen++] = '/';
        }
        int alen = my_strlen(arg);
        for (int i = 0; i < alen && plen < 254; i++) {
            cwd_path[plen++] = arg[i];
        }
        cwd_path[plen] = '\0';
    }

    // Strip trailing slash if present (but keep root "/")
    int flen = my_strlen(cwd_path);
    if (flen > 1 && cwd_path[flen - 1] == '/') {
        cwd_path[flen - 1] = '\0';
    }
}

static void do_tab_completion(int wid) {
    if (cmd_len == 0) return;
    
    // Find start of the word being completed
    int last_space = cmd_len - 1;
    while (last_space >= 0 && cmd[last_space] != ' ') {
        last_space--;
    }
    int word_start = last_space + 1;
    char prefix[64];
    int prefix_len = 0;
    for (int i = word_start; i < cmd_len && prefix_len < 63; i++) {
        prefix[prefix_len++] = cmd[i];
    }
    prefix[prefix_len] = '\0';
    
    // Matches collector
    char matches[16][64];
    int match_count = 0;
    
    if (word_start == 0) {
        // Complete built-in commands first
        for (int i = 0; builtins[i] != 0 && match_count < 16; i++) {
            // Check if builtins[i] starts with prefix
            int match = 1;
            for (int j = 0; j < prefix_len; j++) {
                if (builtins[i][j] != prefix[j]) { match = 0; break; }
            }
            if (match) {
                my_strcpy(matches[match_count++], builtins[i]);
            }
        }
    }
    
    // Complete VFS files
    int node_idx = sys_stat_file(cwd_path);
    if (node_idx >= 0 && match_count < 16) {
        dir_entry_t entries[64];
        int count = sys_list_dir(entries, 64, node_idx);
        for (int i = 0; i < count && match_count < 16; i++) {
            int match = 1;
            for (int j = 0; j < prefix_len; j++) {
                if (entries[i].name[j] != prefix[j]) { match = 0; break; }
            }
            if (match) {
                // If it's a directory, append a slash
                char name[64];
                my_strcpy(name, entries[i].name);
                if (entries[i].type == 1) { // directory
                    int nl = my_strlen(name);
                    if (nl < 62) { name[nl] = '/'; name[nl+1] = '\0'; }
                } else {
                    // Append a space for files
                    int nl = my_strlen(name);
                    if (nl < 62) { name[nl] = ' '; name[nl+1] = '\0'; }
                }
                my_strcpy(matches[match_count++], name);
            }
        }
    }
    
    if (match_count == 1) {
        // Exactly one match! Autocomplete it.
        // Erase prefix
        for (int i = 0; i < prefix_len; i++) {
            term_putchar('\b', 0x0F);
        }
        // Copy match into cmd
        int match_len = my_strlen(matches[0]);
        for (int i = 0; i < match_len && cmd_len < 254; i++) {
            cmd[word_start + i] = matches[0][i];
        }
        cmd_len = word_start + match_len;
        cmd[cmd_len] = '\0';
        term_print(matches[0], 0x0A);
        draw_terminal(wid);
    } else if (match_count > 1) {
        // Multiple matches, list them below
        term_putchar('\n', 0x0F);
        for (int i = 0; i < match_count; i++) {
            term_print(matches[i], 0x0E);
            term_print("  ", 0x0F);
        }
        term_putchar('\n', 0x0F);
        print_prompt();
        // Reprint the command typed so far
        cmd[cmd_len] = '\0';
        term_print(cmd, 0x0F);
        draw_terminal(wid);
    }
}

static void term_scroll() {
    for (int r = 0; r < TERM_ROWS - 1; r++)
        for (int c = 0; c < TERM_COLS; c++) {
            buf[r][c] = buf[r+1][c];
            col[r][c] = col[r+1][c];
        }
    for (int c = 0; c < TERM_COLS; c++) {
        buf[TERM_ROWS-1][c] = ' ';
        col[TERM_ROWS-1][c] = 0;
    }
    cy = TERM_ROWS - 1;
}

static void term_putchar(char c2, uint8_t color) {
    if (c2 == '\n') { cx = 0; cy++; }
    else if (c2 == '\r') { cx = 0; }
    else if (c2 == '\b') {
        if (cx > 0) { cx--; buf[cy][cx] = ' '; col[cy][cx] = 0; }
    } else {
        if (cx >= TERM_COLS) { cx = 0; cy++; }
        buf[cy][cx] = c2;
        col[cy][cx] = color;
        cx++;
    }
    if (cy >= TERM_ROWS) term_scroll();
}

static void term_print(const char* s, uint8_t color) {
    while (*s) term_putchar(*s++, color);
}

// VGA color to RGB (simplified Catppuccin palette)
static uint32_t vga_to_rgb(uint8_t c) {
    switch(c) {
        case 0x00: return 0x0011111B;
        case 0x01: return 0x0089B4FA; // Blue
        case 0x02: return 0x00A6E3A1; // Green
        case 0x03: return 0x0094E2D5; // Cyan
        case 0x04: return 0x00F38BA8; // Red
        case 0x05: return 0x00CBA6F7; // Purple
        case 0x06: return 0x00F9E2AF; // Yellow
        case 0x07: return 0x00BAC2DE; // Light gray
        case 0x08: return 0x00585B70; // Dark gray
        case 0x09: return 0x0089B4FA; // Light blue
        case 0x0A: return 0x00A6E3A1; // Light green
        case 0x0B: return 0x0094E2D5; // Light cyan
        case 0x0C: return 0x00F38BA8; // Light red
        case 0x0D: return 0x00F5C2E7; // Pink
        case 0x0E: return 0x00F9E2AF; // Light yellow
        case 0x0F: return 0x00CDD6F4; // White
        default:   return 0x00CDD6F4;
    }
}

static void draw_terminal(int wid) {
    int cw = 600, ch = 400;
    // Dark background
    sys_draw_rect(wid, 0, 0, cw, ch, 0x0011111B);
    
    // Render text buffer per line chunked by color
    for (int r = 0; r < TERM_ROWS; r++) {
        char line_buf[TERM_COLS + 1];
        int len = 0;
        int start_c = -1;
        uint8_t current_col = 0;

        for (int c = 0; c < TERM_COLS; c++) {
            char ch2 = buf[r][c];
            uint8_t vc = col[r][c];
            if (ch2 && vc) {
                if (start_c == -1) {
                    start_c = c;
                    current_col = vc;
                    line_buf[len++] = ch2;
                } else if (vc == current_col) {
                    int gap = c - (start_c + len);
                    for (int g = 0; g < gap; g++) line_buf[len++] = ' ';
                    line_buf[len++] = ch2;
                } else {
                    line_buf[len] = '\0';
                    sys_draw_text(wid, start_c * 8, r * 16, line_buf, vga_to_rgb(current_col));
                    start_c = c;
                    current_col = vc;
                    len = 0;
                    line_buf[len++] = ch2;
                }
            }
        }
        if (start_c != -1) {
            line_buf[len] = '\0';
            sys_draw_text(wid, start_c * 8, r * 16, line_buf, vga_to_rgb(current_col));
        }
    }
    
    // Cursor blink (simple - always show)
    sys_draw_rect(wid, cx*8, cy*16 + 14, 8, 2, 0x0000FF88);
    
    sys_update_window(wid);
}

static void drain_ipc() {
    // Read all IPC messages (shell output chars)
    char msg[128];
    int count = 0;
    while (count < 4096) { // drain as much as possible
        int ret = sys_ipc_try_recv(ipc_qid, msg, 128);
        if (ret <= 0) break;
        // msg contains [char1, col1, char2, col2, ...]
        for (int i = 0; i < ret; i += 2) {
            term_putchar(msg[i], (uint8_t)msg[i+1]);
        }
        count++;
    }
}

static void print_prompt() {
    term_print("root@mectov", 0x0A);
    term_print(":", 0x08);
    term_print(cwd_path, 0x0B);
    term_print("$ ", 0x0F);
}

void _start() {
    int wid = sys_create_window(60, 40, 600, 400, "Terminal (Ring 3)");
    if (wid < 0) sys_exit();
    
    // Create IPC queue for stdout
    ipc_qid = sys_ipc_create(0xDEAD);
    if (ipc_qid > 0) {
        sys_set_stdout_ipc(ipc_qid);
    }
    
    // Clear buffer
    for (int r = 0; r < TERM_ROWS; r++)
        for (int c = 0; c < TERM_COLS; c++) {
            buf[r][c] = 0; col[r][c] = 0;
        }
    
    term_print("Mectov OS v25.0 Terminal [Ring 3]\n", 0x0B);
    term_print("Welcome Bos Alif! System ready.\n\n", 0x0D);
    print_prompt();
    cmd_len = 0;
    
    draw_terminal(wid);
    
    gui_event_t ev;
    int frame = 0;
    
    while (1) {
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 1) { // Paint
                draw_terminal(wid);
            } else if (ev.type == 2) { // Key
                if (ev.key == 27) {
                    sys_set_stdout_ipc(0); // Disable redirect
                    sys_exit();
                }
                
                if (ev.key == '\n') {
                    term_putchar('\n', 0x0F);
                    cmd[cmd_len] = '\0';
                    
                    if (cmd_len > 0) {
                        // Add to command history
                        history_add(cmd);
                        
                        // Check for local commands
                        if (cmd[0] == 'c' && cmd[1] == 'l' && cmd[2] == 'e' &&
                            cmd[3] == 'a' && cmd[4] == 'r' && cmd[5] == '\0') {
                            // Clear screen locally
                            for (int r = 0; r < TERM_ROWS; r++)
                                for (int c = 0; c < TERM_COLS; c++) {
                                    buf[r][c] = 0; col[r][c] = 0;
                                }
                            cx = 0; cy = 0;
                        } else if (my_strncmp(cmd, "cd", 2) == 0 &&
                                   (cmd[2] == ' ' || cmd[2] == '\0')) {
                            // Handle cd locally: update cwd_path, then forward to kernel
                            char* arg = (cmd[2] == ' ') ? cmd + 3 : "";
                            // Forward to kernel so it updates current_dir
                            sys_exec_cmd(cmd);
                            drain_ipc();
                            // Update local cwd tracking
                            update_cwd(arg);
                        } else {
                            // Execute command via kernel
                            sys_exec_cmd(cmd);
                            // Drain any IPC output
                            drain_ipc();
                        }
                    }
                    
                    print_prompt();
                    cmd_len = 0;
                    draw_terminal(wid);
                } else if (ev.key == '\b') {
                    if (cmd_len > 0) {
                        cmd_len--;
                        term_putchar('\b', 0x0F);
                        draw_terminal(wid);
                    }
                } else if (ev.key == '\t') {
                    do_tab_completion(wid);
                } else if (ev.key == 0xE048) { // Up Arrow (History Back)
                    if (hist_count > 0) {
                        if (hist_pos == -1) hist_pos = hist_next_slot;
                        int new_pos = (hist_pos == 0) ? HIST_MAX - 1 : hist_pos - 1;
                        if (new_pos != hist_next_slot && history[new_pos][0] != '\0') {
                            hist_pos = new_pos;
                            // Erase current input line visually
                            for (int i = 0; i < cmd_len; i++) {
                                term_putchar('\b', 0x0F);
                            }
                            my_strcpy(cmd, history[hist_pos]);
                            cmd_len = my_strlen(cmd);
                            term_print(cmd, 0x0A);
                            draw_terminal(wid);
                        }
                    }
                } else if (ev.key == 0xE050) { // Down Arrow (History Forward)
                    if (hist_count > 0 && hist_pos != -1) {
                        int new_pos = (hist_pos + 1) % HIST_MAX;
                        if (new_pos != hist_next_slot) {
                            hist_pos = new_pos;
                            // Erase current input line visually
                            for (int i = 0; i < cmd_len; i++) {
                                term_putchar('\b', 0x0F);
                            }
                            my_strcpy(cmd, history[hist_pos]);
                            cmd_len = my_strlen(cmd);
                            term_print(cmd, 0x0A);
                            draw_terminal(wid);
                        } else {
                            // Reached newest, clear input line
                            hist_pos = -1;
                            for (int i = 0; i < cmd_len; i++) {
                                term_putchar('\b', 0x0F);
                            }
                            cmd_len = 0;
                            cmd[0] = '\0';
                            draw_terminal(wid);
                        }
                    }
                } else if (ev.key >= ' ' && ev.key <= '~' && cmd_len < 255) {
                    cmd[cmd_len++] = (char)ev.key;
                    term_putchar((char)ev.key, 0x0A);
                    draw_terminal(wid);
                }
            }
        }
        
        // Periodically drain IPC (for async output)
        frame++;
        if (frame % 50 == 0) {
            int old_cx = cx, old_cy = cy;
            drain_ipc();
            if (cx != old_cx || cy != old_cy) {
                draw_terminal(wid);
            }
        }
        
        sys_yield();
    }
}
