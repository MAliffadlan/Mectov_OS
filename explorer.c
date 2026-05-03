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

static void draw_explorer(int wid) {
    int cw = 360, ch = 300;
    sys_draw_rect(wid, 0, 0, cw, ch, 0x001E1E2E);

    // Header bar
    sys_draw_rect(wid, 0, 0, cw, 24, 0x00181825);
    sys_draw_text(wid, 8,  4, "Name", 0x00A6ADC8);
    sys_draw_text(wid, 200, 4, "Type", 0x00A6ADC8);
    sys_draw_text(wid, 280, 4, "Size", 0x00A6ADC8);
    sys_draw_rect(wid, 0, 24, cw, 1, 0x00313244);
    
    // Ring 3 badge
    sys_draw_text(wid, cw - 60, 4, "Ring 3", 0x00F9E2AF);

    int max_rows = (ch - 24) / 20;
    int y = 26;

    for (int i = scroll_offset; i < entry_count && (i - scroll_offset) < max_rows; i++) {
        dir_entry_t* e = &entries[i];
        uint32_t row_bg = (i == selected) ? 0x002A2A3E : ((i % 2 == 0) ? 0x001E1E2E : 0x00181825);
        sys_draw_rect(wid, 0, y, cw, 20, row_bg);

        // Icon indicator
        uint32_t icon_col = 0x00FFBB55; // file orange
        if (e->type == 1) icon_col = 0x0089B4FA; // dir blue
        else if (e->type == 2) icon_col = 0x00A6E3A1; // dev green
        sys_draw_rect(wid, 8, y + 3, 14, 14, icon_col);

        // Name
        sys_draw_text(wid, 28, y + 2, e->name, 0x00CDD6F4);

        // Type
        const char* tstr = "File";
        if (e->type == 1) tstr = "Dir";
        else if (e->type == 2) tstr = "Dev";
        sys_draw_text(wid, 200, y + 2, tstr, 0x006C7086);

        // Size
        if (e->type == 0) { // Only for files
            char sbuf[16];
            itoa_simple(e->size, sbuf);
            int slen = 0;
            while(sbuf[slen]) slen++;
            sbuf[slen++] = ' ';
            sbuf[slen++] = 'B';
            sbuf[slen] = '\0';
            sys_draw_text(wid, 280, y + 2, sbuf, 0x007F849C);
        } else {
            sys_draw_text(wid, 280, y + 2, "-", 0x007F849C);
        }

        y += 20;
    }

    if (entry_count == 0) {
        sys_draw_text(wid, 8, 30, "Direktori kosong.", 0x006C7086);
    }
    
    // Status bar
    sys_draw_rect(wid, 0, ch - 18, cw, 18, 0x00181825);
    char cbuf[32];
    itoa_simple(entry_count, cbuf);
    int clen = 0;
    while(cbuf[clen]) clen++;
    cbuf[clen++] = ' '; cbuf[clen++] = 'i'; cbuf[clen++] = 't';
    cbuf[clen++] = 'e'; cbuf[clen++] = 'm'; cbuf[clen++] = 's';
    cbuf[clen] = '\0';
    sys_draw_text(wid, 8, ch - 16, cbuf, 0x006C7086);

    sys_update_window(wid);
}

void _start() {
    int wid = sys_create_window(100, 100, 360, 300, "Explorer (Ring 3)");
    if (wid < 0) sys_exit();

    refresh_entries();
    draw_explorer(wid);

    gui_event_t ev;

    while (1) {
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 1) { // Paint
                draw_explorer(wid);
            } else if (ev.type == 2) { // Key
                if (ev.key == 27) sys_exit(); // ESC
                if ((ev.key == 'w' || ev.key == 'W') && scroll_offset > 0) {
                    scroll_offset--;
                    draw_explorer(wid);
                }
                if ((ev.key == 's' || ev.key == 'S') && scroll_offset < entry_count - 14) {
                    scroll_offset++;
                    draw_explorer(wid);
                }
            } else if (ev.type == 3) { // Mouse click
                if (ev.key == 1) { // Left click
                    int click_y = ev.y;
                    if (click_y >= 26 && click_y < 300 - 18) {
                        int row = (click_y - 26) / 20 + scroll_offset;
                        if (row >= 0 && row < entry_count) {
                            selected = row;
                            // Double-click into directory (just enter on click for now)
                            if (entries[row].type == 1) {
                                current_parent = entries[row].node_idx;
                                refresh_entries();
                            }
                        }
                    }
                    draw_explorer(wid);
                }
            }
        }
        sys_yield();
    }
}
