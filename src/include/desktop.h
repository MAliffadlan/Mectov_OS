#ifndef DESKTOP_H
#define DESKTOP_H

#include "taskbar.h"

#define ICON_COUNT  11

void desktop_draw();
void desktop_handle_mouse(int mx, int my, int btn, int pbtn);

#endif

