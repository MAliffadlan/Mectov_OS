// notepad.c – Mectov OS Text Editor v3
// Simplified save logic, Save As dialog via status bar
#include "../src/include/syscall.h"

#define BUF_SIZE 4096
#define CW 520
#define CH 380
#define MBAR_H 22
#define SBAR_H 20
#define TEXT_START_Y (MBAR_H + 4)
#define TEXT_END_Y   (CH - SBAR_H - 4)

// App modes
#define MODE_EDIT    0
#define MODE_SAVEAS  1

// Menu states
#define MENU_NONE -1
#define MENU_FILE  0
#define MENU_EDIT  1
#define MENU_HELP  2

// ---- Globals ----
static char buf[BUF_SIZE];   // text buffer
static int  buf_len = 0;
static char filepath[64];    // full VFS path e.g. "home/note.txt"
static int  has_file = 0;    // 1 if we have a filepath
static int  wid = -1;
static int  menu_open = MENU_NONE;
static int  mode = MODE_EDIT;
static int  dirty = 0;       // unsaved changes
static int  save_flash = 0;  // frames to show "Saved!" message

// Save As input
static char sa_input[32];
static int  sa_len = 0;

// ---- String helpers (no libc) ----
static void str_cpy(char* d, const char* s) {
    while (*s) *d++ = *s++; *d = 0;
}
static int str_len(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}
static void str_cat(char* d, const char* s) {
    while (*d) d++; while (*s) *d++ = *s++; *d = 0;
}
static void int_to_str(int n, char* out) {
    if (n == 0) { out[0]='0'; out[1]=0; return; }
    char t[12]; int i=0;
    while(n>0){t[i++]='0'+(n%10);n/=10;}
    int j=0; while(i>0) out[j++]=t[--i]; out[j]=0;
}
static int ends_with_mct(const char* s) {
    int len = str_len(s);
    if (len < 4) return 0;
    return (s[len - 4] == '.' && s[len - 3] == 'm' && s[len - 2] == 'c' && s[len - 1] == 't');
}

// ---- Save/Load using VFS syscalls ----
static void dbg(const char* msg) {
    sys_print(msg, 0x07);
}

static int do_save(void) {
    if (!has_file) { dbg("[NOTEPAD] save: no file\n"); return 0; }

    dbg("[NOTEPAD] save: path=");
    dbg(filepath);
    dbg("\n");

    // Try open existing
    int fd = sys_open(filepath);
    if (fd < 0) {
        dbg("[NOTEPAD] open failed, creating...\n");
        int cr = sys_create_file(filepath);
        dbg("[NOTEPAD] create returned\n");
        (void)cr;
        fd = sys_open(filepath);
    }
    if (fd < 0) { dbg("[NOTEPAD] open still failed!\n"); return 0; }

    dbg("[NOTEPAD] fd ok, writing...\n");
    if (buf_len > 0) {
        int wr = sys_write(fd, buf, buf_len);
        (void)wr;
        dbg("[NOTEPAD] write done\n");
    }
    sys_close(fd);
    dirty = 0;
    save_flash = 200;
    dbg("[NOTEPAD] SAVED OK\n");
    return 1;
}

static void do_load(void) {
    if (!has_file) return;
    int fd = sys_open(filepath);
    if (fd >= 0) {
        int n = sys_read(fd, buf, BUF_SIZE - 1);
        if (n > 0) { buf[n] = 0; buf_len = n; }
        sys_close(fd);
    }
}

// ---- Drawing ----

// Menu bar labels and positions
static const char* menu_labels[] = {"File", "Edit", "Help"};
static const int   menu_x[]      = {14, 54, 94};
#define NUM_MENUS 3

// Menu items
typedef struct { const char* text; int action; } MItem;

static MItem file_menu[] = {
    {"New",        1},
    {"Save",       2},
    {"Save As...", 3},
    {"Exit",       4},
};
#define FILE_MENU_N 4

static MItem edit_menu[] = {
    {"Undo",       10},
    {"Clear All",  11},
};
#define EDIT_MENU_N 2

static MItem help_menu[] = {
    {"About",      20},
};
#define HELP_MENU_N 1

#define DROP_W 100
#define DROP_ITEM_H 18

