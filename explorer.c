#include "src/include/syscall.h"

typedef struct {
    int type;
    int x, y;
    int key;
} gui_event_t;

static dir_entry_t entries[64];
static int entry_count = 0;
static int current_parent = 0; // root
static int scroll_offset = 0;
static int selected = -1;

// --- Back navigation stack (max 8 levels deep) ---
#define MAX_DEPTH 8
static int parent_stack[MAX_DEPTH];
static int stack_depth = 0;

// --- Path bar string ---
static char path_bar[256] = "/";

static int my_strlen(const char* s) {
    int n = 0; while(s[n]) n++; return n;
}

static void my_strcpy(char* d, const char* s) {
    while(*s) *d++ = *s++; *d = '\0';
}

static void itoa_simple(int val, char* buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char temp[12]; int len = 0;
    while (val > 0) { temp[len++] = '0' + (val % 10); val /= 10; }
    int pos = 0;
    for (int i = len - 1; i >= 0; i--) buf[pos++] = temp[i];
    buf[pos] = '\0';
}

static void refresh_entries() {
    entry_count = sys_list_dir(entries, 64, current_parent);
    if (entry_count < 0) entry_count = 0;
    scroll_offset = 0;
    selected = -1;
}

// --- Navigate into a subdirectory ---
static void navigate_into(int node_idx, const char* name) {
    // Push current parent to stack
    if (stack_depth < MAX_DEPTH) {
        parent_stack[stack_depth++] = current_parent;
    }
    current_parent = node_idx;

    // Update path bar: append "/name"
    int plen = my_strlen(path_bar);
    if (plen > 1) { // Not just "/"
        path_bar[plen++] = '/';
    }
    int nlen = my_strlen(name);
    for (int i = 0; i < nlen && plen < 254; i++) {
        path_bar[plen++] = name[i];
    }
    path_bar[plen] = '\0';

    refresh_entries();
}

// --- Navigate back to parent ---
static void navigate_back() {
    if (stack_depth <= 0) return;
    current_parent = parent_stack[--stack_depth];

    // Rebuild path bar: trim last component
    int plen = my_strlen(path_bar);
    // Find last '/'
    int last_slash = 0;
    for (int i = plen - 1; i > 0; i--) {
        if (path_bar[i] == '/') { last_slash = i; break; }
    }
    if (last_slash == 0) {
        path_bar[0] = '/';
        path_bar[1] = '\0';
    } else {
        path_bar[last_slash] = '\0';
    }

    refresh_entries();
}

// --- Draw folder icon (small folder shape) ---
static void draw_folder_icon(int wid, int x, int y) {
    // Folder tab (top part)
    sys_draw_rect(wid, x, y, 6, 2, 0x0089B4FA);
    // Folder body
    sys_draw_rect(wid, x, y + 2, 14, 10, 0x0089B4FA);
    // Folder body inner (slightly darker to give depth)
    sys_draw_rect(wid, x + 1, y + 4, 12, 7, 0x00749DD4);
}

// --- Draw file icon (document shape) ---
static void draw_file_icon(int wid, int x, int y) {
    // Page body
    sys_draw_rect(wid, x, y, 12, 13, 0x00FFBB55);
    // Dog-ear corner (top right)
    sys_draw_rect(wid, x + 8, y, 4, 4, 0x00E0A040);
    // Lines on page
    sys_draw_rect(wid, x + 2, y + 5, 8, 1, 0x00D09030);
    sys_draw_rect(wid, x + 2, y + 8, 6, 1, 0x00D09030);
}

// --- Draw device icon ---
static void draw_dev_icon(int wid, int x, int y) {
    // Gear-like shape
    sys_draw_rect(wid, x + 2, y, 10, 13, 0x00A6E3A1);
    sys_draw_rect(wid, x, y + 3, 14, 7, 0x00A6E3A1);
    sys_draw_rect(wid, x + 4, y + 2, 6, 9, 0x0080C080);
}

