// src/sys/shell/shell_internal.h — internal interface for the shell modules.
//
// The former monolithic src/sys/shell.c is split into:
//   shell_core.c   dispatch (run_cmd_internal), ex_cmd, history/tab/prompt,
//                  env/alias tables, shared helper functions
//   builtins/      one cmd_<name>() per shell command, grouped by theme
//   job/job.c      background-job tracking + bg_child_entry
//   script/script.c  the for/while script interpreter (run_script)
//
// This header is the single shared interface: every module includes it and
// gets the kernel headers, the shell state, and the cross-module symbols it
// needs. Definitions live in shell_core.c (state, helpers), job/job.c (job
// state), and script/script.c (interpreter internals stay file-local).
#ifndef SHELL_INTERNAL_H
#define SHELL_INTERNAL_H

#include "../../include/shell.h"
#include "../../include/vga.h"
#include "../../include/keyboard.h"
#include "../../include/utils.h"
#include "../../include/vfs.h"
#include "../../include/security.h"
#include "../../include/speaker.h"
#include "../../include/mem.h"
#include "../../include/apps.h"
#include "../../include/pci.h"
#include "../../include/net.h"
#include "../../include/rtl8139.h"
#include "../../include/timer.h"
#include "../../include/loader.h"
#include "../../include/spinlock.h"
#include "../../include/task.h"
#include "../../include/rtc.h"
#include "../../include/task.h"
#include "../../include/ext2.h"
#include "../../include/passwd.h"
#include "../../include/serial.h"

// ---- environment variables (shell_core.c) ----
#define MAX_ENV_VARS 32
#define ENV_NAME_LEN 32
#define ENV_VAL_LEN  128
typedef struct {
    char name[ENV_NAME_LEN];
    char value[ENV_VAL_LEN];
} env_var_t;
extern env_var_t env_vars[MAX_ENV_VARS];
extern int env_var_count;

// ---- aliases (shell_core.c) ----
#define MAX_ALIASES 32
#define ALIAS_NAME_LEN 32
#define ALIAS_VAL_LEN  128
typedef struct {
    char name[ALIAS_NAME_LEN];
    char value[ALIAS_VAL_LEN];
} alias_t;
extern alias_t aliases[MAX_ALIASES];
extern int alias_count;

// File redirection stdin buffer: filled by '<' redirection, consumed by
// cat/grep when they would otherwise read the pipe buffer.
extern char shell_stdin_buf[4096];
extern int shell_stdin_len;
extern char hist_b[256];

// Previous directory for `cd -` (OLDPWD). Only updated on a successful cd to
// an explicit directory, so `cd -` always lands somewhere valid.
extern char shell_oldpwd[MAX_PATH];

// Next history slot to overwrite (oldest) — read by the `history` command.
extern int hist_next_slot;

// One text line (offset+length into a buffer) — used by split_lines and the
// sort/uniq commands.
typedef struct { int off; int len; } sh_line_t;

// ---- shell lock (kernel locking audit v38.4) ----
// Serializes shell command execution (SYS_EXEC_CMD) across terminals.
extern spinlock_t shell_lock;
void shell_lock_acquire(void);
void shell_lock_release(void);
// Drop/reacquire across blocking ops (sleep/waitpid) so a killed shell task
// never strands the lock.
void shell_lock_release_for_block(void);
void shell_lock_reacquire(void);

// ---- job control (job/job.c) ----
#define MAX_JOBS 16
typedef struct {
    int tid;
    int done;
    int stopped;      // suspended by SIGTSTP (Ctrl+Z), waiting for bg/fg
    char cmd[48];
} shell_job_t;
extern shell_job_t jobs[MAX_JOBS];
extern int job_count;
extern int shell_bg_flag;   // set while run_cmd_internal() runs a `&` command
int register_job(int tid, const char* cmd);
void print_jobs(void);
int find_job_tid(int num);
void bg_child_entry(void);

// ---- shared helpers (shell_core.c) ----
// Poll cap for net_wait_for(): a timeout of the wrong length beats wedging
// the box with a task that cannot even be killed (see net_wait_for in
// shell_core.c for the full rationale).
#define NET_WAIT_MAX_SPINS 4000000u

void print_num_field(int n, int width);
void sanitize_path(char* path);
int strstr_custom(const char* haystack, const char* needle);
int net_wait_for(volatile int* flag, uint32_t timeout_ms);
int split_lines(const char* src, int src_len, sh_line_t* lines, int max_lines);
int line_cmp(const char* src, const sh_line_t* a, const sh_line_t* b);
int wild_match(const char* pat, const char* str);
void print_hex_value(uint32_t v);
char* next_token(char** pp);
void init_env_vars_and_aliases();
void expand_env_vars(char* out, const char* in, int max_len);
void expand_alias(char* out, const char* in, int max_len);

// ---- dispatch (shell_core.c) ----
void run_cmd_internal(void);

// ---- builtins: one function per command (builtins/<group>/cmd_*.c) ----
void cmd_help(void);
void cmd_clear(void);
void cmd_mfetch(void);
void cmd_mem(void);
void cmd_kmemstats(void);
void cmd_memstat(void);
void cmd_uptime(void);
void cmd_vfsinfo(void);
void cmd_cd(void);
void cmd_pwd(void);
void cmd_ls(void);
void cmd_ls_arg(void);
void cmd_tree(void);
void cmd_tree_arg(void);
void cmd_mkdir_arg(void);
void cmd_touch_arg(void);
void cmd_cat(void);
void cmd_grep_arg(void);
void cmd_uname(void);
void cmd_whoami(void);
void cmd_passwd(void);
void cmd_hostname(void);
void cmd_env(void);
void cmd_seq(void);
void cmd_head(void);
void cmd_wc(void);
void cmd_type_arg(void);
void cmd_rm_arg(void);
void cmd_rmdir_arg(void);
void cmd_cp_arg(void);
void cmd_mv_arg(void);
void cmd_df(void);
void cmd_shutdown(void);
void cmd_reboot(void);
void cmd_lspci(void);
void cmd_snake(void);
void cmd_flappy(void);
void cmd_doom(void);
void cmd_taskmgr(void);
void cmd_lock(void);
void cmd_date(void);
void cmd_color(void);
int  cmd_run(void);          // returns 1 on the foreground path (skip b_idx reset)
void cmd_buat_arg(void);
void cmd_baca_arg(void);
void cmd_edit(void);
void cmd_hapus_arg(void);
void cmd_ping_arg(void);
void cmd_host_arg(void);
void cmd_fetch_arg(void);
void cmd_sh(void);
void cmd_export(void);
void cmd_alias(void);
void cmd_unalias_arg(void);
void cmd_history(void);
void cmd_ps(void);
void cmd_jobs(void);
void cmd_fg(void);
void cmd_bg(void);
void cmd_kill_arg(void);
void cmd_kill(void);
void cmd_echo_arg(void);
void cmd_sleep(void);
void cmd_yes(void);
void cmd_tone(void);
void cmd_beep(void);
void cmd_man_arg(void);
void cmd_printf_arg(void);
void cmd_sort(void);
void cmd_uniq(void);
void cmd_tee_arg(void);
void cmd_find(void);

#endif // SHELL_INTERNAL_H
