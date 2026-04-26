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

// File Explorer
void open_explorer_app();

// Power App
void open_power_app();

// PCI Device Manager
void open_pci_app();
void sa_ex_ed();

// GUI Apps
void open_terminal_app();
void open_clock_app();
void open_sysinfo_app();
void open_browser_app();
void term_clear();
int get_use_term_buf();

// Terminal shell bridge
void ex_cmd_term();
void term_print(const char* s, unsigned char color);
void p_char_gui(char c, unsigned char col);
int  get_use_term_buf();

#endif