static void draw_explorer(int wid) {
    int cw = 400, ch = 340;
    sys_draw_rect(wid, 0, 0, cw, ch, 0x001E1E2E);

    // ====== Header bar (44px tall) ======
    sys_draw_rect(wid, 0, 0, cw, 44, 0x00181825);

    // Back button [←]
    int back_x = 6, back_y = 4;
    uint32_t back_col = (stack_depth > 0) ? 0x00313244 : 0x00252535;
    sys_draw_rect(wid, back_x, back_y, 28, 18, back_col);
    uint32_t arrow_col = (stack_depth > 0) ? 0x00CDD6F4 : 0x00585B70;
    sys_draw_text(wid, back_x + 8, back_y + 1, "<", arrow_col);

    // Path bar
    sys_draw_rect(wid, 40, 4, cw - 110, 18, 0x00252535);
    // Truncate path if too long
    char display_path[48];
    int plen = my_strlen(path_bar);
    if (plen > 42) {
        display_path[0] = '.'; display_path[1] = '.';
        int start = plen - 40;
        int j = 2;
        for (int i = start; i < plen && j < 47; i++) {
            display_path[j++] = path_bar[i];
        }
        display_path[j] = '\0';
    } else {
        my_strcpy(display_path, path_bar);
    }
    sys_draw_text(wid, 44, 5, display_path, 0x0094E2D5);

    // Ring 3 badge
    sys_draw_text(wid, cw - 60, 5, "Ring 3", 0x00F9E2AF);

    // Column headers
    sys_draw_rect(wid, 0, 24, cw, 20, 0x00181825);
    sys_draw_text(wid, 28, 27, "Name", 0x00A6ADC8);
    sys_draw_text(wid, 230, 27, "Type", 0x00A6ADC8);
    sys_draw_text(wid, 310, 27, "Size", 0x00A6ADC8);
    sys_draw_rect(wid, 0, 44, cw, 1, 0x00313244);

    // ====== File list ======
    int list_top = 46;
    int row_h = 22;
    int max_rows = (ch - list_top - 22) / row_h;
    int y = list_top;

    for (int i = scroll_offset; i < entry_count && (i - scroll_offset) < max_rows; i++) {
        dir_entry_t* e = &entries[i];
        uint32_t row_bg = (i == selected) ? 0x002A2A3E : ((i % 2 == 0) ? 0x001E1E2E : 0x001A1A2A);
        sys_draw_rect(wid, 0, y, cw, row_h, row_bg);

        // Icon (folder vs file vs dev)
        if (e->type == 1) {
            draw_folder_icon(wid, 8, y + 4);
        } else if (e->type == 2) {
            draw_dev_icon(wid, 8, y + 4);
        } else {
            draw_file_icon(wid, 8, y + 4);
        }

        // Name
        sys_draw_text(wid, 28, y + 3, e->name, 0x00CDD6F4);

        // Type
        const char* tstr = "File";
        uint32_t tcol = 0x006C7086;
        if (e->type == 1) { tstr = "Dir"; tcol = 0x0089B4FA; }
        else if (e->type == 2) { tstr = "Dev"; tcol = 0x00A6E3A1; }
        sys_draw_text(wid, 230, y + 3, tstr, tcol);

        // Size
        if (e->type == 0) {
            char sbuf[20];
            itoa_simple(e->size, sbuf);
            int slen = my_strlen(sbuf);
            sbuf[slen++] = ' ';
            sbuf[slen++] = 'B';
            sbuf[slen] = '\0';
            sys_draw_text(wid, 310, y + 3, sbuf, 0x007F849C);
        } else {
            sys_draw_text(wid, 310, y + 3, "-", 0x007F849C);
        }

        y += row_h;
    }

    if (entry_count == 0) {
        sys_draw_text(wid, 8, list_top + 8, "Direktori kosong.", 0x006C7086);
    }

    // ====== Status bar ======
    sys_draw_rect(wid, 0, ch - 20, cw, 20, 0x00181825);
    char cbuf[64];
    itoa_simple(entry_count, cbuf);
    int clen = my_strlen(cbuf);
    cbuf[clen++] = ' '; cbuf[clen++] = 'i'; cbuf[clen++] = 't';
    cbuf[clen++] = 'e'; cbuf[clen++] = 'm'; cbuf[clen++] = 's';
    cbuf[clen] = '\0';
    sys_draw_text(wid, 8, ch - 17, cbuf, 0x006C7086);

    // Depth indicator
    if (stack_depth > 0) {
        char depth_buf[16] = "Depth: ";
        itoa_simple(stack_depth, depth_buf + 7);
        sys_draw_text(wid, cw - 80, ch - 17, depth_buf, 0x00585B70);
    }

    sys_update_window(wid);
}

