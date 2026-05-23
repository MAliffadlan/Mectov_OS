#ifndef TASKBAR_H
#define TASKBAR_H

#define TASKBAR_H_PX 32     // taskbar pixel height (modern)

void taskbar_draw();
void taskbar_handle_click(int mx, int my);
void taskbar_tick();
void taskbar_track_mouse(int mx, int my, int px, int py);
int taskbar_volume_popup_open(void);

extern int start_menu_open;
extern int calendar_open;

#endif


