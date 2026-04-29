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
char ed_fn[MAX_FILENAME]; 
int ed_c = 0;
static int nano_win_id = -1;

static void nano_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    draw_rect(cx, cy, cw, ch, GUI_BG);
    draw_rect_border(cx, cy, cw, ch, GUI_BORDER2);
    
    int x = 4, y = 4;
    for(int i = 0; i < ed_c; i++) {
        if(ed_b[i] == '\n') { x = 4; y += 16; continue; }
        if(x + 8 > cw - 4) { x = 4; y += 16; }
        if(y + 16 > ch - 4) break; // Out of bounds
        draw_char_px(cx + x, cy + y, ed_b[i], GUI_TEXT, GUI_BG);
        x += 8;
    }
    // Draw cursor
    if ((get_ticks() / 500) & 1) draw_rect(cx + x, cy + y + 14, 8, 2, 0x0000FF88);
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
}

static void nano_tick(int id) { (void)id; }

void st_ed(const char* f) { 
    if (nano_win_id >= 0 && wm_is_open(nano_win_id)) { wm_raise(nano_win_id); return; }
    strcpy(ed_fn, f); 
    
    // Try to read file via new VFS API
    int sz = vfs_read_file(f, ed_b, NANO_BUF_SIZE - 1);
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
    vfs_write_file(ed_fn, ed_b, ed_c);
    vfs_save(); 
    ed_a = 0; 
    nano_win_id = -1;
}
