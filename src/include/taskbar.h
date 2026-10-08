#ifndef TASKBAR_H
#define TASKBAR_H

#define TASKBAR_H_PX 28     // taskbar pixel height (classic)

// Start menu geometry, shared by taskbar.c draw/hover/click and kernel.c's
// popup-dismiss bounds: 40 px header + 10 app rows + 12 px section divider
// + 3 system rows + 16 px footer hint. When a search query is active the
// menu flattens to just the matching rows (no divider); the panel keeps
// its full height either way.
#define SM_HEAD_H 40
#define SM_ROW_H 28
#define SM_W 200
#define SM_SECT_H 12
#define SM_FOOT_H 16
#define SM_APPS 10   // sm_labels[0..9]; 10..12 are the System group
#define START_MENU_H (SM_HEAD_H + 10*SM_ROW_H + SM_SECT_H + 3*SM_ROW_H + SM_FOOT_H)
#define START_MENU_ITEMS 13

void taskbar_draw();
void taskbar_handle_click(int mx, int my);
void taskbar_handle_key(int sc, char c);
void taskbar_tick();
void taskbar_track_mouse(int mx, int my, int px, int py);
int taskbar_volume_popup_open(void);
void draw_app_icon(int ix, int iy, const char* title, int size);

extern int start_menu_open;
extern int calendar_open;

#endif


