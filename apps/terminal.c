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
static void redraw_input_line(void);
static int get_max_scroll(void);
static void add_default_aliases(void);
static int expand_alias(const char* in, char* out);
static void handle_alias_cmd(const char* arg);
static void update_cwd(const char* arg);
static void do_tab_completion(int wid);
static void term_scroll(void);
static uint32_t vga_to_rgb(uint8_t c);
static void drain_ipc(void);

// Max cell grid — buffers are fixed at boot, the VISIBLE grid (cols/rows)
// below is dynamic so the terminal can relayout on WM resize events (type 5).
#define MAX_TERM_COLS 128
#define MAX_TERM_ROWS 48
#define SCROLLBACK_ROWS 128

static char  buf[SCROLLBACK_ROWS][MAX_TERM_COLS];
static uint8_t col[SCROLLBACK_ROWS][MAX_TERM_COLS];
static int cx = 0, cy = 0;

// Current grid + client area, updated by resize events (client = w-2, h-22)
static int cols = 74, rows = 23;
static int term_cw = 598, term_ch = 378;

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

// --- Alias structures and state ---
typedef struct {
    char name[32];
    char value[128];
} alias_t;

#define ALIAS_MAX 16
static alias_t aliases[ALIAS_MAX];
static int alias_count = 0;

static int edit_cursor = 0;      // Cursor index within 'cmd'
static int cmd_start_cx = 0;     // Input start column
static int cmd_start_cy = 0;     // Input start row

static int scroll_offset = 0;     // Scrolled-up row count
static char suggest_buf[256];    // History auto-suggestion
static int suggest_len = 0;      // auto-suggestion length

static int term_dirty = 1;       // High-performance render cap flags
static uint32_t last_draw_tick = 0;

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
    "help", "clear", "mfetch", "mem", "memstat", "kmemstats", "uptime", "vfsinfo",
    "ls", "cd", "pwd", "mkdir", "touch", "cat", "head", "tree", "rm", "rmdir", "cp", "mv", "df",
    "edit", "nano",
    "sh", "source", "export", "unalias", "ps", "kill",
    "echo", "beep", "tone", "sleep", "date", "color", "lock",
    "uname", "whoami", "hostname", "env", "seq", "wc", "type", "yes",
    "printf", "sort", "uniq", "tee", "find",
    "run", "snake", "taskmgr", "flappy", "doom", "lspci", "man",
    "ping", "host", "grep", "shutdown", "reboot", "alias", "history", 0
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

static int get_max_scroll(void) {
    if (cy < rows) return 0;
    return cy - (rows - 1);
}

static void add_default_aliases(void) {
    my_strcpy(aliases[0].name, "ll");
    my_strcpy(aliases[0].value, "ls -la");
    my_strcpy(aliases[1].name, "..");
    my_strcpy(aliases[1].value, "cd ..");
    my_strcpy(aliases[2].name, "snake");
    my_strcpy(aliases[2].value, "run /apps/snake.mct");
    alias_count = 3;
}

static int expand_alias(const char* in, char* out) {
    char first_word[32];
    int fw_len = 0;
    while (in[fw_len] && in[fw_len] != ' ' && fw_len < 31) {
        first_word[fw_len] = in[fw_len];
        fw_len++;
    }
    first_word[fw_len] = '\0';

    if (fw_len == 0) {
        my_strcpy(out, in);
        return 0;
    }

    for (int i = 0; i < alias_count; i++) {
        if (my_strcmp(aliases[i].name, first_word) == 0) {
            my_strcpy(out, aliases[i].value);
            if (in[fw_len] != '\0') {
                int val_len = my_strlen(aliases[i].value);
                my_strcpy(out + val_len, in + fw_len);
            }
            return 1;
        }
    }
    my_strcpy(out, in);
    return 0;
}