static void draw_menubar(void) {
    sys_draw_rect(wid, 0, 0, CW, MBAR_H, 0x00F5F5F5);
    sys_draw_rect(wid, 0, MBAR_H, CW, 1, 0x00D0D0D0);
    for (int i = 0; i < NUM_MENUS; i++) {
        if (menu_open == i)
            sys_draw_rect(wid, menu_x[i]-3, 1, 38, MBAR_H-2, 0x00D0E4FF);
        sys_draw_text(wid, menu_x[i], 3, menu_labels[i],
                      menu_open == i ? 0x00003399 : 0x00333333);
    }
}

static void draw_dropdown(int midx) {
    MItem* items; int n;
    if      (midx == 0) { items = file_menu; n = FILE_MENU_N; }
    else if (midx == 1) { items = edit_menu; n = EDIT_MENU_N; }
    else                { items = help_menu; n = HELP_MENU_N; }

    int dx = menu_x[midx] - 3;
    int dy = MBAR_H + 1;
    int dh = n * DROP_ITEM_H + 4;

    // Shadow + bg + border
    sys_draw_rect(wid, dx+2, dy+2, DROP_W, dh, 0x00C0C0C0);
    sys_draw_rect(wid, dx, dy, DROP_W, dh, 0x00F9F9F9);
    sys_draw_rect(wid, dx, dy, DROP_W, 1, 0x00AAAAAA);
    sys_draw_rect(wid, dx, dy+dh-1, DROP_W, 1, 0x00AAAAAA);
    sys_draw_rect(wid, dx, dy, 1, dh, 0x00AAAAAA);
    sys_draw_rect(wid, dx+DROP_W-1, dy, 1, dh, 0x00AAAAAA);

    for (int i = 0; i < n; i++) {
        sys_draw_text(wid, dx+8, dy+3+i*DROP_ITEM_H, items[i].text, 0x00111111);
    }
}

static void draw_statusbar(void) {
    int sy = CH - SBAR_H;
    sys_draw_rect(wid, 0, sy, CW, SBAR_H, 0x00F0F0F0);
    sys_draw_rect(wid, 0, sy, CW, 1, 0x00D0D0D0);

    if (mode == MODE_SAVEAS) {
        sys_draw_text(wid, 6, sy+3, "Menunggu nama berkas...", 0x00003399);
    } else if (save_flash > 0) {
        sys_draw_text(wid, 6, sy+3, "Tersimpan!", 0x00008800);
        if (has_file)
            sys_draw_text(wid, 100, sy+3, filepath, 0x00555555);
    } else {
        // Normal: show file or "Untitled"
        if (has_file) {
            if (dirty) {
                sys_draw_text(wid, 6, sy+3, "*", 0x00CC0000);
                sys_draw_text(wid, 14, sy+3, filepath, 0x00555555);
            } else {
                sys_draw_text(wid, 6, sy+3, filepath, 0x00555555);
            }
        } else {
            sys_draw_text(wid, 6, sy+3, dirty ? "* Untitled" : "Untitled", 0x00555555);
        }
        // Char count
        char cstr[16]; int_to_str(buf_len, cstr);
        char stat[24]; str_cpy(stat, "Ln:"); str_cat(stat, cstr);
        sys_draw_text(wid, CW-70, sy+3, stat, 0x00888888);
    }
}

static void draw_saveas_dialog(void) {
    int dw = 320;
    int dh = 120;
    int dx = (CW - dw) / 2;
    int dy = (CH - dh) / 2;

    // Shadow border
    sys_draw_rect(wid, dx - 1, dy - 1, dw + 2, dh + 2, 0x001E1E2E);
    // Background card
    sys_draw_rect(wid, dx, dy, dw, dh, 0x00E2E8F0); // Card BG
    // Header bar
    sys_draw_rect(wid, dx, dy, dw, 24, 0x0089B4FA); // Blue titlebar
    sys_draw_text(wid, dx + 10, dy + 5, "Simpan Sebagai...", 0x00111111);

    // Label
    sys_draw_text(wid, dx + 16, dy + 38, "Nama berkas (di /home):", 0x00313244);

    // Input text field
    sys_draw_rect(wid, dx + 16, dy + 58, dw - 32, 22, 0x0011111B); // Dark input box
    sys_draw_rect(wid, dx + 16, dy + 58, dw - 32, 1, 0x0045475A);  // Border

    // Input text contents
    char disp[32];
    int i;
    for (i = 0; i < sa_len && i < 28; i++) disp[i] = sa_input[i];
    disp[i] = '_'; disp[i+1] = 0;
    sys_draw_text(wid, dx + 24, dy + 61, disp, 0x00A6E3A1); // Green input text

    // Help Footer
    sys_draw_text(wid, dx + 16, dy + 92, "ENTER = OK  |  ESC = Batal", 0x00585B70);
}

