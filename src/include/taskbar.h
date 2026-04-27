#ifndef TASKBAR_H
#define TASKBAR_H

#define TASKBAR_H_PX 28     // taskbar pixel height

void taskbar_draw();
void taskbar_handle_click(int mx, int my);
void taskbar_tick();

extern int start_menu_open;
extern int calendar_open;

#endif

