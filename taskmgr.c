#include "src/include/syscall.h"

#define MAX_TASKS 64
#define MAX_WINDOWS 8
#define ROW_HEIGHT 20
#define HEADER_H 24
#define BTN_H 30
#define LIST_Y (HEADER_H)

typedef struct {
    int is_window; // 0 = task, 1 = window
    int id;        // tid or win_id
    char name[32]; // App name or "Kernel/User Task"
    int ring;      // 0 or 3
    int state;     // 1=Running/Active, 2=Ready/Hidden
    int priority;  // Just for display
} TmRow;

static TmRow rows[128];
static int num_rows = 0;
static int tm_selected_row = -1;

static void refresh_list() {
    num_rows = 0;
    
    // 1. Add GUI Windows
    sys_win_info_t wins[MAX_WINDOWS];
    int win_count = sys_get_windows(wins, MAX_WINDOWS);
    for (int i = 0; i < win_count; i++) {
        TmRow* r = &rows[num_rows++];
        r->is_window = 1;
        r->id = wins[i].id;
        
        int j = 0;
        while (wins[i].title[j] && j < 31) {
            r->name[j] = wins[i].title[j];
            j++;
        }
        r->name[j] = '\0';
        
        r->ring = wins[i].owner_ring;
        r->state = wins[i].minimized ? 2 : 1;
        r->priority = 1;
    }
    
    // 2. Add OS Tasks
    sys_task_info_t tasks[MAX_TASKS];
    int task_count = sys_get_tasks(tasks, MAX_TASKS);
    for (int i = 0; i < task_count; i++) {
        TmRow* r = &rows[num_rows++];
        r->is_window = 0;
        r->id = tasks[i].id;
        
        if (tasks[i].ring == 3) {
            char* name = "App Ring 3 (.mct)";
            int j = 0;
            while(name[j] && j < 31) { r->name[j] = name[j]; j++; }
            r->name[j] = '\0';
        } else if (tasks[i].id == 0) {
            char* name = "Kernel Idle";
            int j = 0;
            while(name[j] && j < 31) { r->name[j] = name[j]; j++; }
            r->name[j] = '\0';
        } else {
            char* name = "Kernel Task";
            int j = 0;
            while(name[j] && j < 31) { r->name[j] = name[j]; j++; }
            r->name[j] = '\0';
        }
        
        r->ring = tasks[i].ring;
        r->state = tasks[i].state;
        r->priority = tasks[i].priority;
    }
    
    if (tm_selected_row >= num_rows) {
        tm_selected_row = -1;
    }
}

static void itoa_pad(int val, char* buf, int pad) {
    char temp[12];
    int len = 0;
    if (val == 0) temp[len++] = '0';
    while (val > 0) { temp[len++] = '0' + (val % 10); val /= 10; }
    
    int pos = 0;
    while (pos < pad - len) buf[pos++] = ' ';
    for (int i = len - 1; i >= 0; i--) buf[pos++] = temp[i];
    buf[pos] = '\0';
}

static void tm_draw(int wid) {
    int cw = 380;
    int ch = 300;
    sys_draw_rect(wid, 0, 0, cw, ch, 0x001E1E2E);

    sys_draw_rect(wid, 0, LIST_Y, cw, ch - LIST_Y - BTN_H, 0x0011111B);
    sys_draw_rect(wid, 0, ch - BTN_H, cw, BTN_H, 0x00313244);

    sys_draw_text(wid, 10, 5, "TYPE   ID   NAME                  RING  STATE", 0x00A6E3A1);
    
    int y = LIST_Y + 5;
    for (int i = 0; i < num_rows; i++) {
        if (y + ROW_HEIGHT > ch - BTN_H) break; 
        
        uint32_t text_col = (rows[i].is_window) ? 0x0089B4FA : 0x00CDD6F4;
        if (rows[i].ring == 0) text_col = 0x00F38BA8;
        
        if (i == tm_selected_row) {
            sys_draw_rect(wid, 5, y - 2, cw - 10, ROW_HEIGHT, 0x0045475A);
            text_col = 0x00FFFFFF;
        }
        
        char buf[64];
        int pos = 0;
        
        char* type = rows[i].is_window ? "WIN " : "TASK";
        for(int j=0; j<4; j++) buf[pos++] = type[j];
        buf[pos++] = ' '; buf[pos++] = ' ';
        
        char id_str[4];
        itoa_pad(rows[i].id, id_str, 3);
        for(int j=0; j<3; j++) buf[pos++] = id_str[j];
        buf[pos++] = ' '; buf[pos++] = ' ';
        
        int name_len = 0;
        while(rows[i].name[name_len] && name_len < 20) {
            buf[pos++] = rows[i].name[name_len];
            name_len++;
        }
        while(name_len < 22) { buf[pos++] = ' '; name_len++; }
        
        buf[pos++] = 'R';
        buf[pos++] = rows[i].ring == 3 ? '3' : '0';
        buf[pos++] = ' '; buf[pos++] = ' '; buf[pos++] = ' '; buf[pos++] = ' ';
        
        char* state_str = "UNK";
        if (rows[i].is_window) {
            state_str = (rows[i].state == 1) ? "ACT" : "HID";
        } else {
            if (rows[i].state == 1) state_str = "RUN";
            else if (rows[i].state == 2) state_str = "RDY";
            else if (rows[i].state == 3) state_str = "SLP";
        }
        for(int j=0; j<3; j++) buf[pos++] = state_str[j];
        buf[pos] = '\0';
        
        sys_draw_text(wid, 10, y, buf, text_col);
        y += ROW_HEIGHT;
    }
    
    if (tm_selected_row >= 0) {
        sys_draw_text(wid, 10, ch - BTN_H + 8, "[K] Kill Selected Task", 0x00F38BA8);
    }
    
    sys_update_window(wid);
}

typedef struct {
    int type; // 1 = paint, 2 = key, 3 = mouse
    int x, y;
    int key;
} gui_event_t;

void _start() {
    int wid = sys_create_window(100, 100, 380, 300, "Manajer Tugas");
    if (wid < 0) sys_exit();
    
    refresh_list();
    tm_draw(wid);
    
    gui_event_t ev;
    int tick = 0;
    int refresh_counter = 0;
    
    while (1) {
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 1) {
                tm_draw(wid);
            } else if (ev.type == 2) {
                if (ev.key == 27) sys_exit(); // ESC
                else if (ev.key == 'k' || ev.key == 'K') {
                    if (tm_selected_row >= 0 && !rows[tm_selected_row].is_window) {
                        sys_kill_task(rows[tm_selected_row].id);
                        refresh_list();
                        tm_draw(wid);
                    }
                }
            } else if (ev.type == 3) { // Mouse click
                int my = ev.y;
                int btn = ev.key;
                if (btn & 1) {
                    if (my >= LIST_Y && my < 300 - BTN_H) {
                        int row = (my - LIST_Y - 3) / ROW_HEIGHT;
                        if (row >= 0 && row < num_rows) {
                            tm_selected_row = row;
                        } else {
                            tm_selected_row = -1;
                        }
                        tm_draw(wid);
                    }
                }
            }
        }
        
        tick++;
        if (tick % 5000 == 0) {
            if (++refresh_counter > 5) {
                refresh_list();
                tm_draw(wid);
                refresh_counter = 0;
            }
        }
        
        sys_yield();
    }
}