static void handle_alias_cmd(const char* arg) {
    term_dirty = 1;
    if (!arg || arg[0] == '\0') {
        term_print("Active Shell Aliases:\n", 0x0E);
        for (int i = 0; i < alias_count; i++) {
            term_print("  ", 0x0F);
            term_print(aliases[i].name, 0x0A);
            term_print(" -> \"", 0x0F);
            term_print(aliases[i].value, 0x0B);
            term_print("\"\n", 0x0F);
        }
        return;
    }

    int eq_idx = -1;
    int arg_len = my_strlen(arg);
    for (int i = 0; i < arg_len; i++) {
        if (arg[i] == '=') {
            eq_idx = i;
            break;
        }
    }

    if (eq_idx == -1) {
        for (int i = 0; i < alias_count; i++) {
            if (my_strcmp(aliases[i].name, arg) == 0) {
                term_print(aliases[i].name, 0x0A);
                term_print("='", 0x0F);
                term_print(aliases[i].value, 0x0B);
                term_print("'\n", 0x0F);
                return;
            }
        }
        term_print("alias: ", 0x0C);
        term_print(arg, 0x0C);
        term_print(" not found.\n", 0x0C);
        return;
    }

    char name[32];
    int n_len = eq_idx;
    if (n_len > 31) n_len = 31;
    for (int i = 0; i < n_len; i++) name[i] = arg[i];
    name[n_len] = '\0';

    const char* val_start = arg + eq_idx + 1;
    int v_len = my_strlen(val_start);
    if ((val_start[0] == '\'' && val_start[v_len-1] == '\'') ||
        (val_start[0] == '\"' && val_start[v_len-1] == '\"')) {
        val_start++;
        v_len -= 2;
    }

    char value[128];
    if (v_len > 127) v_len = 127;
    for (int i = 0; i < v_len; i++) value[i] = val_start[i];
    value[v_len] = '\0';

    for (int i = 0; i < alias_count; i++) {
        if (my_strcmp(aliases[i].name, name) == 0) {
            my_strcpy(aliases[i].value, value);
            term_print("Updated alias: ", 0x0A);
            term_print(name, 0x0A);
            term_print("\n", 0x0F);
            return;
        }
    }

    if (alias_count < ALIAS_MAX) {
        my_strcpy(aliases[alias_count].name, name);
        my_strcpy(aliases[alias_count].value, value);
        alias_count++;
        term_print("Added alias: ", 0x0A);
        term_print(name, 0x0A);
        term_print("\n", 0x0F);
    } else {
        term_print("alias: maximum aliases reached!\n", 0x0C);
    }
}

// --- Update cwd_path based on cd argument ---
static void update_cwd(const char* arg) {
    if (!arg || arg[0] == '\0') {
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
        int plen = my_strlen(cwd_path);
        if (plen <= 1) return;
        
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
        my_strcpy(cwd_path, arg);
    } else {
        int plen = my_strlen(cwd_path);
        if (plen > 1) {
            cwd_path[plen++] = '/';
        }
        int alen = my_strlen(arg);
        for (int i = 0; i < alen && plen < 254; i++) {
            cwd_path[plen++] = arg[i];
        }
        cwd_path[plen] = '\0';
    }

    int flen = my_strlen(cwd_path);
    if (flen > 1 && cwd_path[flen - 1] == '/') {
        cwd_path[flen - 1] = '\0';
    }
}