static void draw_all(void) {
    // Clear canvas
    sys_draw_rect(wid, 0, 0, CW, CH, 0x00FFFFFF);

    // Menu bar
    draw_menubar();

    // Text area
    int x = 14, y = TEXT_START_Y;
    char lb[31]; int ll = 0; int sx = x;

    for (int i = 0; i < buf_len; i++) {
        if (buf[i] == '\n') {
            if (ll > 0) { lb[ll]=0; sys_draw_text(wid, sx, y, lb, 0x00111111); ll=0; }
            x = 14; y += 16; sx = x;
            continue;
        }
        if (x + 8 > CW - 6) {
            if (ll > 0) { lb[ll]=0; sys_draw_text(wid, sx, y, lb, 0x00111111); ll=0; }
            x = 14; y += 16; sx = x;
        }
        if (y >= TEXT_END_Y) break;
        if (ll >= 30) {
            lb[ll]=0; sys_draw_text(wid, sx, y, lb, 0x00111111);
            ll=0; sx=x;
        }
        lb[ll++] = buf[i]; x += 8;
    }
    if (ll > 0) { lb[ll]=0; sys_draw_text(wid, sx, y, lb, 0x00111111); }

    // Cursor
    if (mode == MODE_EDIT)
        sys_draw_rect(wid, x, y+1, 2, 14, 0x000055CC);

    // Status bar
    draw_statusbar();

    // Dropdown overlay
    if (menu_open != MENU_NONE)
        draw_dropdown(menu_open);

    // Centered pop-up dialog
    if (mode == MODE_SAVEAS)
        draw_saveas_dialog();

    sys_update_window(wid);
}

// ---- Hit testing ----
static int pt_in(int px, int py, int rx, int ry, int rw, int rh) {
    return px>=rx && px<rx+rw && py>=ry && py<ry+rh;
}

// Execute menu action
static void exec_action(int a) {
    if (a == 1) { // New
        buf_len = 0; buf[0] = 0; has_file = 0; filepath[0] = 0; dirty = 0;
    } else if (a == 2) { // Save
        if (has_file) {
            do_save();
        } else {
            // No filename — enter Save As mode
            mode = MODE_SAVEAS;
            sa_input[0] = 0; sa_len = 0;
        }
    } else if (a == 3) { // Save As
        mode = MODE_SAVEAS;
        sa_input[0] = 0; sa_len = 0;
    } else if (a == 4) { // Exit
        if (dirty && buf_len > 0 && has_file) do_save();
        sys_exit();
    } else if (a == 10) { // Undo (delete last char)
        if (buf_len > 0) { buf_len--; buf[buf_len]=0; dirty=1; }
    } else if (a == 11) { // Clear All
        buf_len = 0; buf[0] = 0; dirty = 1;
    }
}

// Returns action id, or 0 if not hit
static int check_dropdown_click(int mx, int my) {
    if (menu_open < 0) return 0;
    MItem* items; int n;
    if      (menu_open == 0) { items = file_menu; n = FILE_MENU_N; }
    else if (menu_open == 1) { items = edit_menu; n = EDIT_MENU_N; }
    else                     { items = help_menu; n = HELP_MENU_N; }

    int dx = menu_x[menu_open] - 3;
    int dy = MBAR_H + 1;
    for (int i = 0; i < n; i++) {
        int iy = dy + 3 + i * DROP_ITEM_H;
        if (pt_in(mx, my, dx, iy-2, DROP_W, DROP_ITEM_H)) {
            return items[i].action;
        }
    }
    return 0;
}

