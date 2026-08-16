#ifndef TASKBAR_H
#define TASKBAR_H

#define TASKBAR_H_PX 28     // taskbar pixel height (classic)

// Start menu geometry (shared by taskbar.c draw/hit-test and kernel.c's
// popup-dismiss bounds): 40 px header + 12 items x 28 px.
#define START_MENU_H 376
#define START_MENU_ITEMS 12

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