static void redraw_input_line(void) {
    term_dirty = 1;
    cx = cmd_start_cx;
    cy = cmd_start_cy;
    for (int c = cmd_start_cx; c < cols; c++) {
        buf[cy][c] = ' ';
        col[cy][c] = 0;
    }
    
    int i = 0;
    while (i < cmd_len && cmd[i] == ' ') {
        term_putchar(' ', 0x0F);
        i++;
    }
    
    char first_word[64];
    int fw_len = 0;
    while (i < cmd_len && cmd[i] != ' ' && fw_len < 63) {
        first_word[fw_len++] = cmd[i];
        i++;
    }
    first_word[fw_len] = '\0';
    
    int is_valid_cmd = 0;
    if (fw_len > 0) {
        for (int b = 0; builtins[b] != 0; b++) {
            if (my_strcmp(builtins[b], first_word) == 0) {
                is_valid_cmd = 1;
                break;
            }
        }
        if (!is_valid_cmd) {
            for (int a = 0; a < alias_count; a++) {
                if (my_strcmp(aliases[a].name, first_word) == 0) {
                    is_valid_cmd = 1;
                    break;
                }
            }
        }
    }
    
    uint8_t cmd_color = is_valid_cmd ? 0x0A : 0x0F;
    for (int j = 0; j < fw_len; j++) {
        term_putchar(first_word[j], cmd_color);
    }
    
    while (i < cmd_len) {
        if (cmd[i] == ' ') {
            term_putchar(' ', 0x0F);
            i++;
        } else if (cmd[i] == '-') {
            while (i < cmd_len && cmd[i] != ' ') {
                term_putchar(cmd[i], 0x0D);
                i++;
            }
        } else {
            while (i < cmd_len && cmd[i] != ' ') {
                term_putchar(cmd[i], 0x0B);
                i++;
            }
        }
    }

    suggest_len = 0;
    suggest_buf[0] = '\0';
    if (cmd_len > 0) {
        for (int h = 0; h < hist_count; h++) {
            int idx = (hist_next_slot - 1 - h + HIST_MAX) % HIST_MAX;
            if (history[idx][0] != '\0' && my_strlen(history[idx]) > cmd_len) {
                if (my_strncmp(history[idx], cmd, cmd_len) == 0) {
                    my_strcpy(suggest_buf, history[idx]);
                    suggest_len = my_strlen(suggest_buf);
                    break;
                }
            }
        }
        
        if (suggest_len > cmd_len) {
            int vis_cx = cx;
            int sug_idx = cmd_len;
            while (sug_idx < suggest_len && vis_cx < cols) {
                buf[cy][vis_cx] = suggest_buf[sug_idx];
                col[cy][vis_cx] = 0x08; // Dark gray suggestion
                vis_cx++;
                sug_idx++;
            }
        }
    }
    
    cx = cmd_start_cx + edit_cursor;
    cy = cmd_start_cy;
}

static void do_tab_completion(int wid) {
    term_dirty = 1;
    if (cmd_len == 0) return;
    
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
    
    char matches[16][64];
    int match_count = 0;
    
    if (word_start == 0) {
        for (int i = 0; builtins[i] != 0 && match_count < 16; i++) {
            int match = 1;
            for (int j = 0; j < prefix_len; j++) {
                if (builtins[i][j] != prefix[j]) { match = 0; break; }
            }
            if (match) {
                my_strcpy(matches[match_count++], builtins[i]);
            }
        }
    }
    
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
                char name[64];
                my_strcpy(name, entries[i].name);
                if (entries[i].type == 1) {
                    int nl = my_strlen(name);
                    if (nl < 62) { name[nl] = '/'; name[nl+1] = '\0'; }
                } else {
                    int nl = my_strlen(name);
                    if (nl < 62) { name[nl] = ' '; name[nl+1] = '\0'; }
                }
                my_strcpy(matches[match_count++], name);
            }
        }
    }
    
    if (match_count == 1) {
        int match_len = my_strlen(matches[0]);
        for (int i = 0; i < match_len && cmd_len < 254; i++) {
            cmd[word_start + i] = matches[0][i];
        }
        cmd_len = word_start + match_len;
        cmd[cmd_len] = '\0';
        edit_cursor = cmd_len;
        redraw_input_line();
    } else if (match_count > 1) {
        term_putchar('\n', 0x0F);
        for (int i = 0; i < match_count; i++) {
            term_print(matches[i], 0x0E);
            term_print("  ", 0x0F);
        }
        term_putchar('\n', 0x0F);
        print_prompt();
        cmd_start_cx = cx;
        cmd_start_cy = cy;
        edit_cursor = cmd_len;
        redraw_input_line();
    }
}

static void term_scroll(void) {
    for (int r = 0; r < SCROLLBACK_ROWS - 1; r++)
        for (int c = 0; c < cols; c++) {
            buf[r][c] = buf[r+1][c];
            col[r][c] = col[r+1][c];
        }
    for (int c = 0; c < cols; c++) {
        buf[SCROLLBACK_ROWS-1][c] = ' ';
        col[SCROLLBACK_ROWS-1][c] = 0;
    }
    cy = SCROLLBACK_ROWS - 1;
    if (cmd_start_cy > 0) cmd_start_cy--;
}

