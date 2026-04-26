#include "../include/apps.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/vfs.h"
#include "../include/utils.h"
#include "../include/wm.h"
#include "../include/timer.h"

int ed_a = 0; 
char ed_b[MAX_FILE_SIZE]; 
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
    if (get_ticks() & 16) draw_rect(cx + x, cy + y + 14, 8, 2, 0x0000FF88);
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
    } else if (c != 0 && ed_c < MAX_FILE_SIZE - 1) {
        ed_b[ed_c++] = c;
        ed_b[ed_c] = '\0';
    }
}

static void nano_tick(int id) { (void)id; }

void st_ed(const char* f) { 
    if (nano_win_id >= 0 && wm_is_open(nano_win_id)) { wm_raise(nano_win_id); return; }
    strcpy(ed_fn, f); 
    int i = vfs_find(f); 
    if (i >= 0) { 
        strcpy(ed_b, fs[i].data); 
        ed_c = fs[i].size; 
    } else { 
        ed_b[0] = '\0'; 
        ed_c = 0; 
    } 
    ed_a = 1; 
    
    char title[64];
    strcpy(title, "Editor: ");
    strcpy(title + 8, f);
    
    nano_win_id = wm_open(150, 100, 420, 320, title, nano_draw, nano_key, nano_tick);
}

void sa_ex_ed() { 
    int i = vfs_find(ed_fn); 
    if (i == -1) i = vfs_create(ed_fn); 
    if (i >= 0) { 
        strcpy(fs[i].data, ed_b); 
        fs[i].size = ed_c; 
    } 
    vfs_save(); 
    ed_a = 0; 
    nano_win_id = -1;
}
