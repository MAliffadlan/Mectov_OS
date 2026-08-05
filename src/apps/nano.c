#include "../include/apps.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/vfs.h"
#include "../include/utils.h"
#include "../include/wm.h"
#include "../include/timer.h"

#define NANO_BUF_SIZE 4096

int ed_a = 0; 
char ed_b[NANO_BUF_SIZE]; 
char ed_fn[MAX_PATH]; 
int ed_c = 0;
static int nano_win_id = -1;

static void nano_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    draw_rect(cx, cy, cw, ch, GUI_BG);
    draw_rect_border(cx, cy, cw, ch, GUI_BORDER2);
    
    int footer_h = 22;
    int max_text_y = ch - footer_h - 4;
    
    int x = 4, y = 4;
    for(int i = 0; i < ed_c; i++) {
        if(ed_b[i] == '\n') { x = 4; y += 16; continue; }
        if(x + 8 > cw - 4) { x = 4; y += 16; }
        if(y + 16 > max_text_y) break; // Out of bounds (above footer)
        draw_char_px(cx + x, cy + y, ed_b[i], GUI_TEXT, GUI_BG);
        x += 8;
    }
    // Draw cursor (only if it falls above the footer)
    if (y + 16 <= max_text_y) {
        if ((get_ticks() / 500) & 1) draw_rect(cx + x, cy + y + 14, 8, 2, 0x0000FF88);
    }

    // Status Bar / Footer at the bottom
    int footer_y = cy + ch - footer_h;
    draw_rect(cx + 1, footer_y - 1, cw - 2, footer_h, 0x00151522); // Sleek dark bar
    draw_line(cx + 1, footer_y - 1, cx + cw - 2, footer_y - 1, GUI_BORDER2); // Top divider line

    // Draw shortcut info
    draw_string_px(cx + 8, footer_y + 3, "ESC: Save & Exit", GUI_TEAL, 0);

    // Draw character count
    char count_buf[32];
    strcpy(count_buf, "Karakter: ");
    int val = ed_c;
    int ci = 10;
    if (val == 0) {
        count_buf[ci++] = '0';
    } else {
        char rev[8]; int rl = 0;
        while (val > 0) { rev[rl++] = '0' + (val % 10); val /= 10; }
        while (rl > 0) count_buf[ci++] = rev[--rl];
    }
    count_buf[ci] = '\0';
    draw_string_px(cx + cw - 120, footer_y + 3, count_buf, GUI_DIM, 0);
}

static void nano_key(int id, char c, uint8_t sc) {
    if (sc == 1) { // ESC -> Save and exit
        sa_ex_ed();
        wm_close(id);
        return;
    }
    if (c == '\b' && ed_c > 0) {
        ed_c--;
        ed_b[ed_c] = '\0';
    } else if (c != 0 && ed_c < NANO_BUF_SIZE - 1) {
        ed_b[ed_c++] = c;
        ed_b[ed_c] = '\0';
    }
    wm_invalidate(id);
}

static void nano_tick(int id) {
    if (wm_is_open(id)) {
        wm_invalidate(id);
    }
}

void st_ed(const char* f) { 
    if (nano_win_id >= 0 && wm_is_open(nano_win_id)) { wm_raise(nano_win_id); return; }
    
    // Resolve to absolute path immediately in caller's task context
    char abs_path[MAX_PATH];
    vfs_resolve_path(f, abs_path, MAX_PATH);
    strcpy(ed_fn, abs_path); 
    
    // Try to read file via new VFS API
    int sz = vfs_read_file(ed_fn, ed_b, NANO_BUF_SIZE - 1);
    if (sz > 0) {
        ed_b[sz] = '\0';
        ed_c = sz;
    } else {
        ed_b[0] = '\0';
        ed_c = 0;
    }
    ed_a = 1; 
    
    char title[64];
    strcpy(title, "Editor: ");
    strcpy(title + 8, f);
    
    nano_win_id = wm_open(150, 100, 420, 320, title, nano_draw, nano_key, nano_tick, 0);
}

void sa_ex_ed() { 
    // Save via new VFS API
    if (vfs_get_node(ed_fn) < 0) {
        vfs_create_file(ed_fn);
    }
    vfs_write_file(ed_fn, ed_b, ed_c);
    vfs_save(); 
    ed_a = 0; 
    nano_win_id = -1;
}