static void term_putchar(char c2, uint8_t color) {
    term_dirty = 1;
    if (c2 == '\n') { cx = 0; cy++; }
    else if (c2 == '\r') { cx = 0; }
    else if (c2 == '\b') {
        if (cx > 0) { cx--; buf[cy][cx] = ' '; col[cy][cx] = 0; }
    } else {
        if (cx >= cols) { cx = 0; cy++; }
        buf[cy][cx] = c2;
        col[cy][cx] = color;
        cx++;
    }
    if (cy >= SCROLLBACK_ROWS) term_scroll();
}

static void term_print(const char* s, uint8_t color) {
    while (*s) term_putchar(*s++, color);
}

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
    int cw = term_cw, ch = term_ch;
    sys_draw_rect(wid, 0, 0, cw, ch, 0x0011111B);
    
    int start_row = 0;
    if (cy >= rows) {
        start_row = cy - (rows - 1) - scroll_offset;
    }
    
    for (int r_vis = 0; r_vis < rows; r_vis++) {
        int r = start_row + r_vis;
        char line_buf[MAX_TERM_COLS + 1];
        int len = 0;
        int start_c = -1;
        uint8_t current_col = 0;

        for (int c = 0; c < cols; c++) {
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
                    sys_draw_text(wid, start_c * 8, r_vis * 16, line_buf, vga_to_rgb(current_col));
                    start_c = c;
                    current_col = vc;
                    len = 0;
                    line_buf[len++] = ch2;
                }
            }
        }
        if (start_c != -1) {
            line_buf[len] = '\0';
            sys_draw_text(wid, start_c * 8, r_vis * 16, line_buf, vga_to_rgb(current_col));
        }
    }
    
    int cursor_vis_y = cy - start_row;
    if (cursor_vis_y >= 0 && cursor_vis_y < rows) {
        sys_draw_rect(wid, cx*8, cursor_vis_y*16 + 14, 8, 2, 0x0000FF88);
    }
    
    int max_scroll = get_max_scroll();
    if (max_scroll > 0) {
        int track_h = ch - 20;
        int bar_h = (rows * track_h) / (cy + 1);
        if (bar_h < 20) bar_h = 20;
        
        int scroll_y = 10 + ((max_scroll - scroll_offset) * (track_h - bar_h)) / max_scroll;
        
        sys_draw_rect(wid, cw - 6, 10, 4, track_h, 0x001E1E2E);
        sys_draw_rect(wid, cw - 6, scroll_y, 4, bar_h, 0x0089B4FA);
    }
    
    sys_update_window(wid);
}

static void drain_ipc(void) {
    char msg[128];
    int count = 0;
    while (count < 4096) {
        int ret = sys_ipc_try_recv(ipc_qid, msg, 128);
        if (ret <= 0) break;
        for (int i = 0; i < ret; i += 2) {
            term_putchar(msg[i], (uint8_t)msg[i+1]);
        }
        term_dirty = 1;
        count++;
    }
}

static void print_prompt(void) {
    term_print("root@mectov", 0x0A);
    term_print(":", 0x08);
    term_print(cwd_path, 0x0B);
    term_print("$ ", 0x0F);
}

