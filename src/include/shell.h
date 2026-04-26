#ifndef SHELL_H
#define SHELL_H

extern char cmd_b[256], hist_b[256];
extern int b_idx, is_script;
extern const char* cmd_list[];

void ex_cmd();
void run_script(const char* f);

#endif