void _start() { sys_print("starting explorer...\n", 0x0A);
    int wid = sys_create_window(100, 80, 400, 340, "Explorer (Ring 3)");
    if (wid < 0) sys_exit();

    // Init path
    path_bar[0] = '/';
    path_bar[1] = '\0';
    stack_depth = 0;
    current_parent = 0;

    refresh_entries();
    draw_explorer(wid);

    gui_event_t ev;

    while (1) {
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 1) { // Paint
                draw_explorer(wid);
            } else if (ev.type == 2) { // Key
                if (ev.key == 27) sys_exit(); // ESC

                // Scroll with W/S or arrow-like keys
                if ((ev.key == 'w' || ev.key == 'W') && scroll_offset > 0) {
                    scroll_offset--;
                    draw_explorer(wid);
                }
                if ((ev.key == 's' || ev.key == 'S') && scroll_offset < entry_count - 10) {
                    scroll_offset++;
                    draw_explorer(wid);
                }
                // Backspace = go back
                if (ev.key == '\b') {
                    navigate_back();
                    draw_explorer(wid);
                }
            } else if (ev.type == 3) { // Mouse click
                if (ev.key == 1) { // Left click
                    int click_x = ev.x;
                    int click_y = ev.y;

                    // Check Back button click
                    if (click_x >= 6 && click_x <= 34 && click_y >= 4 && click_y <= 22) {
                        if (stack_depth > 0) {
                            navigate_back();
                            draw_explorer(wid);
                        }
                        continue;
                    }

                    // Check file list click
                    int list_top = 46;
                    int row_h = 22;
                    if (click_y >= list_top && click_y < 340 - 20) {
                        int row = (click_y - list_top) / row_h + scroll_offset;
                        if (row >= 0 && row < entry_count) {
                            if (selected == row) {
                                // Second click on the same row!
                                if (entries[row].type == 0) {
                                    // It's a file! Open it!
                                    // Construct full path
                                    char full_path[256];
                                    int p_len = my_strlen(path_bar);
                                    my_strcpy(full_path, path_bar);
                                    if (p_len > 1 && path_bar[p_len - 1] != '/') {
                                        full_path[p_len++] = '/';
                                        full_path[p_len] = '\0';
                                    }
                                    my_strcpy(full_path + p_len, entries[row].name);

                                    // Check extension
                                    int nlen = my_strlen(entries[row].name);
                                    char cmd[512];
                                    if (nlen > 4 && entries[row].name[nlen-4] == '.' && 
                                        entries[row].name[nlen-3] == 'm' && 
                                        entries[row].name[nlen-2] == 'c' && 
                                        entries[row].name[nlen-1] == 't') {
                                        // Run executable directly
                                        my_strcpy(cmd, "jalankan ");
                                        my_strcpy(cmd + 9, full_path);
                                    } else if (nlen > 4 && entries[row].name[nlen-4] == '.' && 
                                               (entries[row].name[nlen-3] == 'w' || entries[row].name[nlen-3] == 'W') && 
                                               (entries[row].name[nlen-2] == 'a' || entries[row].name[nlen-2] == 'A') && 
                                               (entries[row].name[nlen-1] == 'v' || entries[row].name[nlen-1] == 'V')) {
                                        // Play WAV file in mplayer
                                        my_strcpy(cmd, "jalankan /apps/mplayer.mct ");
                                        my_strcpy(cmd + 27, full_path);
                                    } else {
                                        // Open standard files in Notepad
                                        my_strcpy(cmd, "jalankan /apps/notepad.mct ");
                                        my_strcpy(cmd + 27, full_path);
                                    }
                                    sys_exec_cmd(cmd);
                                }
                            } else {
                                selected = row;
                                // Enter directory on click
                                if (entries[row].type == 1) {
                                    navigate_into(entries[row].node_idx, entries[row].name);
                                }
                            }
                        }
                    }
                    draw_explorer(wid);
                }
            } else if (ev.type == 4) { // Scroll wheel
                if (ev.key > 0 && scroll_offset > 0) {
                    // Scroll up
                    for (int s = 0; s < 3 && scroll_offset > 0; s++)
                        scroll_offset--;
                } else if (ev.key < 0 && scroll_offset < entry_count - 10) {
                    // Scroll down
                    for (int s = 0; s < 3 && scroll_offset < entry_count - 10; s++)
                        scroll_offset++;
                }
                draw_explorer(wid);
            }
        }
        sys_yield();
    }
}
