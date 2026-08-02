#include "src/include/syscall.h"

#define NANO_BUF_SIZE 4096

int ed_a = 0; 
char ed_b[NANO_BUF_SIZE]; 
char ed_fn[128]; 
int ed_c = 0;
static int nano_win_id = -1;

// safe_strlen to avoid includes
static int my_strlen(const char* s) {
    int i = 0;
    while(s[i]) i++;
    return i;
}

static void my_strcpy(char* d, const char* s) {
    int i = 0;
    while(s[i]) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static void save_file(void) {
    if (ed_fn[0] == '\0') return;
    
    // Try to open existing file
    int fd = sys_open(ed_fn);
    if (fd < 0) {
        // File doesn't exist — create it first
        sys_create_file(ed_fn);
        fd = sys_open(ed_fn);
    }
    
    if (fd >= 0) {
        sys_write(fd, ed_b, ed_c);
        sys_close(fd);
    }
}

static void nano_draw(int wid) {
    int cw = 420;
    int ch = 320;
    sys_draw_rect(wid, 0, 0, cw, ch, 0x001E1E2E); // GUI_BG
    sys_draw_rect(wid, 0, 0, cw, 1, 0x0045475A);  // Border
    sys_draw_rect(wid, 0, ch-1, cw, 1, 0x0045475A);
    sys_draw_rect(wid, 0, 0, 1, ch, 0x0045475A);
    sys_draw_rect(wid, cw-1, 0, 1, ch, 0x0045475A);
    
    // Status bar at bottom
    sys_draw_rect(wid, 0, ch - 18, cw, 18, 0x00313244);
    sys_draw_text(wid, 4, ch - 15, ed_fn, 0x00A6ADC8);
    sys_draw_text(wid, cw - 120, ch - 15, "ESC=Save&Quit", 0x0089B4FA);
    
    int x = 4, y = 4;
    
    // Group characters into a line buffer before drawing to save IPC messages
    char line_buf[128];
    int line_len = 0;
    int start_x = x;
    
    for(int i = 0; i < ed_c; i++) {
        if(ed_b[i] == '\n') {
            if (line_len > 0) {
                line_buf[line_len] = '\0';
                sys_draw_text(wid, start_x, y, line_buf, 0x00CDD6F4); // GUI_TEXT
                line_len = 0;
            }
            x = 4; y += 16;
            start_x = x;
            continue;
        }
        if(x + 8 > cw - 4) {
            if (line_len > 0) {
                line_buf[line_len] = '\0';
                sys_draw_text(wid, start_x, y, line_buf, 0x00CDD6F4);
                line_len = 0;
            }
            x = 4; y += 16;
            start_x = x;
        }
        if(y + 16 > ch - 22) break; // Out of bounds (leave room for status bar)
        
        line_buf[line_len++] = ed_b[i];
        x += 8;
    }
    
    if (line_len > 0) {
        line_buf[line_len] = '\0';
        sys_draw_text(wid, start_x, y, line_buf, 0x00CDD6F4);
    }
    
    // Draw cursor
    sys_draw_rect(wid, x, y + 14, 8, 2, 0x00A6E3A1); // Green cursor
    sys_update_window(wid);
}

typedef struct {
    int type; // 1 = paint, 2 = key, 3 = mouse
    int x, y;
    int key;
} gui_event_t;

void _start() {
    // Get the launch argument (filename) from the kernel
    char arg[128];
    int arg_len = sys_get_launch_arg(arg, sizeof(arg));
    
    if (arg_len > 0 && arg[0] != '\0') {
        my_strcpy(ed_fn, arg);
    } else {
        my_strcpy(ed_fn, "home/note.txt");
    }
    
    int fd = sys_open(ed_fn);
    if (fd >= 0) {
        int sz = sys_read(fd, ed_b, NANO_BUF_SIZE - 1);
        if (sz > 0) {
            ed_b[sz] = '\0';
            ed_c = sz;
        }
        sys_close(fd);
    } else {
        ed_b[0] = '\0';
        ed_c = 0;
    }

    char title[64];
    my_strcpy(title, "Editor: ");
    my_strcpy(title + 8, ed_fn);
    
    int wid = sys_create_window(150, 100, 420, 320, title);
    if (wid < 0) sys_exit();
    
    nano_win_id = wid;
    nano_draw(wid);
    
    gui_event_t ev;
    int tick = 0;
    
    while (1) {
        int got = sys_get_event(wid, &ev);
        if (got < 0) {
            // Window was closed via mouse X button — save before exit
            save_file();
            sys_exit();
        }
        
        if (got > 0) {
            if (ev.type == 1) {
                nano_draw(wid);
            } else if (ev.type == 2) {
                char c = ev.key;
                if (c == 27) { // ESC -> Save and exit
                    save_file();
                    sys_exit();
                } else if (c == '\b' && ed_c > 0) {
                    ed_c--;
                    ed_b[ed_c] = '\0';
                    nano_draw(wid);
                } else if (c != 0 && ed_c < NANO_BUF_SIZE - 1 && c != 27 && c != '\b') {
                    ed_b[ed_c++] = c;
                    ed_b[ed_c] = '\0';
                    nano_draw(wid);
                }
            }
        }
        
        tick++;
        if (tick % 5000 == 0) {
            nano_draw(wid); // Redraw for cursor blink maybe
        }
        
        sys_yield();
    }
}
