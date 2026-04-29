#ifndef SHELL_H
#define SHELL_H

#include "types.h"

#define CMD_BUF_SIZE 256
#define HIST_MAX     16
#define TAB_MAX      32

extern char cmd_b[CMD_BUF_SIZE];
extern int b_idx, is_script;
extern const char* cmd_list[];

// History
extern char history[HIST_MAX][CMD_BUF_SIZE];
extern int hist_count;
extern int hist_pos;   // -1 = no recall

// Tab completion
extern int tab_match_count;
extern char tab_matches[TAB_MAX][CMD_BUF_SIZE];

void ex_cmd();
void run_script(const char* f);

// New helpers
int shell_try_complete();
int shell_history_up();
int shell_history_down();
void shell_reset_history_nav();
void shell_apply_tab();

#endif