void _start(void) {
    int wid = sys_create_window(60, 40, 600, 400, "Terminal (Ring 3)");
    if (wid < 0) sys_exit();

    // Become the controlling terminal's session leader: setsid() makes this
    // task its own process-group and session leader, then tcsetpgrp() marks
    // that group as the terminal's foreground group. Apps spawned from the
    // shell inherit this group (so they read the terminal freely); an app that
    // calls setpgid(0,0) moves to its own background group and SIGTTIN stops
    // it from stealing keyboard input.
    sys_setsid();
    sys_tcsetpgrp(1, sys_getpid());

    add_default_aliases();
    
    ipc_qid = sys_ipc_create(0xDEAD);
    if (ipc_qid > 0) {
        sys_set_stdout_ipc(ipc_qid);
    }
    
    for (int r = 0; r < SCROLLBACK_ROWS; r++)
        for (int c = 0; c < MAX_TERM_COLS; c++) {
            buf[r][c] = 0; col[r][c] = 0;
        }
    
    term_print("Mectov OS v36.3 Terminal [Ring 3]\n", 0x0B);
    term_print("Welcome Bos Alif! System ready.\n\n", 0x0D);
    print_prompt();
    cmd_len = 0;
    edit_cursor = 0;
    
    cmd_start_cx = cx;
    cmd_start_cy = cy;
    
    term_dirty = 1;
    last_draw_tick = sys_get_ticks();
    
    gui_event_t ev;
    
    while (1) {
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 1) {
                term_dirty = 1;
            } else if (ev.type == 2) { // Keyboard key press
                term_dirty = 1;
                if (ev.key == 27) { // Escape
                    sys_set_stdout_ipc(0);
                    sys_exit();
                }
                
                if (ev.key == '\n') {
                    scroll_offset = 0; // Snap to bottom
                    term_putchar('\n', 0x0F);
                    cmd[cmd_len] = '\0';
                    
                    if (cmd_len > 0) {
                        history_add(cmd);
                        
                        char expanded[256];
                        expand_alias(cmd, expanded);
                        
                        // Parse command locally
                        if (my_strncmp(expanded, "alias", 5) == 0 && (expanded[5] == ' ' || expanded[5] == '\0')) {
                            const char* arg = (expanded[5] == ' ') ? expanded + 6 : "";
                            handle_alias_cmd(arg);
                        } else if (my_strcmp(expanded, "history") == 0) {
                            term_print("Shell Command History:\n", 0x0E);
                            for (int h = 0; h < hist_count; h++) {
                                int idx = (hist_next_slot - hist_count + h + HIST_MAX) % HIST_MAX;
                                
                                // Print 1-based index
                                char num_str[8];
                                int num = h + 1;
                                int n_idx = 0;
                                if (num >= 10) {
                                    num_str[n_idx++] = '0' + (num / 10);
                                }
                                num_str[n_idx++] = '0' + (num % 10);
                                num_str[n_idx] = '\0';
                                
                                term_print("  ", 0x0F);
                                term_print(num_str, 0x0A);
                                term_print(": ", 0x08);
                                term_print(history[idx], 0x0F);
                                term_print("\n", 0x0F);
                            }
                        } else if (my_strcmp(expanded, "clear") == 0) {
                            for (int r = 0; r < SCROLLBACK_ROWS; r++)
                                for (int c = 0; c < MAX_TERM_COLS; c++) {
                                    buf[r][c] = 0; col[r][c] = 0;
                                }
                            cx = 0; cy = 0;
                        } else if (my_strncmp(expanded, "cd", 2) == 0 && (expanded[2] == ' ' || expanded[2] == '\0')) {
                            char* arg = (expanded[2] == ' ') ? expanded + 3 : "";
                            sys_exec_cmd(expanded);
                            drain_ipc();
                            update_cwd(arg);
                        } else {
                            sys_exec_cmd(expanded);
                            drain_ipc();
                        }
                    }
                    
                    print_prompt();
                    cmd_len = 0;
                    edit_cursor = 0;
                    cmd_start_cx = cx;
                    cmd_start_cy = cy;
                    redraw_input_line();
                } else if (ev.key == '\b') {
                    scroll_offset = 0; // Snap to bottom
                    if (edit_cursor > 0) {
                        // Shift left
                        for (int i = edit_cursor - 1; i < cmd_len - 1; i++) {
                            cmd[i] = cmd[i+1];
                        }
                        cmd_len--;
                        edit_cursor--;
                        cmd[cmd_len] = '\0';
                        redraw_input_line();
                    }
                } else if (ev.key == '\t') {
                    scroll_offset = 0;
                    do_tab_completion(wid);
                } else if (ev.key == 0xE048) { // Up Arrow (History Back)
                    scroll_offset = 0;
                    if (hist_count > 0) {
                        if (hist_pos == -1) hist_pos = hist_next_slot;
                        int new_pos = (hist_pos == 0) ? HIST_MAX - 1 : hist_pos - 1;
                        if (new_pos != hist_next_slot && history[new_pos][0] != '\0') {
                            hist_pos = new_pos;
                            my_strcpy(cmd, history[hist_pos]);
                            cmd_len = my_strlen(cmd);
                            edit_cursor = cmd_len;
                            redraw_input_line();
                        }
                    }
                } else if (ev.key == 0xE050) { // Down Arrow (History Forward)
                    scroll_offset = 0;
                    if (hist_count > 0 && hist_pos != -1) {
                        int new_pos = (hist_pos + 1) % HIST_MAX;
                        if (new_pos != hist_next_slot) {
                            hist_pos = new_pos;
                            my_strcpy(cmd, history[hist_pos]);
                            cmd_len = my_strlen(cmd);
                            edit_cursor = cmd_len;
                            redraw_input_line();
                        } else {
                            hist_pos = -1;
                            cmd_len = 0;
                            edit_cursor = 0;
                            cmd[0] = '\0';
                            redraw_input_line();
                        }
                    }
                } else if (ev.key == 0xE04B) { // Left Arrow
                    if (edit_cursor > 0) {
                        edit_cursor--;
                        cx--;
                        redraw_input_line();
                    }
                } else if (ev.key == 0xE04D) { // Right Arrow
                    if (suggest_len > cmd_len && edit_cursor == cmd_len) {
                        // Accept auto-suggestion!
                        my_strcpy(cmd, suggest_buf);
                        cmd_len = suggest_len;
                        edit_cursor = cmd_len;
                        redraw_input_line();
                    } else if (edit_cursor < cmd_len) {
                        edit_cursor++;
                        cx++;
                        redraw_input_line();
                    }
                } else if (ev.key == 0xE047) { // Home Key
                    edit_cursor = 0;
                    cx = cmd_start_cx;
                    redraw_input_line();
                } else if (ev.key == 0xE04F) { // End Key
                    if (suggest_len > cmd_len) {
                        // Accept auto-suggestion!
                        my_strcpy(cmd, suggest_buf);
                        cmd_len = suggest_len;
                        edit_cursor = cmd_len;
                        redraw_input_line();
                    } else {
                        edit_cursor = cmd_len;
                        cx = cmd_start_cx + cmd_len;
                        redraw_input_line();
                    }
                } else if (ev.key == 0xE049) { // Page Up
                    int max = get_max_scroll();
                    scroll_offset += 6;
                    if (scroll_offset > max) scroll_offset = max;
                } else if (ev.key == 0xE051) { // Page Down
                    scroll_offset -= 6;
                    if (scroll_offset < 0) scroll_offset = 0;
                } else if (ev.key == 3) { // Ctrl+C, no fg app = cancel line (^C)
                    // The kernel consumes Ctrl+C while a foreground app runs
                    // (SIGINT to its process group) and only forwards this
                    // event when the terminal is free. POSIX-style ^C: print
                    // the marker and drop whatever was being typed.
                    scroll_offset = 0;
                    term_print("^C\n", 0x0F);
                    cmd_len = 0;
                    edit_cursor = 0;
                    cmd[0] = '\0';
                    print_prompt();
                    cmd_start_cx = cx;
                    cmd_start_cy = cy;
                    redraw_input_line();
                } else if (ev.key == 0x8003) { // Ctrl+Shift+C = Copy current command
                    if (cmd_len > 0) {
                        sys_clipboard_copy(cmd, cmd_len);
                    }
                } else if (ev.key == 22) { // Ctrl+V = Paste from clipboard
                    char paste_buf[256];
                    int pasted = sys_clipboard_paste(paste_buf, 256);
                    if (pasted > 0) {
                        scroll_offset = 0; // Snap to bottom
                        for (int i = 0; i < pasted && cmd_len < 255; i++) {
                            if (paste_buf[i] >= ' ' && paste_buf[i] <= '~') {
                                // Shift right to insert
                                for (int j = cmd_len; j > edit_cursor; j--) {
                                    cmd[j] = cmd[j-1];
                                }
                                cmd[edit_cursor] = paste_buf[i];
                                cmd_len++;
                                edit_cursor++;
                            }
                        }
                        cmd[cmd_len] = '\0';
                        redraw_input_line();
                    }
                } else if (ev.key >= ' ' && ev.key <= '~' && cmd_len < 255) {
                    scroll_offset = 0; // Snap to bottom
                    // Shift right to insert
                    for (int i = cmd_len; i > edit_cursor; i--) {
                        cmd[i] = cmd[i-1];
                    }
                    cmd[edit_cursor] = (char)ev.key;
                    cmd_len++;
                    edit_cursor++;
                    cmd[cmd_len] = '\0';
                    redraw_input_line();
                }
            } else if (ev.type == 3) { // Mouse Click Event
                if (ev.key == 1) { // Left click
                    term_dirty = 1;
                    scroll_offset = 0; // Snap back to bottom on input click
                    
                    int click_cx = ev.x / 8;
                    int click_cy = ev.y / 16;
                    int start_row = 0;
                    if (cy >= rows) {
                        start_row = cy - (rows - 1) - scroll_offset;
                    }
                    int click_r = start_row + click_cy;
                    
                    int last_char_r = cmd_start_cy + (cmd_start_cx + cmd_len) / cols;
                    if (click_r >= cmd_start_cy && click_r <= last_char_r) {
                        if (click_r == cmd_start_cy) {
                            if (click_r == last_char_r) {
                                int relative = click_cx - cmd_start_cx;
                                if (relative < 0) edit_cursor = 0;
                                else if (relative > cmd_len) edit_cursor = cmd_len;
                                else edit_cursor = relative;
                            } else {
                                int relative = click_cx - cmd_start_cx;
                                if (relative < 0) edit_cursor = 0;
                                else if (relative >= cols - cmd_start_cx) edit_cursor = cols - cmd_start_cx - 1;
                                else edit_cursor = relative;
                            }
                        } else if (click_r == last_char_r) {
                            int row_start_idx = (cols - cmd_start_cx) + (click_r - cmd_start_cy - 1) * cols;
                            int relative = click_cx;
                            int last_row_chars = (cmd_start_cx + cmd_len) % cols;
                            if (relative < 0) edit_cursor = row_start_idx;
                            else if (relative > last_row_chars) edit_cursor = cmd_len;
                            else edit_cursor = row_start_idx + relative;
                        } else {
                            int row_start_idx = (cols - cmd_start_cx) + (click_r - cmd_start_cy - 1) * cols;
                            int relative = click_cx;
                            if (relative < 0) edit_cursor = row_start_idx;
                            else if (relative >= cols) edit_cursor = row_start_idx + cols - 1;
                            else edit_cursor = row_start_idx + relative;
                        }
                        redraw_input_line();
                    }
                }
            } else if (ev.type == 4) { // Scroll Wheel Event
                term_dirty = 1;
                if (ev.key > 0) { // Scroll Up
                    int max = get_max_scroll();
                    scroll_offset += 3;
                    if (scroll_offset > max) scroll_offset = max;
                } else if (ev.key < 0) { // Scroll Down
                    scroll_offset -= 3;
                    if (scroll_offset < 0) scroll_offset = 0;
                }
            } else if (ev.type == 5) { // Window Resize: ev.x = client w, ev.y = client h
                term_dirty = 1;
                scroll_offset = 0; // Snap to bottom
                term_cw = ev.x;
                term_ch = ev.y;
                cols = ev.x / 8;
                rows = ev.y / 16;
                if (cols < 8) cols = 8;
                if (rows < 3) rows = 3;
                if (cols > MAX_TERM_COLS) cols = MAX_TERM_COLS;
                if (rows > MAX_TERM_ROWS) rows = MAX_TERM_ROWS;
                // Keep cursor and prompt within the new grid
                if (cx >= cols) { cx = 0; cy++; }
                if (cy >= SCROLLBACK_ROWS) cy = SCROLLBACK_ROWS - 1;
                redraw_input_line();
            }
        }
        
        // 2. Kuras IPC secara real-time di setiap loop (0ms delay)
        drain_ipc();
        
        // 3. Batasi render maksimal 60 FPS (16ms) jika kotor
        uint32_t current_tick = sys_get_ticks();
        if (term_dirty && (current_tick - last_draw_tick >= 16)) {
            draw_terminal(wid);
            last_draw_tick = current_tick;
            term_dirty = 0;
        }
        
        sys_yield();
    }
}
