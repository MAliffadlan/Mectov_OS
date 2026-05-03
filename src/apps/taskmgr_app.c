#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/wm.h"
#include "../include/task.h"
#include "../include/timer.h"
#include "../include/serial.h"

static int tm_win_id = -1;
static int tm_open   = 0;
static int tm_selected_row = -1;

#define MAX_TASKS 64
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

static void refresh_list() {
    num_rows = 0;
    
    // 1. Add GUI Windows
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wm_wins[i].id >= 0 && wm_wins[i].visible) {
            TmRow* r = &rows[num_rows++];
            r->is_window = 1;
            r->id = wm_wins[i].id;
            
            // Salin judul window
            int j = 0;
            while (wm_wins[i].title[j] && j < 31) {
                r->name[j] = wm_wins[i].title[j];
                j++;
            }
            r->name[j] = '\0';
            
            r->ring = wm_wins[i].owner_ring;
            r->state = wm_wins[i].minimized ? 2 : 1; // 1=Active, 2=Hidden
            r->priority = 1; // Interactive
        }
    }
    
    // 2. Add OS Tasks (Scheduler)
    for (int i = 0; i < MAX_TASKS; i++) {
        task_info_t info;
        if (get_task_info(i, &info)) {
            TmRow* r = &rows[num_rows++];
            r->is_window = 0;
            r->id = info.id;
            
            if (info.ring == 3) {
                strcpy(r->name, "App Ring 3 (.mct)");
            } else if (i == 0) {
                strcpy(r->name, "Kernel Idle");
            } else {
                strcpy(r->name, "Kernel Task");
            }
            
            r->ring = info.ring;
            r->state = info.state; // 1=Running, 2=Ready, 3=Sleep
            r->priority = info.priority;
        }
    }
    
    // Pastikan seleksi tidak out of bounds
    if (tm_selected_row >= num_rows) {
        tm_selected_row = -1;
    }
}

// Minimalist conversion to string
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

static void tm_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    // App background
    draw_rect(cx, cy, cw, ch, GUI_BG);

    // List area background
    int list_h = ch - BTN_H - 10 - HEADER_H;
    draw_rect(cx + 10, cy + LIST_Y, cw - 20, list_h, 0x00151515);
    draw_rect_border(cx + 10, cy + LIST_Y, cw - 20, list_h, GUI_BORDER2);

    // Header
    draw_string_px(cx + 20, cy + 8, "ID", GUI_DIM, 0);
    draw_string_px(cx + 60, cy + 8, "Aplikasi", GUI_DIM, 0);
    draw_string_px(cx + 200, cy + 8, "Ring", GUI_DIM, 0);
    draw_string_px(cx + 260, cy + 8, "Status", GUI_DIM, 0);

    // Draw rows
    int y = cy + LIST_Y + 2;
    int drawn = 0;

    for (int i = 0; i < num_rows; i++) {
        TmRow* r = &rows[i];
        
        // Selected background
        if (i == tm_selected_row) {
            draw_rect(cx + 11, y, cw - 22, ROW_HEIGHT, 0x002A2A2A);
        }

        char id_str[8];
        itoa_pad(r->id, id_str, 3);
        draw_string_px(cx + 15, y + 4, id_str, GUI_TEXT, 0);

        draw_string_px(cx + 60, y + 4, r->name, r->is_window ? 0x00AABBFF : GUI_TEXT, 0);

        const char* ring_str = (r->ring == 0) ? "Ring 0" : "Ring 3";
        draw_string_px(cx + 200, y + 4, ring_str, (r->ring == 0) ? GUI_TEAL : 0x00FF8888, 0);

        const char* state_str = "Unknown";
        if (r->is_window) {
            state_str = (r->state == 1) ? "Active" : "Hidden";
        } else {
            if (r->state == 1) state_str = "Running";
            else if (r->state == 2) state_str = "Ready";
            else if (r->state == 3) state_str = "Sleep";
        }
        
        draw_string_px(cx + 260, y + 4, state_str, (r->state == 1) ? 0x0055FF55 : GUI_DIM, 0);

        y += ROW_HEIGHT;
        drawn++;
        if (y > cy + LIST_Y + list_h - ROW_HEIGHT) break; // clip
    }

    // Kill Button
    int by = cy + ch - BTN_H - 5;
    int bx = cx + cw - 160;
    
    // Draw button (red if something is selected, dim if not)
    uint32_t btn_bg = (tm_selected_row >= 0) ? 0x00441111 : 0x00222222;
    uint32_t btn_fg = (tm_selected_row >= 0) ? 0x00FF5555 : GUI_DIM;
    
    draw_rounded_rect(bx, by, 150, BTN_H, 4, btn_bg);
    draw_rounded_rect_border(bx, by, 150, BTN_H, 4, GUI_BORDER2);
    draw_string_px(bx + 12, by + 8, "Hentikan Proses", btn_fg, 0);
}

static void tm_key(int id, char c, uint8_t sc) { (void)id;(void)c;(void)sc; }

static void tm_mouse(int id, int cx, int cy, int btn) {
    (void)id;
    if (btn != 1) return; // Only process left clicks

    int w = wm_wins[id].w;
    int h = wm_wins[id].h - 24; // relative to content area

    // Check if clicked inside list
    int list_h = h - BTN_H - 10 - HEADER_H;
    if (cx >= 10 && cx <= w - 10 && cy >= LIST_Y && cy <= LIST_Y + list_h) {
        int rel_y = cy - LIST_Y - 2;
        int row = rel_y / ROW_HEIGHT;
        
        if (row >= 0 && row < num_rows) {
            tm_selected_row = row;
        } else {
            tm_selected_row = -1;
        }
        return;
    }

    // Check if clicked kill button
    int by = h - BTN_H - 5;
    int bx = w - 160;
    if (cx >= bx && cx <= bx + 150 && cy >= by && cy <= by + BTN_H) {
        if (tm_selected_row >= 0 && tm_selected_row < num_rows) {
            TmRow* r = &rows[tm_selected_row];
            
            if (r->is_window) {
                // Jangan bunuh Task Manager itu sendiri dari tombol ini untuk mencegah crash ganda
                if (r->id != id) {
                    wm_close(r->id);
                    tm_selected_row = -1; // Reset selection
                    refresh_list(); // refresh immediately
                }
            } else {
                // Never kill task 0 (kernel idle)
                if (r->id > 0) {
                    task_kill(r->id);
                    tm_selected_row = -1; // Reset selection
                    refresh_list(); // refresh immediately
                }
            }
        }
    }
}

static void tm_tick(int id) {
    if (!wm_is_open(id)) {
        tm_open = 0;
        tm_win_id = -1;
        tm_selected_row = -1;
        return;
    }
    
    // Auto refresh periodic
    static int refresh_counter = 0;
    if (++refresh_counter > 20) {
        refresh_list();
        refresh_counter = 0;
    }
}

void open_taskmgr_app() {
    if (tm_open && wm_is_open(tm_win_id)) { wm_raise(tm_win_id); return; }
    refresh_list(); // initial load
    tm_win_id = wm_open(100, 100, 380, 300, "Manajer Tugas", tm_draw, tm_key, tm_tick, tm_mouse);
    tm_open = (tm_win_id >= 0);
    tm_selected_row = -1;
}
