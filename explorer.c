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

// --- Dialog overlay state ---
static int dialog_mode = 0; // 0 = none, 1 = new file, 2 = new folder, 3 = rename
static char dialog_input[32] = "";
static int dialog_input_len = 0;

// --- Context menu state ---
static int explorer_ctx_open = 0;
static int explorer_ctx_x = 0;
static int explorer_ctx_y = 0;

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

// --- Helper to build full path for an item ---
static void build_full_path(char* out, const char* filename) {
    int plen = my_strlen(path_bar);
    my_strcpy(out, path_bar);
    if (plen > 1 && path_bar[plen - 1] != '/') {
        out[plen++] = '/';
        out[plen] = '\0';
    }
    my_strcpy(out + plen, filename);
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

static void draw_dialog(int wid) {
    int dw = 280;
    int dh = 100;
    int dx = (400 - dw) / 2;
    int dy = (340 - dh) / 2;

    // Shadow
    sys_draw_rect(wid, dx - 1, dy - 1, dw + 2, dh + 2, 0x0011111B);
    // Background card
    sys_draw_rect(wid, dx, dy, dw, dh, 0x00E2E8F0);
    // Titlebar
    sys_draw_rect(wid, dx, dy, dw, 22, 0x0089B4FA);
    if (dialog_mode == 1) {
        sys_draw_text(wid, dx + 8, dy + 3, "Berkas Baru", 0x00111111);
    } else if (dialog_mode == 2) {
        sys_draw_text(wid, dx + 8, dy + 3, "Direktori Baru", 0x00111111);
    } else {
        sys_draw_text(wid, dx + 8, dy + 3, "Ubah Nama", 0x00111111);
    }

    // Input text field box
    sys_draw_rect(wid, dx + 12, dy + 38, dw - 24, 20, 0x0011111B);
    
    // Display string
    char disp[36];
    int i = 0;
    for (; i < dialog_input_len && i < 28; i++) {
        disp[i] = dialog_input[i];
    }
    disp[i++] = '_';
    disp[i] = '\0';
    sys_draw_text(wid, dx + 18, dy + 40, disp, 0x00A6E3A1);

    // Help Footer
    sys_draw_text(wid, dx + 12, dy + 74, "ENTER = OK  |  ESC = Batal", 0x00585B70);
}

static void draw_explorer(int wid) {
    int cw = 400, ch = 340;
    sys_draw_rect(wid, 0, 0, cw, ch, 0x001E1E2E);

    // ====== Header bar (24px tall) ======
    sys_draw_rect(wid, 0, 0, cw, 24, 0x00181825);

    // Back button [←]
    int back_x = 6, back_y = 3;
    uint32_t back_col = (stack_depth > 0) ? 0x00313244 : 0x00252535;
    sys_draw_rect(wid, back_x, back_y, 28, 18, back_col);
    uint32_t arrow_col = (stack_depth > 0) ? 0x00CDD6F4 : 0x00585B70;
    sys_draw_text(wid, back_x + 8, back_y + 1, "<", arrow_col);

    // Path bar
    sys_draw_rect(wid, 40, 3, cw - 110, 18, 0x00252535);
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
    sys_draw_text(wid, 44, 4, display_path, 0x0094E2D5);

    // Ring 3 badge
    sys_draw_text(wid, cw - 60, 4, "Ring 3", 0x00F9E2AF);

    // ====== Toolbar (y=24 to 44) ======
    sys_draw_rect(wid, 0, 24, cw, 20, 0x0011111B);
    
    // Draw "+File" button
    sys_draw_rect(wid, 6, 25, 60, 17, 0x00313244);
    sys_draw_text(wid, 12, 26, "+File", 0x00CDD6F4);

    // Draw "+Folder" button
    sys_draw_rect(wid, 72, 25, 70, 17, 0x00313244);
    sys_draw_text(wid, 78, 26, "+Folder", 0x00CDD6F4);

    // Draw "Hapus" button (only highlighted if selected >= 0)
    uint32_t del_bg = (selected >= 0) ? 0x00F38BA8 : 0x00252535;
    uint32_t del_fg = (selected >= 0) ? 0x0011111B : 0x00585B70;
    sys_draw_rect(wid, 148, 25, 55, 17, del_bg);
    sys_draw_text(wid, 154, 26, "Hapus", del_fg);

    // Column headers
    sys_draw_rect(wid, 0, 44, cw, 20, 0x00181825);
    sys_draw_text(wid, 28, 47, "Name", 0x00A6ADC8);
    sys_draw_text(wid, 230, 47, "Type", 0x00A6ADC8);
    sys_draw_text(wid, 310, 47, "Size", 0x00A6ADC8);
    sys_draw_rect(wid, 0, 64, cw, 1, 0x00313244);

    // ====== File list ======
    int list_top = 66;
    int row_h = 22;
    int max_rows = (ch - list_top - 20) / row_h;
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

    // Dialog Overlay
    if (dialog_mode != 0) {
        draw_dialog(wid);
    }

    // Explorer Context Menu Overlay
    if (explorer_ctx_open) {
        int dx = explorer_ctx_x;
        int dy = explorer_ctx_y;
        int dw = 90;
        int dh = 58; // 3 items * 18 + 4 = 58
        
        sys_draw_rect(wid, dx - 1, dy - 1, dw + 2, dh + 2, 0x0011111B);
        sys_draw_rect(wid, dx, dy, dw, dh, 0x00E2E8F0);
        
        const char* ctx_items[] = {
            "Buka",
            "Hapus",
            "Ubah Nama"
        };
        
        for (int i = 0; i < 3; i++) {
            int iy = dy + 2 + i * 18;
            sys_draw_text(wid, dx + 8, iy + 3, ctx_items[i], 0x0011111B);
        }
    }

    sys_update_window(wid);
}

void _start() {
    sys_print("starting explorer...\n", 0x0A);
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
                if (ev.key == 27) { // ESC
                    if (dialog_mode != 0) {
                        dialog_mode = 0;
                        draw_explorer(wid);
                    } else if (explorer_ctx_open) {
                        explorer_ctx_open = 0;
                        draw_explorer(wid);
                    } else {
                        sys_exit();
                    }
                }

                if (dialog_mode != 0) {
                    char c = (char)ev.key;
                    if (c == '\n') {
                        if (dialog_input_len > 0) {
                            char full_path[256];
                            build_full_path(full_path, dialog_input);
                            if (dialog_mode == 1) {
                                sys_create_file(full_path);
                            } else if (dialog_mode == 2) {
                                sys_mkdir(full_path);
                            } else if (dialog_mode == 3) {
                                if (selected >= 0) {
                                    char old_path[256];
                                    build_full_path(old_path, entries[selected].name);
                                    sys_rename_file(old_path, full_path);
                                }
                            }
                            dialog_mode = 0;
                            refresh_entries();
                            draw_explorer(wid);
                        }
                    } else if (c == '\b') {
                        if (dialog_input_len > 0) {
                            dialog_input_len--;
                            dialog_input[dialog_input_len] = '\0';
                            draw_explorer(wid);
                        }
                    } else if (c >= ' ' && c <= '~' && dialog_input_len < 24) {
                        if (c != '/' && c != '\\') {
                            dialog_input[dialog_input_len++] = c;
                            dialog_input[dialog_input_len] = '\0';
                            draw_explorer(wid);
                        }
                    }
                } else {
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
                }
            } else if (ev.type == 3) { // Mouse click
                int click_x = ev.x;
                int click_y = ev.y;

                if (ev.key == 1) { // Left click
                    if (dialog_mode != 0) {
                        // Clicks are ignored during dialog mode modal
                        continue;
                    }

                    if (explorer_ctx_open) {
                        int dx = explorer_ctx_x;
                        int dy = explorer_ctx_y;
                        int dw = 90;
                        int dh = 58;
                        if (click_x >= dx && click_x < dx + dw && click_y >= dy && click_y < dy + dh) {
                            int item = (click_y - dy - 2) / 18;
                            if (item == 0) { // Buka
                                if (selected >= 0 && entries[selected].type == 0) {
                                    char full_path[256];
                                    build_full_path(full_path, entries[selected].name);
                                    int nlen = my_strlen(entries[selected].name);
                                    char cmd[512];
                                    if (nlen > 4 && entries[selected].name[nlen-4] == '.' && 
                                        entries[selected].name[nlen-3] == 'm' && 
                                        entries[selected].name[nlen-2] == 'c' && 
                                        entries[selected].name[nlen-1] == 't') {
                                        my_strcpy(cmd, "jalankan ");
                                        my_strcpy(cmd + 9, full_path);
                                    } else if (nlen > 4 && entries[selected].name[nlen-4] == '.' && 
                                               (entries[selected].name[nlen-3] == 'w' || entries[selected].name[nlen-3] == 'W') && 
                                               (entries[selected].name[nlen-2] == 'a' || entries[selected].name[nlen-2] == 'A') && 
                                               (entries[selected].name[nlen-1] == 'v' || entries[selected].name[nlen-1] == 'V')) {
                                        my_strcpy(cmd, "jalankan /apps/mplayer.mct ");
                                        my_strcpy(cmd + 27, full_path);
                                    } else {
                                        my_strcpy(cmd, "jalankan /apps/notepad.mct ");
                                        my_strcpy(cmd + 27, full_path);
                                    }
                                    sys_exec_cmd(cmd);
                                } else if (selected >= 0 && entries[selected].type == 1) { // Directory
                                    navigate_into(entries[selected].node_idx, entries[selected].name);
                                }
                            } else if (item == 1) { // Hapus
                                if (selected >= 0) {
                                    char full_path[256];
                                    build_full_path(full_path, entries[selected].name);
                                    sys_delete_file(full_path);
                                    selected = -1;
                                    refresh_entries();
                                }
                            } else if (item == 2) { // Ubah Nama
                                if (selected >= 0) {
                                    dialog_mode = 3; // Rename modal
                                    my_strcpy(dialog_input, entries[selected].name);
                                    dialog_input_len = my_strlen(dialog_input);
                                }
                            }
                        }
                        explorer_ctx_open = 0;
                        draw_explorer(wid);
                        continue;
                    }

                    // Check Back button click
                    if (click_x >= 6 && click_x <= 34 && click_y >= 3 && click_y <= 21) {
                        if (stack_depth > 0) {
                            navigate_back();
                            draw_explorer(wid);
                        }
                        continue;
                    }

                    // Check "+File" click
                    if (click_x >= 6 && click_x <= 66 && click_y >= 25 && click_y <= 42) {
                        dialog_mode = 1;
                        dialog_input[0] = '\0';
                        dialog_input_len = 0;
                        draw_explorer(wid);
                        continue;
                    }

                    // Check "+Folder" click
                    if (click_x >= 72 && click_x <= 142 && click_y >= 25 && click_y <= 42) {
                        dialog_mode = 2;
                        dialog_input[0] = '\0';
                        dialog_input_len = 0;
                        draw_explorer(wid);
                        continue;
                    }

                    // Check "Hapus" click
                    if (click_x >= 148 && click_x <= 203 && click_y >= 25 && click_y <= 42) {
                        if (selected >= 0) {
                            char full_path[256];
                            build_full_path(full_path, entries[selected].name);
                            sys_delete_file(full_path);
                            selected = -1;
                            refresh_entries();
                            draw_explorer(wid);
                        }
                        continue;
                    }

                    // Check file list click
                    int list_top = 66;
                    int row_h = 22;
                    if (click_y >= list_top && click_y < 340 - 20) {
                        int row = (click_y - list_top) / row_h + scroll_offset;
                        if (row >= 0 && row < entry_count) {
                            if (selected == row) {
                                // Second click on the same row!
                                if (entries[row].type == 0) {
                                    // It's a file! Open it!
                                    char full_path[256];
                                    build_full_path(full_path, entries[row].name);

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
                } else if (ev.key == 2) { // Right click
                    if (dialog_mode != 0) continue;

                    if (explorer_ctx_open) {
                        explorer_ctx_open = 0;
                    }

                    // Check file list right-click
                    int list_top = 66;
                    int row_h = 22;
                    if (click_y >= list_top && click_y < 340 - 20) {
                        int row = (click_y - list_top) / row_h + scroll_offset;
                        if (row >= 0 && row < entry_count) {
                            selected = row;
                            explorer_ctx_open = 1;
                            explorer_ctx_x = click_x;
                            explorer_ctx_y = click_y;
                            if (explorer_ctx_x + 90 > 400) explorer_ctx_x = 400 - 90;
                            if (explorer_ctx_y + 58 > 320) explorer_ctx_y = 320 - 58;
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