// ====== MAIN ======
typedef struct { int type; int x, y; int key; } gui_event_t;

void _start() {
    // Check launch argument
    char arg[64];
    int al = sys_get_launch_arg(arg, 64);
    if (al > 0 && arg[0] != '\0' && !ends_with_mct(arg)) {
        str_cpy(filepath, arg);
        has_file = 1;
        do_load();
    } else {
        has_file = 0;
        filepath[0] = 0;
        buf[0] = 0; buf_len = 0;
    }

    char title[48];
    str_cpy(title, "Notepad");
    if (has_file) { str_cat(title, " - "); str_cat(title, filepath); }

    wid = sys_create_window(100, 60, CW, CH, title);
    if (wid < 0) sys_exit();
    draw_all();

    gui_event_t ev;
    while (1) {
        int r = sys_get_event(wid, &ev);
        if (r < 0) { // Window closed
            if (dirty && has_file) do_save();
            sys_exit();
        }

        if (r > 0) {
            if (ev.type == 1) { // Paint
                draw_all();

            } else if (ev.type == 3 && (ev.key & 1)) { // Left click
                int mx = ev.x, my = ev.y;

                // If dropdown is open, check dropdown first
                if (menu_open != MENU_NONE) {
                    int act = check_dropdown_click(mx, my);
                    menu_open = MENU_NONE;
                    if (act > 0) exec_action(act);
                    draw_all();

                } else if (my < MBAR_H) {
                    // Clicked on menu bar
                    int hit = -1;
                    for (int i = 0; i < NUM_MENUS; i++) {
                        if (pt_in(mx, my, menu_x[i]-3, 0, 38, MBAR_H)) {
                            hit = i; break;
                        }
                    }
                    menu_open = hit;
                    draw_all();
                }
                // Clicks elsewhere just close menus (already closed above)

            } else if (ev.type == 2) { // Key press
                char c = (char)ev.key;

                // Close any open menu on keypress
                if (menu_open != MENU_NONE) {
                    menu_open = MENU_NONE;
                    // Don't consume the key — let it pass through
                }

                // Global shortcuts (works in both EDIT and SAVEAS modes)
                if (c == 17) { // Ctrl+Q = Quit/Exit
                    exec_action(4);
                } else if (c == 14) { // Ctrl+N = New
                    mode = MODE_EDIT;
                    exec_action(1);
                    draw_all();
                } else if (mode == MODE_SAVEAS) {
                    // ---- Save As input mode ----
                    if (c == '\n' || c == 19) { // Enter or Ctrl+S to save
                        if (sa_len > 0) {
                            // Build full path
                            str_cpy(filepath, "home/");
                            str_cat(filepath, sa_input);
                            has_file = 1;
                            do_save();
                            mode = MODE_EDIT;
                        }
                    } else if (c == 27) { // ESC = cancel
                        mode = MODE_EDIT;
                    } else if (c == '\b') {
                        if (sa_len > 0) { sa_len--; sa_input[sa_len] = 0; }
                    } else if (c >= 32 && c < 127 && sa_len < 24) {
                        sa_input[sa_len++] = c;
                        sa_input[sa_len] = 0;
                    }
                    draw_all();

                } else {
                    // ---- Normal edit mode ----
                    if (c == 27) { // ESC = save & exit
                        if (dirty && has_file) do_save();
                        sys_exit();
                    } else if (c == 19) { // Ctrl+S = Save
                        exec_action(2);
                        draw_all();
                    } else if (c == '\b') {
                        if (buf_len > 0) { buf_len--; buf[buf_len]=0; dirty=1; draw_all(); }
                    } else if (c == '\n' && buf_len < BUF_SIZE-1) {
                        buf[buf_len++] = '\n'; buf[buf_len] = 0; dirty=1; draw_all();
                    } else if (c >= 32 && c < 127 && buf_len < BUF_SIZE-1) {
                        buf[buf_len++] = c; buf[buf_len] = 0; dirty=1; draw_all();
                    }
                }
            }
        }

        // Tick-down saved indicator
        if (save_flash > 0) {
            save_flash--;
            if (save_flash == 0) draw_all();
        }

        sys_yield();
    }
}
