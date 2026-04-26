#include "../include/apps.h"
#include "../include/vga.h"
#include "../include/wm.h"
#include "../include/vfs.h"
#include "../include/utils.h"

int explorer_win_id = -1;
int explorer_open = 0;

static void explorer_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    draw_rect(cx, cy, cw, ch, GUI_BG); // Clear

    // Header
    draw_rect(cx, cy, cw, 24, GUI_BORDER2);
    draw_string_px(cx + 8, cy + 4, "Name", GUI_TEXT, GUI_BORDER2);
    draw_string_px(cx + 180, cy + 4, "Size", GUI_TEXT, GUI_BORDER2);
    draw_rect(cx, cy + 24, cw, 1, GUI_BORDER); // Separator

    int y_offset = 30;
    int file_count = 0;

    for (int i = 0; i < MAX_FILES; i++) {
        if (fs[i].in_use) {
            // Hover effect can be complex without tracking mouse move, so we skip it.
            // Draw File Icon (a simple square)
            draw_rect(cx + 8, cy + y_offset, 16, 16, 0x00FFBB55); // Orange folder/file
            draw_rect(cx + 10, cy + y_offset + 2, 12, 12, 0x00FFDDAA);

            // File Name
            draw_string_px(cx + 32, cy + y_offset, fs[i].name, GUI_TEXT, GUI_BG);

            // Size
            char sz[16];
            int size = fs[i].size;
            int idx = 0;
            if (size == 0) { sz[idx++] = '0'; }
            else {
                char rev[16]; int r = 0;
                while (size > 0) { rev[r++] = '0' + (size % 10); size /= 10; }
                while (r > 0) { sz[idx++] = rev[--r]; }
            }
            sz[idx++] = ' '; sz[idx++] = 'B'; sz[idx] = '\0';

            draw_string_px(cx + 180, cy + y_offset, sz, 0x00666666, GUI_BG);

            y_offset += 24;
            file_count++;
        }
    }

    if (file_count == 0) {
        draw_string_px(cx + 8, cy + y_offset, "No files found. Disk is empty.", 0x00888888, GUI_BG);
    }
}

static void explorer_key(int id, char c, uint8_t sc) {
    (void)id; (void)c; (void)sc;
    // Keyboard navigation not implemented
}

static void explorer_mouse(int id, int cx, int cy, int btn) {
    (void)id;
    if (btn == 1) { // Left click
        if (cy >= 30) {
            int index = (cy - 30) / 24;
            // Find the nth file
            int current = 0;
            for (int i = 0; i < MAX_FILES; i++) {
                if (fs[i].in_use) {
                    if (current == index) {
                        // Open this file in nano
                        st_ed(fs[i].name);
                        return;
                    }
                    current++;
                }
            }
        }
    }
}

static void explorer_tick(int id) {
    if (!wm_is_open(id)) {
        explorer_open = 0;
        explorer_win_id = -1;
    }
}

void open_explorer_app() {
    if (explorer_open && wm_is_open(explorer_win_id)) { wm_raise(explorer_win_id); return; }
    explorer_win_id = wm_open(100, 100, 320, 240, "File Explorer", explorer_draw, explorer_key, explorer_tick, explorer_mouse);
    explorer_open = (explorer_win_id >= 0);
}
