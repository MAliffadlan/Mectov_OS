#ifndef APPS_H
#define APPS_H

#include "vfs.h"

// Snake
void start_ular();

// Nano (Editor)
extern int ed_a;
extern char ed_b[MAX_FILE_SIZE], ed_fn[MAX_FILENAME];
extern int ed_c;
void st_ed(const char* f);
void sa_ex_ed();

#endif
