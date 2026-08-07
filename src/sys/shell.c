// ============================================================
// shell.c — Mectov OS Shell with Tab Completion & History
// ============================================================

#include "../include/shell.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/utils.h"
#include "../include/vfs.h"
#include "../include/security.h"
#include "../include/speaker.h"
#include "../include/mem.h"
#include "../include/apps.h"
#include "../include/pci.h"
#include "../include/net.h"
#include "../include/rtl8139.h"
#include "../include/timer.h"
#include "../include/loader.h"
#include "../include/rtc.h"
#include "../include/task.h"
#include "../include/ext2.h"
#include "../include/serial.h"  // write_serial_string/hex (job diagnostics)

// Single source of truth for the kernel release string shown by the help
// banner, mfetch and uname, so the three can never drift out of sync.
#define OS_VERSION "36.8"

// --- Command buffer & state ---
char cmd_b[CMD_BUF_SIZE]; int b_idx = 0;

// File redirection stdin buffer: filled by '<' redirection, consumed by
// cat/grep when they would otherwise read the pipe buffer.
char shell_stdin_buf[4096];
int shell_stdin_len = 0;
char hist_b[256];
int is_script = 0;

#define MAX_ENV_VARS 32
#define ENV_NAME_LEN 32
#define ENV_VAL_LEN  128

typedef struct {
    char name[ENV_NAME_LEN];
    char value[ENV_VAL_LEN];
} env_var_t;

env_var_t env_vars[MAX_ENV_VARS];
int env_var_count = 0;

// Shared one-shot init guard for the env/alias tables (see ex_cmd and
// shell_print_prompt). Module-level so both init sites agree and never reset
// user-added variables.
static int env_alias_initialized = 0;

#define MAX_ALIASES 32
#define ALIAS_NAME_LEN 32
#define ALIAS_VAL_LEN  128

typedef struct {
    char name[ALIAS_NAME_LEN];
    char value[ALIAS_VAL_LEN];
} alias_t;

alias_t aliases[MAX_ALIASES];
int alias_count = 0;

void init_env_vars_and_aliases() {
    // Default environment variables
    strcpy(env_vars[0].name, "USER");
    strcpy(env_vars[0].value, "root");
    
    strcpy(env_vars[1].name, "OS");
    strcpy(env_vars[1].value, "Mectov_OS");
    
    strcpy(env_vars[2].name, "HOME");
    strcpy(env_vars[2].value, "/home");
    
    strcpy(env_vars[3].name, "SHELL");
    strcpy(env_vars[3].value, "/sys/shell");
    
    env_var_count = 4;
    
    // Default aliases
    strcpy(aliases[0].name, "ll");
    strcpy(aliases[0].value, "ls");
    
    strcpy(aliases[1].name, "la");
    strcpy(aliases[1].value, "ls");
    
    strcpy(aliases[2].name, "cls");
    strcpy(aliases[2].value, "clear");
    
    strcpy(aliases[3].name, "neofetch");
    strcpy(aliases[3].value, "mfetch");
    
    // Legacy Indonesian command names → English equivalents (still work)
    strcpy(aliases[4].name, "buat");      strcpy(aliases[4].value, "touch");
    strcpy(aliases[5].name, "tulis");     strcpy(aliases[5].value, "nano");
    strcpy(aliases[6].name, "baca");      strcpy(aliases[6].value, "cat");
    strcpy(aliases[7].name, "hapus");     strcpy(aliases[7].value, "rm");
    strcpy(aliases[8].name, "ular");      strcpy(aliases[8].value, "snake");
    strcpy(aliases[9].name, "nada");      strcpy(aliases[9].value, "tone");
    strcpy(aliases[10].name, "tunggu");   strcpy(aliases[10].value, "sleep");
    strcpy(aliases[11].name, "waktu");    strcpy(aliases[11].value, "date");
    strcpy(aliases[12].name, "warna");    strcpy(aliases[12].value, "color");
    strcpy(aliases[13].name, "kunci");    strcpy(aliases[13].value, "lock");
    strcpy(aliases[14].name, "jalankan"); strcpy(aliases[14].value, "run");
    strcpy(aliases[15].name, "matikan");  strcpy(aliases[15].value, "shutdown");
    strcpy(aliases[16].name, "mulaiulang"); strcpy(aliases[16].value, "reboot");
    
    alias_count = 17;
}

void expand_env_vars(char* out, const char* in, int max_len) {
    int i = 0, o = 0;
    while (in[i] && o < max_len - 1) {
        if (in[i] == '$') {
            i++; // skip '$'
            char var_name[ENV_NAME_LEN];
            int vn = 0;
            while (in[i] && vn < ENV_NAME_LEN - 1 && 
                   ((in[i] >= 'a' && in[i] <= 'z') || 
                    (in[i] >= 'A' && in[i] <= 'Z') || 
                    (in[i] >= '0' && in[i] <= '9') || 
                    in[i] == '_')) {
                var_name[vn++] = in[i++];
            }
            var_name[vn] = '\0';
            
            if (vn == 0) {
                out[o++] = '$';
            } else {
                // Look up in env_vars
                const char* val = "";
                for (int k = 0; k < env_var_count; k++) {
                    if (strcmp(env_vars[k].name, var_name) == 0) {
                        val = env_vars[k].value;
                        break;
                    }
                }
                
                // Copy value to output
                while (*val && o < max_len - 1) {
                    out[o++] = *val++;
                }
            }
        } else {
            out[o++] = in[i++];
        }
    }
    out[o] = '\0';
}

void expand_alias(char* out, const char* in, int max_len) {
    // Extract the first word (command)
    char first_word[ALIAS_NAME_LEN];
    int i = 0, fw = 0;
    while (in[i] && in[i] == ' ') i++; // skip leading spaces
    
    while (in[i] && in[i] != ' ' && fw < ALIAS_NAME_LEN - 1) {
        first_word[fw++] = in[i++];
    }
    first_word[fw] = '\0';
    
    // Look up in aliases
    const char* val = NULL;
    for (int k = 0; k < alias_count; k++) {
        if (strcmp(aliases[k].name, first_word) == 0) {
            val = aliases[k].value;
            break;
        }
    }
    
    if (val) {
        // Copy the alias value first
        int o = 0;
        while (*val && o < max_len - 1) {
            out[o++] = *val++;
        }
        // Copy the rest of the command (arguments)
        while (in[i] && o < max_len - 1) {
            out[o++] = in[i++];
        }
        out[o] = '\0';
    } else {
        // No alias found, just copy input as is
        strncpy(out, in, max_len - 1);
        out[max_len - 1] = '\0';
    }
}

// --- Job control (background processes) ---
// `cmd &` forks a copy of the shell's task to run the command (or, for `run`,
// just launches without grabbing the terminal). Jobs are tracked here so the
// shell can list them (jobs), wait on them (fg) or kill them (kill %n).
#define MAX_JOBS 16
typedef struct {
    int tid;
    int done;
    int stopped;      // suspended by SIGTSTP (Ctrl+Z), waiting for bg/fg
    char cmd[48];
} shell_job_t;
static shell_job_t jobs[MAX_JOBS];
static int job_count = 0;
static int shell_bg_flag = 0;   // set while run_cmd_internal() runs a `&` command

static int register_job(int tid, const char* cmd) {
    // Reuse a finished job's slot when full
    int slot = -1;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].tid > 0 && !task_is_alive(jobs[i].tid)) { slot = i; break; }
    }
    if (slot < 0 && job_count < MAX_JOBS) slot = job_count++;
    if (slot < 0) return -1;
    jobs[slot].tid = tid;
    jobs[slot].done = 0;
    jobs[slot].stopped = 0;
    int n = 0;
    for (; cmd[n] && n < 47; n++) jobs[slot].cmd[n] = cmd[n];
    jobs[slot].cmd[n] = '\0';
    // Return the 1-based job number
    int num = 0;
    for (int i = 0; i <= slot; i++) if (jobs[i].tid > 0) num++;
    write_serial_string("[JOBS] registered job ");
    write_serial_hex(num);
    write_serial_string(" tid=");
    write_serial_hex(tid);
    write_serial_string(" cmd='");
    write_serial_string(cmd);
    write_serial_string("'\n");
    return num;
}

static void refresh_jobs(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].tid <= 0) continue;
        extern int task_get_state(int tid);
        if (!task_is_alive(jobs[i].tid)) {
            jobs[i].done = 1;
            jobs[i].stopped = 0;
        } else if (task_get_state(jobs[i].tid) == TASK_STATE_STOPPED) {
            jobs[i].stopped = 1;
            jobs[i].done = 0;
        } else {
            jobs[i].stopped = 0;
        }
    }
}

// Public: register the foreground app that Ctrl+Z just suspended, so `jobs`
// lists it and `bg`/`fg` can resume it. Called from kernel.c.
int shell_register_stopped_job(int tid) {
    if (tid <= 0) return -1;
    int n = register_job(tid, "foreground");
    if (n > 0) {
        for (int i = 0; i < MAX_JOBS; i++) {
            if (jobs[i].tid == tid) { jobs[i].stopped = 1; jobs[i].done = 0; break; }
        }
        print("[", 0x0E); p_int(n, 0x0E); print("] Stopped", 0x0C);
        print(" pid=", 0x07); p_int(tid, 0x07); print("  (Ctrl+Z)\n", 0x07);
        write_serial_string("[JOBS] stopped job ");
        write_serial_hex(n);
        write_serial_string(" tid=");
        write_serial_hex(tid);
        write_serial_string("\n");
    }
    return n;
}

static void print_jobs(void) {
    refresh_jobs();
    int num = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].tid <= 0) continue;
        num++;
        print("[", 0x0E); p_int(num, 0x0E); print("] ", 0x0E);
        if (jobs[i].stopped)      print("Stopped ", 0x0C);
        else if (jobs[i].done)    print("Done    ", 0x0A);
        else                      print("Running ", 0x0B);
        print("pid=", 0x07); p_int(jobs[i].tid, 0x07); print("  ", 0x07);
        print(jobs[i].cmd, 0x0F);
        print("\n", 0x0F);
    }
}

// Find the tid of the n-th job (1-based). -1 if none.
static int find_job_tid(int num) {
    refresh_jobs();
    int count = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].tid <= 0) continue;
        count++;
        if (count == num) return jobs[i].tid;
    }
    return -1;
}

const char* cmd_list[] = {
    "help","clear","mfetch","mem","memstat","kmemstats","uptime","vfsinfo",
    "ls","cd","pwd","mkdir","touch","cat","head","tree","rm","rmdir","cp","mv","df",
    "edit","nano",
    "sh","source","export","alias","unalias","history","ps","kill",
    "jobs","fg","bg",
    "echo","beep","tone","sleep","date","color","lock",
    "uname","whoami","hostname","env","seq",
    "run","snake","taskmgr","flappy","doom","lspci","man",
    "ping","host","fetch","grep",
    "shutdown","reboot", NULL
};

// --- History circular buffer ---
char history[HIST_MAX][CMD_BUF_SIZE];
int hist_count = 0;
int hist_pos = -1;

static int hist_next_slot = 0; // next slot to overwrite (oldest)

// --- Prompt with timestamp (ToaruOS style) ---
void shell_print_timestamp() {
    rtc_time_t tm = rtc_read_time();
    char ts[20];
    int i = 0;
    ts[i++] = '[';
    ts[i++] = '0' + tm.month / 10; ts[i++] = '0' + tm.month % 10;
    ts[i++] = '/';
    ts[i++] = '0' + tm.day / 10; ts[i++] = '0' + tm.day % 10;
    ts[i++] = ' ';
    ts[i++] = '0' + tm.hour / 10; ts[i++] = '0' + tm.hour % 10;
    ts[i++] = ':';
    ts[i++] = '0' + tm.minute / 10; ts[i++] = '0' + tm.minute % 10;
    ts[i++] = ':';
    ts[i++] = '0' + tm.second / 10; ts[i++] = '0' + tm.second % 10;
    ts[i++] = ']';
    ts[i] = '\0';
    print(ts, 0x0C);
}

void shell_print_prompt() {
    if (!env_alias_initialized) {
        init_env_vars_and_aliases();
        env_alias_initialized = 1;
    }
    char cwd[MAX_PATH];
    vfs_get_abs_path(get_current_dir(), cwd, MAX_PATH);
    print("root@mectov", 0x0A);
    print(":", 0x07);
    print(cwd, 0x0B);
    print("$ ", 0x0F);
}

static void history_add(const char* cmd) {
    if (!cmd || cmd[0] == '\0') return;
    // Don't add duplicate of last entry
    if (hist_count > 0) {
        int last = (hist_next_slot == 0) ? HIST_MAX - 1 : hist_next_slot - 1;
        if (strcmp(history[last], cmd) == 0) return;
    }
    strcpy(history[hist_next_slot], cmd);
    hist_next_slot = (hist_next_slot + 1) % HIST_MAX;
    if (hist_count < HIST_MAX) hist_count++;
    hist_pos = -1;
}

// --- Tab completion ---
int tab_match_count = 0;
char tab_matches[TAB_MAX][CMD_BUF_SIZE];

static int tab_match_prefix(const char* prefix, const char* str) {
    while (*prefix && *str && *prefix == *str) { prefix++; str++; }
    return (*prefix == '\0'); // prefix fully matched
}

int shell_try_complete() {
    tab_match_count = 0;
    if (b_idx == 0) return 0;
    
    int spc = 0;
    cmd_b[b_idx] = '\0';
    for (int i = 0; cmd_b[i]; i++) if (cmd_b[i] == ' ') spc++;
    
    // If there's a space, we're completing a filename arg
    if (spc > 0) {
        // Find start of last word
        int last = b_idx - 1;
        while (last >= 0 && cmd_b[last] != ' ') last--;
        int word_start = last + 1;
        
        // Extract prefix into temp buffer (don't corrupt cmd_b)
        char prefix[CMD_BUF_SIZE];
        strcpy(prefix, &cmd_b[word_start]);
        
        // Scan VFS for matching files/dirs
        for (int i = 0; i < MAX_NODES && tab_match_count < TAB_MAX; i++) {
            if (!fs_nodes[i].in_use) continue;
            // Only match if parent is current_dir, or if path is absolute
            int p = fs_nodes[i].parent;
            int cur_cd = get_current_dir();
            if (p == cur_cd || cur_cd == 0) {
                if (tab_match_prefix(prefix, fs_nodes[i].name)) {
                    strcpy(tab_matches[tab_match_count], fs_nodes[i].name);
                    tab_match_count++;
                }
            }
        }
    } else {
        // Completing built-in commands
        for (int i = 0; cmd_list[i] != NULL && tab_match_count < TAB_MAX; i++) {
            if (tab_match_prefix(cmd_b, cmd_list[i])) {
                strcpy(tab_matches[tab_match_count], cmd_list[i]);
                tab_match_count++;
            }
        }
    }
    return tab_match_count;
}

// --- History navigation ---
int shell_history_up() {
    if (hist_count == 0) return 0;
    if (hist_pos == -1) hist_pos = hist_next_slot; // start from newest
    // Go back one
    int new_pos = (hist_pos == 0) ? HIST_MAX - 1 : hist_pos - 1;
    // If we wrapped around past oldest, stop
    if (new_pos == hist_next_slot) return 0;
    
    // Check if there's actually a command there
    if (history[new_pos][0] == '\0') return 0;
    
    hist_pos = new_pos;
    strcpy(cmd_b, history[hist_pos]);
    b_idx = strlen(cmd_b);
    return 1;
}

int shell_history_down() {
    if (hist_count == 0 || hist_pos == -1) return 0;
    
    int new_pos = (hist_pos + 1) % HIST_MAX;
    
    // If we've gone back to baseline
    if (new_pos == hist_next_slot) {
        hist_pos = -1;
        cmd_b[0] = '\0';
        b_idx = 0;
        return 1;
    }
    
    hist_pos = new_pos;
    strcpy(cmd_b, history[hist_pos]);
    b_idx = strlen(cmd_b);
    return 1;
}

void shell_reset_history_nav() { hist_pos = -1; }

// --- Print a range of cmd_b ---
static void shell_redisplay() {
    cmd_b[b_idx] = '\0';
    if (get_use_term_buf()) {
        extern void term_print(const char*, unsigned char);
        extern void term_putchar(char, unsigned char);
        // Access term state directly to clear current line
        extern int term_get_cx(void);
        extern int term_get_cy(void);
        extern void term_clear_line(void);
        
        term_clear_line(); // Clear current line buffer
        term_print("root@mectov", 0x0A);
        {
            char cwd[MAX_PATH];
            vfs_get_abs_path(get_current_dir(), cwd, MAX_PATH);
            term_print(":", 0x07);
            term_print(cwd, 0x0B);
        }
        term_print("$ ", 0x0F);
        term_print(cmd_b, 0x0F);
    } else {
        print("\r", 0x00);
        shell_print_prompt();
        print(cmd_b, 0x0F);
        // Clear rest of line
        int row_len = b_idx + 16;
        for (int i = row_len; i < 80; i++) print(" ", 0x00);
        print("\r", 0x00);
        shell_print_prompt();
        print(cmd_b, 0x0F);
    }
}

// --- Tab completion handler (call from keyboard handler) ---
// Apply a tab completion: replace cmd_b with common prefix
void shell_apply_tab() {
    int n = shell_try_complete();
    if (n == 0) return;
    
    if (n == 1) {
        // Single match — complete immediately
        // Find start of last word
        int last = b_idx - 1;
        while (last >= 0 && cmd_b[last] != ' ') last--;
        int word_start = last + 1;
        int is_first_word = (word_start == 0); // completing a command name
        
        // Replace from word_start with the match
        int j = word_start;
        int k = 0;
        // cmd_b is CMD_BUF_SIZE bytes; word_start can be up to 255, so the
        // match must be truncated rather than allowed to overflow into the
        // adjacent globals (hist_b, env_vars, aliases).
        while (tab_matches[0][k] && j < CMD_BUF_SIZE - 2) {
            cmd_b[j++] = tab_matches[0][k++];
        }
        // If directory, add trailing /
        int node = vfs_get_node(tab_matches[0]);
        if (node >= 0 && vfs_is_dir(node)) {
            if (j < CMD_BUF_SIZE - 1) cmd_b[j++] = '/';
        } else if (is_first_word) {
            // Add trailing space after command name for convenience
            if (j < CMD_BUF_SIZE - 1) cmd_b[j++] = ' ';
        }
        cmd_b[j] = '\0';
        b_idx = j;
    } else {
        // Multiple matches — show them and redisplay
        if (get_use_term_buf()) {
            term_putchar('\n', 0x00);
            for (int i = 0; i < n; i++) {
                term_print(tab_matches[i], 0x0F);
                term_print("  ", 0x07);
                if ((i + 1) % 5 == 0) term_putchar('\n', 0x00);
            }
            term_putchar('\n', 0x00);
        } else {
            print("\n", 0x00);
            for (int i = 0; i < n; i++) {
                print(tab_matches[i], 0x0F);
                print("  ", 0x07);
                if ((i + 1) % 5 == 0) print("\n", 0x00);
            }
            print("\n", 0x00);
        }
    }
    shell_redisplay();
}

static int strstr_custom(const char* haystack, const char* needle) {
    if (!*needle) return 0;
    for (int i = 0; haystack[i]; i++) {
        int match = 1;
        for (int j = 0; needle[j]; j++) {
            if (haystack[i + j] != needle[j]) {
                match = 0;
                break;
            }
        }
        if (match) return i;
    }
    return -1;
}

static void sanitize_path(char* path) {
    if (!path) return;
    int start = 0;
    while (path[start] == ' ') start++;
    
    int len = 0;
    while (path[start + len] != '\0') len++;
    
    // Trim trailing spaces
    while (len > 0 && path[start + len - 1] == ' ') {
        len--;
    }
    
    // Strip surrounding quotes
    if (len >= 2 && 
        ((path[start] == '"' && path[start + len - 1] == '"') ||
         (path[start] == '\'' && path[start + len - 1] == '\''))) {
        start++;
        len -= 2;
    }
    
    // Move to beginning of path buffer
    for (int i = 0; i < len; i++) {
        path[i] = path[start + i];
    }
    path[len] = '\0';
}

// Poll the network until *flag goes non-zero. Returns 1 on success, 0 on give-up.
//
// Bounded two ways on purpose. The tick deadline is the real timeout while the
// clock is running, but ex_cmd()'s live entry point is SYS_EXEC_CMD and int 0x80
// is an interrupt gate (IF=0), so IRQ0 cannot fire and get_ticks() may never
// advance at all. The spin cap is what actually guarantees we return in that
// case: a timeout of the wrong length beats wedging the box with a task that
// cannot even be killed. Remove the cap only once the syscall path is
// preemptible (trap gate) and every handler has been audited for re-entrancy.
#define NET_WAIT_MAX_SPINS 4000000u

static int net_wait_for(volatile int* flag, uint32_t timeout_ms) {
    uint32_t start = get_ticks();
    uint32_t spins = 0;
    while (!*flag) {
        if ((get_ticks() - start) >= timeout_ms) break;
        if (++spins >= NET_WAIT_MAX_SPINS) break;
        net_poll();
    }
    return *flag != 0;
}

// ============================================================
// Main command execution
// ============================================================

static void run_cmd_internal() {
    // --- HELP ---
    if (strcmp(cmd_b, "help") == 0) {
        print("======================================================================\n", 0x0B);
        print("                ⚡ MECTOV OS v", 0x0F); print(OS_VERSION, 0x0F); print(" - COMMAND CENTER ⚡                \n", 0x0F);
        print("======================================================================\n", 0x0B);
        print(" SYSTEM  : ", 0x0B); print("mfetch, date, color, clear, mem, memstat, kmemstats, uptime, lock, ps, kill\n", 0x0F);
        print(" FILE VFS: ", 0x0B); print("ls, cd, pwd, mkdir, touch, cat, head, tree, rm, rmdir, cp, mv, df\n", 0x0F);
        print(" IDENTITY: ", 0x0B); print("uname [-a], whoami, hostname, env, seq [FIRST] LAST\n", 0x0F);
        print(" EDITOR  : ", 0x0B); print("nano, edit\n", 0x0F);
        print(" SHELL   : ", 0x0B); print("export [NAME=VAL], alias [NAME=VAL], unalias, history, sh\n", 0x0F);
        print(" JOBS    : ", 0x0B); print("cmd & (background), jobs, fg [n], bg [n], kill [%n]\n", 0x0F);
        print(" REDIR   : ", 0x0B); print("cmd > file (truncate), cmd >> file (append), cmd < file (stdin)\n", 0x0F);
        print(" APPS GUI: ", 0x0B); print("flappy, doom, taskmgr, snake, run [app.mct], run [app.mct] &\n", 0x0A);
        print(" NET & HW: ", 0x0B); print("ping [ip], host [domain], fetch [domain], lspci\n", 0x0F);
        print(" UTILS   : ", 0x0B); print("echo [msg], sleep [sec], tone [freq], beep, man [cmd]\n", 0x0F);
        print(" POWER   : ", 0x0B); print("reboot, shutdown\n", 0x0C);
        print("----------------------------------------------------------------------\n", 0x07);
        print(" SHORTCUT: ", 0x0E); print("Tab=Autocomplete  |  Up/Down=History  |  Pipes: cmd1 | cmd2\n", 0x0F);
        print(" LANG    : ", 0x0E); print("English UI; legacy Indonesian aliases (buat, hapus, ular) still work\n", 0x0F);
        print("======================================================================\n", 0x0B);
    }
    // --- CLEAR ---
    else if (strcmp(cmd_b, "clear") == 0) { 
        if (get_use_term_buf()) term_clear();
        else c_work(); 
    }
    // --- MFETCH (ToaruOS sysinfo style) ---
    else if (strcmp(cmd_b, "mfetch") == 0) {
        // Row 1: color blocks + username
        print("  ", 0x00);
        // 8 colored blocks using block char
        print("## ## ## ## ", 0x09); print("## ## ## ## ", 0x0B);
        print("  root@mectov\n", 0x0A);
        // Row 2: color blocks + separator
        print("  ", 0x00);
        print("## ## ## ## ", 0x01); print("## ## ## ## ", 0x03);
        print("  --------------\n", 0x0F);
        // Row 3: color blocks + OS
        print("  ", 0x00);
        print("## ## ## ## ", 0x0D); print("## ## ## ## ", 0x05);
        print("  OS: ", 0x0B); print("Mectov OS v", 0x0F); print(OS_VERSION, 0x0F); print("\n", 0x0F);
        // Row 4: color blocks + Kernel
        print("  ", 0x00);
        print("## ## ## ## ", 0x0E); print("## ## ## ## ", 0x06);
        print("  Kernel: ", 0x0B); print("Mectov ", 0x0F); print(OS_VERSION, 0x0F); print(".0\n", 0x0F);
        // Row 5: color blocks + Uptime
        print("  ", 0x00);
        print("## ## ## ## ", 0x0C); print("## ## ## ## ", 0x04);
        print("  Uptime: ", 0x0B); print("up ", 0x0F);
        extern uint32_t get_uptime_seconds(void);
        uint32_t up = get_uptime_seconds();
        p_int(up / 60, 0x0F); print(" min ", 0x0F);
        p_int(up % 60, 0x0F); print(" sec\n", 0x0F);
        // Row 6: Shell
        print("  ", 0x00);
        print("## ## ## ## ", 0x0A); print("## ## ## ## ", 0x02);
        print("  Shell: ", 0x0B); print("msh 2.0\n", 0x0F);
        // Row 7: Resolution
        print("  ", 0x00);
        print("## ## ## ## ", 0x09); print("## ## ## ## ", 0x01);
        print("  Resolution: ", 0x0B); p_int(fb_width, 0x0F); print("x", 0x0F); p_int(fb_height, 0x0F); print("\n", 0x0F);
        // Row 8: WM
        print("                        ", 0x00);
        print("  WM: ", 0x0B); print("MectovWM\n", 0x0F);
        // Row 9: CPU
        print("                        ", 0x00);
        extern char cpu_brand[49];
        print("  CPU: ", 0x0B); print(cpu_brand, 0x0F); print("\n", 0x0F);
        // Row 10: RAM
        print("                        ", 0x00);
        print("  RAM: ", 0x0B);
        p_int(get_used_memory()/1024, 0x0F); print(" KB / ", 0x0F);
        p_int(get_total_memory()/1024, 0x0F); print(" KB\n", 0x0F);
    }
    // --- MEM / KMEMSTATS ---
    else if (strcmp(cmd_b, "mem") == 0) {
        print("RAM Status:\n", 0x0B);
        print("Total: ", 0x0F); p_int(get_total_memory()/1024, 0x0A); print(" KB\n", 0x0F);
        print("Free : ", 0x0F); p_int(get_free_memory()/1024, 0x0A); print(" KB\n", 0x0F);
    }
    else if (strcmp(cmd_b, "kmemstats") == 0) {
        kmalloc_stats(print);
    }
    // --- MEMSTAT ---
    else if (strcmp(cmd_b, "memstat") == 0) {
        print("==================================================\n", 0x0B);
        print("                SYSTEM MEMORY STATS               \n", 0x0F);
        print("==================================================\n", 0x0B);
        print("Physical RAM:\n", 0x0E);
        print("  Total Memory : ", 0x0F); p_int(get_total_memory()/1024, 0x0A); print(" KB\n", 0x0F);
        print("  Used Memory  : ", 0x0F); p_int(get_used_memory()/1024, 0x0C); print(" KB\n", 0x0F);
        print("  Free Memory  : ", 0x0F); p_int(get_free_memory()/1024, 0x0A); print(" KB\n", 0x0F);
        print("--------------------------------------------------\n", 0x07);
        kmalloc_stats(print);
        print("==================================================\n", 0x0B);
    }
    // --- UPTIME ---
    else if (strcmp(cmd_b, "uptime") == 0) {
        extern uint32_t get_uptime_seconds(void);
        uint32_t up = get_uptime_seconds();
        uint32_t hours = up / 3600;
        uint32_t minutes = (up % 3600) / 60;
        uint32_t seconds = up % 60;
        
        print("System Uptime:\n", 0x0B);
        print("  Running for: ", 0x0F);
        if (hours > 0) {
            p_int(hours, 0x0A); print(" hour(s), ", 0x0F);
        }
        if (hours > 0 || minutes > 0) {
            p_int(minutes, 0x0A); print(" minute(s), ", 0x0F);
        }
        p_int(seconds, 0x0A); print(" second(s)\n", 0x0F);
        
        print("  Total ticks: ", 0x0F);
        p_int(get_ticks(), 0x0E);
        print("\n", 0x0F);
    }
    // --- VFS INFO ---
    else if (strcmp(cmd_b, "vfsinfo") == 0) {
        int count = 0;
        for (int i = 0; i < MAX_NODES; i++) if (fs_nodes[i].in_use) count++;
        print("VFS STATUS:\n", 0x0B);
        print("  MAX NODES : ", 0x0F); p_int(MAX_NODES, 0x0A); print("\n", 0x0F);
        print("  IN USE    : ", 0x0F); p_int(count, 0x0A); print("\n", 0x0F);
        print("  FREE      : ", 0x0F); p_int(MAX_NODES - count, 0x0A); print("\n", 0x0F);
        print("  ROOT INUSE: ", 0x0F); p_int(fs_nodes[0].in_use, 0x0A); print("\n", 0x0F);
    }
    // --- CD ---
    else if (strncmp(cmd_b, "cd ", 3) == 0 || strcmp(cmd_b, "cd") == 0) {
        if (strcmp(cmd_b, "cd") == 0) {
            set_current_dir(0); // Go to root
        } else {
            char* dirpath = cmd_b + 3;
            sanitize_path(dirpath);
            int node = vfs_get_node(dirpath);
            if (node < 0 || !vfs_is_dir(node)) {
                print("cd: directory not found: ", 0x0C);
                print(dirpath, 0x0C);
                print("\n", 0x0C);
            } else {
                set_current_dir(node);
            }
        }
    }
    // --- PWD ---
    else if (strcmp(cmd_b, "pwd") == 0) {
        char cwd[MAX_PATH];
        vfs_get_abs_path(get_current_dir(), cwd, MAX_PATH);
        print(cwd, 0x0F);
        print("\n", 0x0F);
    }
    // --- LS (new VFS version) ---
    else if (strcmp(cmd_b, "ls") == 0) {
        vfs_list_dir(get_current_dir(), print);
    }
    else if (strncmp(cmd_b, "ls ", 3) == 0) {
        char* dirpath = cmd_b + 3;
        sanitize_path(dirpath);
        int node = vfs_get_node(dirpath);
        if (node < 0 || !vfs_is_dir(node)) {
            print("ls: directory not found: ", 0x0C);
            print(dirpath, 0x0C);
            print("\n", 0x0C);
        } else {
            vfs_list_dir(node, print);
        }
    }
    // --- TREE ---
    else if (strcmp(cmd_b, "tree") == 0) {
        vfs_tree(get_current_dir(), 0, print);
    }
    else if (strncmp(cmd_b, "tree ", 5) == 0) {
        char* dirpath = cmd_b + 5;
        sanitize_path(dirpath);
        int node = vfs_get_node(dirpath);
        if (node < 0 || !vfs_is_dir(node)) {
            print("tree: directory not found\n", 0x0C);
        } else {
            vfs_tree(node, 0, print);
        }
    }
    // --- MKDIR ---
    else if (strncmp(cmd_b, "mkdir ", 6) == 0) {
        char* dirpath = cmd_b + 6;
        sanitize_path(dirpath);
        int res = vfs_mkdir(dirpath);
        if (res < 0) {
            print("mkdir: failed (", 0x0C);
            p_int(res, 0x0C);
            print(")\n", 0x0C);
        }
    }
    // --- TOUCH (create empty file) ---
    else if (strncmp(cmd_b, "touch ", 6) == 0) {
        char* fpath = cmd_b + 6;
        sanitize_path(fpath);
        int res = vfs_create_file(fpath);
        if (res < 0) {
            print("touch: failed\n", 0x0C);
        }
    }
    // --- CAT (read file) ---
    else if (strncmp(cmd_b, "cat ", 4) == 0 || strcmp(cmd_b, "cat") == 0) {
        if (strcmp(cmd_b, "cat") == 0) {
            extern int pipe_buf_len;
            extern char pipe_buffer[];
            if (shell_stdin_len > 0) {
                print(shell_stdin_buf, 0x0F);
            } else if (pipe_buf_len > 0) {
                print(pipe_buffer, 0x0F);
                print("\n", 0x0F);
            } else {
                print("cat: no input\n", 0x0C);
            }
        } else {
            char* fpath = cmd_b + 4;
            sanitize_path(fpath);
            char buf[2048];
            int sz = vfs_read_file(fpath, buf, 2047);
            if (sz < 0) {
                print("cat: file not found\n", 0x0C);
            } else {
                buf[sz] = '\0';
                print(buf, 0x0F);
                // Serial mirror so automated tests can verify file contents
                // (terminal output goes over IPC, not serial).
                extern void write_serial_string(const char*);
                extern void write_serial_hex(uint32_t);
                write_serial_string("[SH] cat ");
                write_serial_hex(sz);
                write_serial_string(" bytes: ");
                for (int ci = 0; ci < sz && ci < 120; ci++) {
                    char cc = buf[ci];
                    if (cc == '\n') write_serial_string("\\n");
                    else write_serial(cc);
                }
                write_serial_string("\n");
                print("\n", 0x0F);
            }
        }
    }
    // --- GREP ---
    else if (strncmp(cmd_b, "grep ", 5) == 0) {
        char* pattern = cmd_b + 5;
        while (*pattern == ' ') pattern++;
        
        extern int pipe_buf_len;
        extern char pipe_buffer[];
        // With '<' redirection, grep consumes the stdin buffer instead.
        const char* src = (shell_stdin_len > 0) ? shell_stdin_buf : pipe_buffer;
        int src_len = (shell_stdin_len > 0) ? shell_stdin_len : pipe_buf_len;
        if (src_len > 0) {
            int i = 0;
            char line[256];
            int line_len = 0;
            while (i < src_len) {
                char c = src[i++];
                if (c == '\n' || c == '\r') {
                    line[line_len] = '\0';
                    if (line_len > 0) {
                        if (strstr_custom(line, pattern) >= 0) {
                            print(line, 0x0A); // Print matching lines in green
                            print("\n", 0x0F);
                            // Serial mirror for stdin-redirected grep so tests
                            // can see matches without the IPC channel.
                            if (shell_stdin_len > 0) {
                                extern void write_serial_string(const char*);
                                write_serial_string("[SH] grep: ");
                                write_serial_string(line);
                                write_serial_string("\n");
                            }
                        }
                    }
                    line_len = 0;
                } else {
                    if (line_len < 255) {
                        line[line_len++] = c;
                    }
                }
            }
            if (line_len > 0) {
                line[line_len] = '\0';
                if (strstr_custom(line, pattern) >= 0) {
                    print(line, 0x0A);
                    print("\n", 0x0F);
                }
            }
        } else {
            print("grep: no input\n", 0x0C);
        }
    }
    // --- UNAME (OS identity, Linux-style) ---
    else if (strcmp(cmd_b, "uname") == 0 || strncmp(cmd_b, "uname ", 6) == 0) {
        // `uname` prints the OS name; `uname -a` prints the full kernel banner
        // (name hostname release version machine).
        if (strncmp(cmd_b, "uname -a", 8) == 0) {
            print("MectovOS mectov ", 0x0F); print(OS_VERSION, 0x0F);
            print(" mectov i686\n", 0x0F);
        } else {
            print("MectovOS\n", 0x0F);
        }
    }
    // --- WHOAMI ---
    else if (strcmp(cmd_b, "whoami") == 0) {
        // Single-user OS: always root (matches the $USER default).
        print("root\n", 0x0F);
    }
    // --- HOSTNAME ---
    else if (strcmp(cmd_b, "hostname") == 0) {
        print("mectov\n", 0x0F);
    }
    // --- ENV (list exported environment variables) ---
    else if (strcmp(cmd_b, "env") == 0) {
        if (env_var_count == 0) {
            print("env: no environment variables\n", 0x0C);
        } else {
            for (int i = 0; i < env_var_count; i++) {
                print(env_vars[i].name, 0x0F);
                print("=", 0x07);
                print(env_vars[i].value, 0x0F);
                print("\n", 0x0F);
            }
        }
    }
    // --- SEQ (print a number sequence) ---
    else if (strncmp(cmd_b, "seq ", 4) == 0 || strcmp(cmd_b, "seq") == 0) {
        // seq LAST       → 1 2 ... LAST
        // seq FIRST LAST → FIRST ... LAST (descending works too)
        char* arg = cmd_b + 4;
        while (*arg == ' ') arg++;
        int first = 1, last = -1;
        // Accept numbers only (atoi returns 0 for garbage, which would
        // otherwise print a bogus "1 0" sequence).
        if (*arg >= '0' && *arg <= '9') {
            int a = atoi(arg);
            while (*arg >= '0' && *arg <= '9') arg++;
            if (*arg == ' ') {
                while (*arg == ' ') arg++;
                if (*arg >= '0' && *arg <= '9') {
                    first = a;
                    last = atoi(arg);
                }
            } else {
                last = a;
            }
        }
        if (last < 0) {
            print("seq: usage: seq [FIRST] LAST\n", 0x0C);
        } else if (first <= last) {
            for (int i = first; i <= last; i++) {
                p_int(i, 0x0F);
                if (i < last) print(" ", 0x0F);
            }
            print("\n", 0x0F);
        } else {
            for (int i = first; i >= last; i--) {
                p_int(i, 0x0F);
                if (i > last) print(" ", 0x0F);
            }
            print("\n", 0x0F);
        }
    }
    // --- HEAD (print the first N lines of a file or stdin) ---
    else if (strncmp(cmd_b, "head ", 5) == 0 || strcmp(cmd_b, "head") == 0) {
        // head [FILE]      → first 10 lines
        // head -n N [FILE] → first N lines
        // head (no args)   → read from pipe / '<' redirection like cat
        int lines = 10;
        char fpath[MAX_PATH];
        int have_file = 0;
        if (strncmp(cmd_b, "head -n ", 8) == 0) {
            char* p = cmd_b + 8;
            lines = atoi(p);
            if (lines < 1) lines = 1;
            while (*p >= '0' && *p <= '9') p++;
            while (*p == ' ') p++;
            if (*p) {
                strncpy(fpath, p, MAX_PATH - 1);
                fpath[MAX_PATH - 1] = '\0';
                have_file = 1;
            }
        } else if (strncmp(cmd_b, "head ", 5) == 0) {
            strncpy(fpath, cmd_b + 5, MAX_PATH - 1);
            fpath[MAX_PATH - 1] = '\0';
            have_file = 1;
        }
        if (have_file) {
            sanitize_path(fpath);
            char buf[2048];
            int sz = vfs_read_file(fpath, buf, 2047);
            if (sz < 0) {
                print("head: file not found: ", 0x0C);
                print(fpath, 0x0C);
                print("\n", 0x0C);
            } else {
                int nl = 0;
                // Serial mirror so automated tests can verify head's output
                // (terminal output goes over IPC, not serial). Mirrors the
                // bytes actually printed (after -n truncation), like cat's.
                extern void write_serial_string(const char*);
                extern void write_serial_hex(uint32_t);
                int printed = 0;
                for (int i = 0; i < sz && nl < lines; i++) {
                    char c = buf[i];
                    if (c == '\n') nl++;
                    p_char(c, 0x0F);
                    printed++;
                }
                write_serial_string("[SH] head ");
                write_serial_hex(printed);
                write_serial_string(" bytes: ");
                for (int i = 0; i < printed; i++) {
                    char cc = buf[i];
                    if (cc == '\n') write_serial_string("\\n");
                    else write_serial(cc);
                }
                write_serial_string("\n");
                if (printed > 0 && buf[printed - 1] != '\n') print("\n", 0x0F);
            }
        } else {
            // stdin (pipe / '<' redirection)
            extern int pipe_buf_len;
            extern char pipe_buffer[];
            const char* src = (shell_stdin_len > 0) ? shell_stdin_buf : pipe_buffer;
            int src_len = (shell_stdin_len > 0) ? shell_stdin_len : pipe_buf_len;
            if (src_len > 0) {
                int nl = 0;
                extern void write_serial_string(const char*);
                write_serial_string("[SH] head stdin: ");
                for (int i = 0; i < src_len && nl < lines; i++) {
                    char c = src[i];
                    if (c == '\n') nl++;
                    p_char(c, 0x0F);
                    if (c == '\n') write_serial_string("\\n");
                    else write_serial(c);
                }
                write_serial_string("\n");
                if (src_len > 0 && src[src_len - 1] != '\n') print("\n", 0x0F);
            } else {
                print("head: no input\n", 0x0C);
            }
        }
    }
    // --- RM (delete) ---
    else if (strncmp(cmd_b, "rm ", 3) == 0) {
        char* fpath = cmd_b + 3;
        sanitize_path(fpath);
        int res = vfs_delete_node(fpath);
        if (res < 0) {
            print("rm: failed\n", 0x0C);
        }
    }
    // --- RMDIR (remove empty directory) ---
    else if (strncmp(cmd_b, "rmdir ", 6) == 0) {
        char* dpath = cmd_b + 6;
        sanitize_path(dpath);
        int node = vfs_get_node(dpath);
        if (node < 0 || !vfs_is_dir(node)) {
            print("rmdir: not a directory: ", 0x0C);
            print(dpath, 0x0C);
            print("\n", 0x0C);
        } else {
            // Refuse non-empty directories (vfs_delete_node would recurse).
            int has_child = 0;
            for (int i = 0; i < MAX_NODES; i++) {
                if (fs_nodes[i].in_use && fs_nodes[i].parent == node) {
                    has_child = 1;
                    break;
                }
            }
            if (has_child) {
                print("rmdir: directory not empty: ", 0x0C);
                print(dpath, 0x0C);
                print("\n", 0x0C);
            } else if (vfs_delete_node(dpath) < 0) {
                print("rmdir: failed\n", 0x0C);
            }
        }
    }
    // --- CP (copy file) ---
    else if (strncmp(cmd_b, "cp ", 3) == 0) {
        // Parse "cp src dst"
        char src[MAX_PATH], dst[MAX_PATH];
        char* rest = cmd_b + 3;
        while (*rest == ' ') rest++;
        int si = 0;
        while (*rest && *rest != ' ' && si < MAX_PATH - 1) src[si++] = *rest++;
        src[si] = '\0';
        while (*rest == ' ') rest++;
        int di = 0;
        while (*rest && *rest != ' ' && di < MAX_PATH - 1) dst[di++] = *rest++;
        dst[di] = '\0';
        sanitize_path(src);
        sanitize_path(dst);
        
        if (src[0] == '\0' || dst[0] == '\0') {
            print("cp: usage: cp [source] [destination]\n", 0x0C);
        } else {
            int snode = vfs_get_node(src);
            if (snode < 0 || !vfs_is_file(snode)) {
                print("cp: source not found or not a file: ", 0x0C);
                print(src, 0x0C);
                print("\n", 0x0C);
            } else if (vfs_get_node(dst) >= 0) {
                print("cp: destination already exists: ", 0x0C);
                print(dst, 0x0C);
                print("\n", 0x0C);
            } else {
                int size = fs_nodes[snode].size;
                if (size < 0 || size > 4 * 1024 * 1024) {
                    print("cp: file too large\n", 0x0C);
                } else {
                    char* buf = (char*)kmalloc(size + 1);
                    if (!buf) {
                        print("cp: out of memory\n", 0x0C);
                    } else {
                        int rd = vfs_read_file(src, buf, size);
                        if (rd < 0) {
                            print("cp: read failed\n", 0x0C);
                        } else if (vfs_create_file(dst) < 0) {
                            print("cp: cannot create destination\n", 0x0C);
                        } else {
                            // Check the written count, not just the sign: on a
                            // full disk ext2_write_file_data returns the bytes
                            // written so far (positive but < rd), which would
                            // otherwise be reported as a successful copy.
                            int wr = vfs_write_file(dst, buf, rd);
                            if (wr != rd) {
                                print("cp: write failed (disk full? wrote ", 0x0C);
                                p_int(wr, 0x0C);
                                print(" of ", 0x0C);
                                p_int(rd, 0x0C);
                                print(" bytes)\n", 0x0C);
                            } else {
                                print("cp: copied ", 0x0A);
                                print(src, 0x0F);
                                print(" -> ", 0x07);
                                print(dst, 0x0F);
                                print(" (", 0x07);
                                p_int(rd, 0x0A);
                                print(" bytes)\n", 0x07);
                            }
                        }
                        kfree(buf);
                    }
                }
            }
        }
    }
    // --- MV (move / rename) ---
    else if (strncmp(cmd_b, "mv ", 3) == 0) {
        // Parse "mv src dst"
        char src[MAX_PATH], dst[MAX_PATH];
        char* rest = cmd_b + 3;
        while (*rest == ' ') rest++;
        int si = 0;
        while (*rest && *rest != ' ' && si < MAX_PATH - 1) src[si++] = *rest++;
        src[si] = '\0';
        while (*rest == ' ') rest++;
        int di = 0;
        while (*rest && *rest != ' ' && di < MAX_PATH - 1) dst[di++] = *rest++;
        dst[di] = '\0';
        sanitize_path(src);
        sanitize_path(dst);
        
        if (src[0] == '\0' || dst[0] == '\0') {
            print("mv: usage: mv [source] [destination]\n", 0x0C);
        } else {
            // vfs_rename silently deletes an existing destination before
            // moving, so refuse when the destination is a directory — moving a
            // file onto one would erase it (and the VFS has no "move into
            // dir" semantics).
            int dst_node = vfs_get_node(dst);
            if (dst_node >= 0 && vfs_is_dir(dst_node)) {
                print("mv: cannot overwrite a directory: ", 0x0C);
                print(dst, 0x0C);
                print("\n", 0x0C);
            } else {
                int res = vfs_rename(src, dst);
                if (res < 0) {
                    if (res == -4) print("mv: cannot move a directory into itself\n", 0x0C);
                    else if (res == -5) print("mv: cross-directory move not supported on ext2\n", 0x0C);
                    else if (res == -3) print("mv: cannot rename root\n", 0x0C);
                    else print("mv: failed\n", 0x0C);
                } else {
                    print("mv: ", 0x0A);
                    print(src, 0x0F);
                    print(" -> ", 0x07);
                    print(dst, 0x0F);
                    print("\n", 0x0A);
                }
            }
        }
    }
    // --- DF (disk free) ---
    else if (strcmp(cmd_b, "df") == 0) {
        print("Filesystem   1K-blocks   Used   Free   Use%  Mounted on\n", 0x0E);
        // MECTOVFS (drive 0): 1MB disk = 2048 sectors, 65 metadata + node table
        int used_sectors = 65;
        for (int i = 0; i < MAX_NODES; i++) {
            if (fs_nodes[i].in_use && fs_nodes[i].type == FS_FILE) {
                int secs = (fs_nodes[i].size + 511) / 512;
                if (secs < 1) secs = 1;
                used_sectors += secs;
            }
        }
        if (used_sectors > 2048) used_sectors = 2048;
        int free_sectors = 2048 - used_sectors;
        print("mectovfs       ", 0x0B);
        p_int(1024, 0x0F); print("      ", 0x07);
        p_int(used_sectors / 2, 0x0F); print("    ", 0x07);
        p_int(free_sectors / 2, 0x0F); print("    ", 0x07);
        p_int(used_sectors * 100 / 2048, 0x0F); print("%  /", 0x0F);
        print("\n", 0x0F);
        
        // ext2 (drive 1)
        uint32_t tblocks = 0, fblocks = 0, tinodes = 0, finodes = 0, bsize = 1024;
        if (ext2_get_stats(&tblocks, &fblocks, &tinodes, &finodes, &bsize) == 0 && tblocks > 0) {
            uint32_t total_kb = tblocks * bsize / 1024;
            uint32_t free_kb = fblocks * bsize / 1024;
            uint32_t used_kb = total_kb - free_kb;
            uint32_t pct = used_kb * 100 / total_kb;
            print("ext2           ", 0x0B);
            p_int(total_kb, 0x0F); print("      ", 0x07);
            p_int(used_kb, 0x0F); print("    ", 0x07);
            p_int(free_kb, 0x0F); print("    ", 0x07);
            p_int(pct, 0x0F); print("%  /ext2", 0x0F);
            print("\n", 0x0F);
            print("  Inodes: ", 0x07);
            p_int(tinodes - finodes, 0x0F); print(" used / ", 0x07);
            p_int(tinodes, 0x0F); print(" total\n", 0x07);
        } else {
            print("ext2           not mounted\n", 0x07);
        }
    }
    // --- SHUTDOWN / REBOOT ---
    // Note: the Indonesian names below (and in the other legacy handlers) are
    // fallbacks — in the normal flow expand_alias() already mapped them to the
    // English commands. They stay reachable after `unalias <id-name>` or when a
    // script runs before the alias table is initialized, so don't delete them.
    else if (strcmp(cmd_b, "matikan") == 0 || strcmp(cmd_b, "shutdown") == 0) shutdown();
    else if (strcmp(cmd_b, "mulaiulang") == 0 || strcmp(cmd_b, "reboot") == 0) reboot();
    // --- LSPCI ---
    else if (strcmp(cmd_b, "lspci") == 0) {
        print("--- PCI Bus Devices ---\n", 0x0B);
        for (int i = 0; i < pci_device_count; i++) {
            pci_device_t *d = &pci_devices[i];
            print(" ", 0x0F);
            print(pci_vendor_name(d->vendor_id), 0x0A);
            print(" | ", 0x07);
            print(pci_class_name(d->class_code, d->subclass), 0x0E);
            print("\n", 0x0F);
        }
    }
    // --- SNAKE (ular) ---
    else if (strcmp(cmd_b, "snake") == 0 || strcmp(cmd_b, "ular") == 0) start_ular();
    // --- FLAPPY ---
    else if (strcmp(cmd_b, "flappy") == 0) load_mct_app("/apps/flappy.mct");
    // --- DOOM ---
    else if (strcmp(cmd_b, "doom") == 0) {
        print("Starting DOOM...\n", 0x0C);
        extern void doom_start(void);
        doom_start();
    }
    // --- TASKMGR ---
    else if (strcmp(cmd_b, "taskmgr") == 0) load_mct_app("/apps/taskmgr.mct");
    // --- LOCK (kunci) ---
    else if (strcmp(cmd_b, "lock") == 0 || strcmp(cmd_b, "kunci") == 0) lock_screen();
    // --- DATE (waktu) ---
    else if (strcmp(cmd_b, "date") == 0 || strcmp(cmd_b, "waktu") == 0) {
        rtc_time_t tm = rtc_read_time();
        unsigned char h = (tm.hour + 7) % 24;
        print("Current time (WIB): ", 0x0B);
        p_int(h, 0x0F); print(":", 0x0F);
        if (tm.minute < 10) { print("0", 0x0F); }
        p_int(tm.minute, 0x0F); print(":", 0x0F);
        if (tm.second < 10) { print("0", 0x0F); }
        p_int(tm.second, 0x0F); print("\n", 0x0F);
    }
    // --- COLOR (warna) ---
    else if (strcmp(cmd_b, "color") == 0 || strcmp(cmd_b, "warna") == 0) {
        print("Color output is only available on TTY (Text Mode).\n", 0x0E);
    }
    // --- RUN / JALANKAN (run .mct app) ---
    else if (strncmp(cmd_b, "run ", 4) == 0 || strncmp(cmd_b, "jalankan ", 9) == 0) {
        char* fname = (strncmp(cmd_b, "run ", 4) == 0) ? cmd_b + 4 : cmd_b + 9;
        sanitize_path(fname);
        
        // Split program name and arguments
        char* arg = "";
        for (int i = 0; fname[i]; i++) {
            if (fname[i] == ' ') {
                fname[i] = '\0';
                arg = fname + i + 1;
                // Trim leading spaces from argument
                while (*arg == ' ') arg++;
                break;
            }
        }

        // Use new VFS: read file data
        int node = vfs_get_node(fname);
        if (node < 0) {
            print("File not found: ", 0x0C); print(fname, 0x0C); print("\n", 0x0C);
        } else {
            print("Launching MCT app: ", 0x0A); print(fname, 0x0A); print("\n", 0x0A);
            int res = load_mct_app_with_arg(fname, arg);
            if (res >= 0) {
                // Give the app its own process group: a foreground app becomes
                // the controlling terminal's foreground group (Ctrl+C/Z now
                // target the group via task_signal_pgrp); a background app gets
                // its own group so SIGTTIN stops it from reading the terminal.
                extern int task_set_pgrp(int, int);
                extern void task_set_fg_pgrp(int);
                task_set_pgrp(res, res);
                if (shell_bg_flag) {
                    // `run app.mct &` — the app runs on its own; don't grab
                    // the terminal, just track it as a background job. It keeps
                    // its own pgrp (!= fg), so reading the terminal stops it.
                    int jn = register_job(res, fname);
                    print("[+] App in background [", 0x0A);
                    p_int(jn, 0x0A); print("] (Task ID: ", 0x0A);
                    p_int(res, 0x0A); print(")\n", 0x0A);
                } else {
                    print("[+] User Mode Task Created! (Task ID: ", 0x0A); p_int(res, 0x0A); print(")\n", 0x0A);
                    
                    extern int term_app_running;
                    extern int term_app_task_id;
                    term_app_running = 1;
                    term_app_task_id = res;
                    task_set_fg_pgrp(res);   // foreground group owns the terminal
                    return; // DO NOT PRINT PROMPT
                }
            } else {
                print("[-] Failed to execute MCT.\n", 0x0C);
            }
        }
    }
    // --- Legacy: BUAT ---
    else if (strncmp(cmd_b, "buat ", 5) == 0) {
        char* fname = cmd_b + 5;
        sanitize_path(fname);
        // Use new VFS
        int res = vfs_create_file(fname);
        if (res >= 0) print("File created successfully.\n", 0x0A);
        else if (res == -2) print("File already exists.\n", 0x0C);
        else print("Disk is full!\n", 0x0C);
    }
    // --- BACA ---
    else if (strncmp(cmd_b, "baca ", 5) == 0) {
        char* fname = cmd_b + 5;
        sanitize_path(fname);
        char buf[512];
        int sz = vfs_read_file(fname, buf, 511);
        if (sz < 0) print("File not found.\n", 0x0C);
        else { buf[sz] = '\0'; print(buf, 0x0F); print("\n", 0x0F); }
    }
    // --- Legacy: EDIT / TULIS / NANO ---
    else if (strncmp(cmd_b, "edit ", 5) == 0 || strncmp(cmd_b, "tulis ", 6) == 0 || strncmp(cmd_b, "nano ", 5) == 0) {
        char* fname = NULL;
        if (strncmp(cmd_b, "edit ", 5) == 0) fname = cmd_b + 5;
        else if (strncmp(cmd_b, "tulis ", 6) == 0) fname = cmd_b + 6;
        else fname = cmd_b + 5;
        sanitize_path(fname);
        print("Launching Editor...\n", 0x0E);
        st_ed(fname);
    }
    // --- Legacy: HAPUS ---
    else if (strncmp(cmd_b, "hapus ", 6) == 0) {
        char* fname = cmd_b + 6;
        sanitize_path(fname);
        int res = vfs_delete_node(fname);
        if (res < 0) print("File not found.\n", 0x0C);
        else print("File deleted.\n", 0x0A);
    }
    // --- PING ---
    else if (strncmp(cmd_b, "ping ", 5) == 0) {
        char* ip_str = cmd_b + 5;
        uint8_t tip[4] = {0, 0, 0, 0};
        int octet = 0, val = 0;
        for (int i = 0; ip_str[i] && octet < 4; i++) {
            if (ip_str[i] >= '0' && ip_str[i] <= '9') val = val * 10 + (ip_str[i] - '0');
            else if (ip_str[i] == '.') { tip[octet++] = (uint8_t)val; val = 0; }
        }
        if (octet < 4) tip[octet] = (uint8_t)val;
        
        if (!rtl_present) {
            print("No network card detected.\n", 0x0C);
        } else {
            print("PING ", 0x0B);
            p_int(tip[0], 0x0F); print(".", 0x0F);
            p_int(tip[1], 0x0F); print(".", 0x0F);
            p_int(tip[2], 0x0F); print(".", 0x0F);
            p_int(tip[3], 0x0F); print(" ...\n", 0x0F);
            
            if (!net_ready) {
                print("Resolving gateway (ARP)...\n", 0x07);
                net_send_arp_request(gateway_ip);
                if (!net_wait_for(&net_ready, 2000)) {
                    print("ARP timeout: gateway not found.\n", 0x0C);
                    goto ping_done;
                }
                print("Gateway resolved!\n", 0x0A);
            }

            net_send_ping(tip);
            if (net_wait_for(&ping_replied, 3000)) {
                uint32_t ms = (ping_rtt * 1000) / 60;
                print("Reply from ", 0x0A);
                p_int(tip[0], 0x0F); print(".", 0x0F);
                p_int(tip[1], 0x0F); print(".", 0x0F);
                p_int(tip[2], 0x0F); print(".", 0x0F);
                p_int(tip[3], 0x0F);
                print(" time=", 0x0F); p_int(ms, 0x0A); print("ms\n", 0x0F);
            } else {
                print("Request timed out.\n", 0x0C);
            }
        }
        ping_done: ;
    }
    // --- HOST ---
    else if (strncmp(cmd_b, "host ", 5) == 0) {
        char* domain = cmd_b + 5;
        if (!rtl_present) {
            print("No network card detected.\n", 0x0C);
        } else {
            print("Resolving ", 0x0B); print(domain, 0x0F); print(" ...\n", 0x0F);
            
            if (!net_ready) {
                net_send_arp_request(gateway_ip);
                if (!net_wait_for(&net_ready, 2000)) {
                    print("  [!] Gateway ARP timeout!\n", 0x0C);
                    goto host_done;
                }
            }

            net_send_dns_query(domain);
            if (net_wait_for(&dns_resolved, 3000)) {
                print(domain, 0x0A); print(" has address ", 0x0F);
                p_int(dns_resolved_ip[0], 0x0A); print(".", 0x0A);
                p_int(dns_resolved_ip[1], 0x0A); print(".", 0x0A);
                p_int(dns_resolved_ip[2], 0x0A); print(".", 0x0A);
                p_int(dns_resolved_ip[3], 0x0A); print("\n", 0x0A);
            } else {
                print("Host not found (timeout).\n", 0x0C);
            }
        }
        host_done: ;
    }
    // --- FETCH (HTTP GET over TCP, Ring 0 demo of the connection stack) ---
    else if (strncmp(cmd_b, "fetch ", 6) == 0) {
        char* domain = cmd_b + 6;
        if (!rtl_present) {
            print("No network card detected.\n", 0x0C);
        } else {
            print("Fetching ", 0x0B); print(domain, 0x0F); print(" ...\n", 0x0F);

            if (!net_ready) {
                print("Resolving gateway (ARP)...\n", 0x07);
                net_send_arp_request(gateway_ip);
                if (!net_wait_for(&net_ready, 2000)) {
                    print("  [!] Gateway ARP timeout!\n", 0x0C);
                    goto fetch_done;
                }
            }

            net_send_dns_query(domain);
            if (!net_wait_for(&dns_resolved, 3000)) {
                print("Host not found (timeout).\n", 0x0C);
                goto fetch_done;
            }

            int id = net_tcp_connect(dns_resolved_ip, 80);
            if (id < 0) {
                print("No free TCP connection slots.\n", 0x0C);
                goto fetch_done;
            }
            print("Connecting...\n", 0x07);

            // Wait for the handshake (bounded like net_wait_for: tick deadline
            // plus the spin cap for the interrupt-gate syscall path)
            {
                uint32_t start = get_ticks();
                uint32_t spins = 0;
                while (net_tcp_state(id) != TCP_ESTABLISHED) {
                    if ((get_ticks() - start) >= 5000 || ++spins >= NET_WAIT_MAX_SPINS) break;
                    net_poll();
                }
            }
            if (net_tcp_state(id) != TCP_ESTABLISHED) {
                print("Connection failed.\n", 0x0C);
                net_tcp_close(id);
                goto fetch_done;
            }

            // Build the GET request (bounded by req[256])
            char req[256];
            const char* pre = "GET / HTTP/1.1\r\nHost: ";
            const char* post = "\r\nConnection: close\r\n\r\n";
            int n = 0;
            while (pre[n]) { req[n] = pre[n]; n++; }
            for (int i = 0; domain[i] && n < 250; i++) req[n++] = domain[i];
            for (int i = 0; post[i] && n < 255; i++) req[n++] = post[i];
            req[n] = '\0';

            net_tcp_send(id, (uint8_t*)req, n);
            print("Connected! Response:\n", 0x0A);

            // Drain until EOF (peer FIN), 15s idle cap plus the spin bound
            char rbuf[1024];
            uint32_t start = get_ticks();
            uint32_t spins = 0;
            for (;;) {
                int r = net_tcp_recv(id, (uint8_t*)rbuf, 1024);
                if (r == -1) break; // EOF: gateway closed after the body
                if (r == -2) {      // connection reset / slot freed
                    print("\n[fetch] connection lost.\n", 0x0C);
                    break;
                }
                if (r > 0) {
                    rbuf[r] = '\0';
                    print(rbuf, 0x0F);
                    start = get_ticks();
                    spins = 0;
                } else if ((get_ticks() - start) >= 15000 || ++spins >= NET_WAIT_MAX_SPINS) {
                    print("\n[fetch] idle timeout.\n", 0x0C);
                    break;
                } else {
                    net_poll();
                }
            }
            print("\n[fetch] done.\n", 0x0E);
            net_tcp_close(id);
        }
        fetch_done: ;
    }
    // --- SH / SOURCE ---
    else if (strncmp(cmd_b, "sh ", 3) == 0 || strncmp(cmd_b, "source ", 7) == 0) {
        char* fname = (strncmp(cmd_b, "sh ", 3) == 0) ? cmd_b + 3 : cmd_b + 7;
        sanitize_path(fname);
        run_script(fname);
    }
    // --- EXPORT ---
    else if (strncmp(cmd_b, "export ", 7) == 0 || strcmp(cmd_b, "export") == 0) {
        if (strcmp(cmd_b, "export") == 0) {
            for (int i = 0; i < env_var_count; i++) {
                print("declare -x ", 0x0E);
                print(env_vars[i].name, 0x0F);
                print("=\"", 0x07);
                print(env_vars[i].value, 0x0F);
                print("\"\n", 0x07);
            }
        } else {
            char* arg = cmd_b + 7;
            while (*arg == ' ') arg++;
            
            char name[ENV_NAME_LEN];
            char val[ENV_VAL_LEN];
            int n = 0, v = 0;
            
            while (*arg && *arg != '=' && *arg != ' ' && n < ENV_NAME_LEN - 1) {
                name[n++] = *arg++;
            }
            name[n] = '\0';
            
            if (*arg == '=') {
                arg++;
                char quote = '\0';
                if (*arg == '"' || *arg == '\'') {
                    quote = *arg;
                    arg++;
                }
                while (*arg && v < ENV_VAL_LEN - 1) {
                    if (quote && *arg == quote) {
                        arg++;
                        break;
                    }
                    val[v++] = *arg++;
                }
                val[v] = '\0';
                
                int found = -1;
                for (int i = 0; i < env_var_count; i++) {
                    if (strcmp(env_vars[i].name, name) == 0) {
                        found = i;
                        break;
                    }
                }
                if (found >= 0) {
                    strcpy(env_vars[found].value, val);
                } else if (env_var_count < MAX_ENV_VARS) {
                    strcpy(env_vars[env_var_count].name, name);
                    strcpy(env_vars[env_var_count].value, val);
                    env_var_count++;
                } else {
                    print("export: too many variables\n", 0x0C);
                }
            } else {
                print("usage: export NAME=VALUE\n", 0x0C);
            }
        }
    }
    // --- ALIAS ---
    else if (strncmp(cmd_b, "alias ", 6) == 0 || strcmp(cmd_b, "alias") == 0) {
        if (strcmp(cmd_b, "alias") == 0) {
            for (int i = 0; i < alias_count; i++) {
                print("alias ", 0x0E);
                print(aliases[i].name, 0x0F);
                print("='", 0x07);
                print(aliases[i].value, 0x0F);
                print("'\n", 0x07);
            }
        } else {
            char* arg = cmd_b + 6;
            while (*arg == ' ') arg++;
            
            char name[ALIAS_NAME_LEN];
            char val[ALIAS_VAL_LEN];
            int n = 0, v = 0;
            
            while (*arg && *arg != '=' && *arg != ' ' && n < ALIAS_NAME_LEN - 1) {
                name[n++] = *arg++;
            }
            name[n] = '\0';
            
            if (*arg == '=') {
                arg++;
                char quote = '\0';
                if (*arg == '"' || *arg == '\'') {
                    quote = *arg;
                    arg++;
                }
                while (*arg && v < ALIAS_VAL_LEN - 1) {
                    if (quote && *arg == quote) {
                        arg++;
                        break;
                    }
                    val[v++] = *arg++;
                }
                val[v] = '\0';
                
                int found = -1;
                for (int i = 0; i < alias_count; i++) {
                    if (strcmp(aliases[i].name, name) == 0) {
                        found = i;
                        break;
                    }
                }
                if (found >= 0) {
                    strcpy(aliases[found].value, val);
                } else if (alias_count < MAX_ALIASES) {
                    strcpy(aliases[alias_count].name, name);
                    strcpy(aliases[alias_count].value, val);
                    alias_count++;
                } else {
                    print("alias: too many aliases\n", 0x0C);
                }
            } else {
                print("usage: alias NAME=\"VALUE\"\n", 0x0C);
            }
        }
    }
    // --- UNALIAS ---
    else if (strncmp(cmd_b, "unalias ", 8) == 0) {
        char* name = cmd_b + 8;
        while (*name == ' ') name++;
        
        int found = -1;
        for (int i = 0; i < alias_count; i++) {
            if (strcmp(aliases[i].name, name) == 0) {
                found = i;
                break;
            }
        }
        
        if (found >= 0) {
            for (int i = found; i < alias_count - 1; i++) {
                aliases[i] = aliases[i + 1];
            }
            alias_count--;
        } else {
            print("unalias: alias not found: ", 0x0C);
            print(name, 0x0C);
            print("\n", 0x0C);
        }
    }
    // --- HISTORY ---
    else if (strcmp(cmd_b, "history") == 0) {
        int idx = hist_next_slot;
        int count = 0;
        if (hist_count < HIST_MAX) {
            idx = 0;
        }
        while (count < hist_count) {
            p_int(count + 1, 0x0E);
            print("  ", 0x07);
            print(history[idx], 0x0F);
            print("\n", 0x0F);
            idx = (idx + 1) % HIST_MAX;
            count++;
        }
    }
    // --- PS (Process Status) ---
    else if (strcmp(cmd_b, "ps") == 0) {
        print("========================================================\n", 0x0B);
        print("  PID  RING  PRIO  STATE  PROCESS NAME\n", 0x0E);
        print("========================================================\n", 0x0B);
        for (int i = 0; i < 64; i++) {
            task_info_t info;
            if (get_task_info(i, &info)) {
                // Print PID
                print("  ", 0x0F);
                p_int(info.id, 0x0F);
                if (info.id < 10) print("    ", 0x0F);
                else print("   ", 0x0F);

                // Print Ring
                p_int(info.ring, 0x0F);
                print("     ", 0x0F);

                // Print Priority
                p_int(info.priority, 0x0F);
                print("    ", 0x0F);

                // Print State (1=RUNNING, 2=READY, 3=SLEEP)
                if (info.state == 1)      print("RUN     ", 0x0A);
                else if (info.state == 2) print("RDY     ", 0x0B);
                else if (info.state == 3) print("SLP     ", 0x07);
                else                      print("UNK     ", 0x0C);

                // Print Process Name (launch arg)
                const char* name = task_get_launch_arg(info.id);
                if (name && name[0] != '\0') {
                    print(name, 0x0F);
                } else {
                    print("unknown", 0x07);
                }
                print("\n", 0x0F);
            }
        }
        print("========================================================\n", 0x0B);
    }
    // --- JOBS ---
    else if (strcmp(cmd_b, "jobs") == 0) {
        print_jobs();
        if (job_count == 0) print("No background jobs.\n", 0x07);
    }
    // --- FG: bring a background job to the foreground (wait for it) ---
    else if (strncmp(cmd_b, "fg", 2) == 0 &&
             (cmd_b[2] == '\0' || cmd_b[2] == ' ')) {
        int num = (cmd_b[2] == ' ') ? atoi(cmd_b + 3) : -1;
        if (num <= 0) {
            print("Usage: fg [job_number]  — wait for a background job\n", 0x0E);
        } else {
            int t = find_job_tid(num);
            if (t < 0) {
                print("fg: no such job\n", 0x0C);
            } else {
                extern int task_waitpid(int pid, int* status, int options);
                extern int task_signal(int tid, int sig);
                extern int task_get_state(int tid);
                int st = -1;
                // Resume a Ctrl+Z-suspended job before waiting on it.
                if (task_get_state(t) == TASK_STATE_STOPPED) {
                    task_signal(t, SIGCONT);
                    write_serial_string("[JOBS] fg SIGCONT tid=");
                    write_serial_hex(t);
                    write_serial_string("\n");
                }
                int r = task_waitpid(t, &st, 0);
                if (r >= 0) {
                    print("[", 0x0E); p_int(num, 0x0E); print("] Done", 0x0A);
                    print(" (exit ", 0x07); p_int(st, 0x07); print(")\n", 0x07);
                    write_serial_string("[JOBS] fg done status=");
                    write_serial_hex(st);
                    write_serial_string("\n");
                } else {
                    print("fg: job already finished\n", 0x0C);
                }
            }
        }
    }
    // --- BG: resume a stopped (Ctrl+Z) job in the background ---
    else if (strncmp(cmd_b, "bg", 2) == 0 &&
             (cmd_b[2] == '\0' || cmd_b[2] == ' ')) {
        int num = (cmd_b[2] == ' ') ? atoi(cmd_b + 3) : -1;
        if (num <= 0) {
            print("Usage: bg [job_number]\n", 0x0E);
        } else {
            int t = find_job_tid(num);
            if (t < 0) {
                print("bg: no such job\n", 0x0C);
            } else {
                extern int task_signal(int tid, int sig);
                extern int task_get_state(int tid);
                if (task_get_state(t) == TASK_STATE_STOPPED) {
                    task_signal(t, SIGCONT);
                    write_serial_string("[JOBS] bg SIGCONT tid=");
                    write_serial_hex(t);
                    write_serial_string("\n");
                    print("bg: job ", 0x0A); p_int(num, 0x0A);
                    print(" resumed in background\n", 0x0A);
                } else {
                    print("bg: job ", 0x0A); p_int(num, 0x0A);
                    print(" is not stopped\n", 0x0C);
                }
            }
        }
    }
    // --- KILL ---
    else if (strncmp(cmd_b, "kill ", 5) == 0) {
        char* arg = cmd_b + 5;
        while (*arg == ' ') arg++;
        if (arg[0] == '%') {
            // Job syntax: kill %n
            int num = atoi(arg + 1);
            write_serial_string("[JOBS] kill % arg='");
            write_serial_string(arg);
            write_serial_string("'\n");
            int t = find_job_tid(num);
            if (t < 0) {
                print("kill: no such job\n", 0x0C);
                write_serial_string("[JOBS] kill no such job\n");
            } else {
                task_kill(t);
                print("Job ", 0x0A); p_int(num, 0x0A); print(" (pid ", 0x0A);
                p_int(t, 0x0A); print(") sent SIGKILL.\n", 0x0A);
                write_serial_string("[JOBS] kill %");
                write_serial_hex(num);
                write_serial_string(" tid=");
                write_serial_hex(t);
                write_serial_string(" SIGKILL sent\n");
            }
        } else {
            int tid = atoi(arg);
            if (tid == 0) {
                print("kill: cannot terminate idle kernel process (PID 0)!\n", 0x0C);
            } else if (tid < 0 || tid >= 64) {
                print("kill: invalid PID!\n", 0x0C);
            } else if (!task_is_alive(tid)) {
                print("kill: process not found!\n", 0x0C);
            } else {
                int res = task_kill(tid);
                if (res == 0) {
                    print("Process ", 0x0A);
                    p_int(tid, 0x0A);
                    print(" terminated successfully.\n", 0x0A);
                } else {
                    print("kill: failed to terminate process!\n", 0x0C);
                }
            }
        }
    }
    else if (strcmp(cmd_b, "kill") == 0) {
        print("Usage: kill [PID | %job]  — Terminate a process or job\n", 0x0E);
    }
    // --- ECHO ---
    else if (strncmp(cmd_b, "echo ", 5) == 0) {
        print(cmd_b + 5, 0x0F);
        print("\n", 0x0F);
    }
    // --- SLEEP (tunggu) ---
    else if (strncmp(cmd_b, "sleep ", 6) == 0 || strncmp(cmd_b, "tunggu ", 7) == 0) {
        int seconds = atoi((strncmp(cmd_b, "sleep ", 6) == 0) ? cmd_b + 6 : cmd_b + 7);
        if (seconds > 0 && seconds < 60000) {
            // task_sleep() consumes PIT ticks, while the command contract says
            // "sleep [sec]". Convert here so user-facing behavior matches
            // the help text and stays scheduler-driven.
            uint64_t ticks64 = (uint64_t)seconds * 1000u;
            if (ticks64 > 0x7FFFFFFF) ticks64 = 0x7FFFFFFF;
            task_sleep((int)ticks64);
        }
    }
    // --- TONE (nada, beep frequency) ---
    else if (strncmp(cmd_b, "tone ", 5) == 0 || strncmp(cmd_b, "nada ", 5) == 0) {
        int freq = atoi(cmd_b + 5);
        if (freq > 20 && freq < 20000) beep(freq, 300);
    }
    // --- BEEP ---
    else if (strcmp(cmd_b, "beep") == 0) {
        beep(880, 200);
    }
    // --- MAN ---
    else if (strncmp(cmd_b, "man ", 4) == 0) {
        char* topic = cmd_b + 4;
        if (strcmp(topic, "cd") == 0) {
            print("cd [dir]  — Change directory\n", 0x0B);
            print("  cd /   = go to root\n", 0x0F);
            print("  cd ..  = go up one level\n", 0x0F);
            print("  cd home/user = relative path\n", 0x0F);
        } else if (strcmp(topic, "ls") == 0) {
            print("ls [dir]  — List directory contents\n", 0x0B);
        } else if (strcmp(topic, "mkdir") == 0) {
            print("mkdir [path] — Create directory\n", 0x0B);
        } else if (strcmp(topic, "rm") == 0) {
            print("rm [path] — Remove file or directory\n", 0x0B);
        } else if (strcmp(topic, "touch") == 0) {
            print("touch [path] — Create empty file\n", 0x0B);
        } else if (strcmp(topic, "cat") == 0) {
            print("cat [path] — Display file contents\n", 0x0B);
        } else if (strcmp(topic, "cp") == 0) {
            print("cp [src] [dst] — Copy a file (VFS or /ext2)\n", 0x0B);
        } else if (strcmp(topic, "mv") == 0) {
            print("mv [src] [dst] — Move/rename a file\n", 0x0B);
        } else if (strcmp(topic, "rmdir") == 0) {
            print("rmdir [path] — Remove an empty directory\n", 0x0B);
        } else if (strcmp(topic, "df") == 0) {
            print("df — Show disk usage for mectovfs and /ext2\n", 0x0B);
        } else if (strcmp(topic, "head") == 0) {
            print("head [-n N] [file] — Print the first N lines (default 10)\n", 0x0B);
        } else if (strcmp(topic, "seq") == 0) {
            print("seq [FIRST] LAST — Print a sequence of numbers\n", 0x0B);
        } else if (strcmp(topic, "uname") == 0) {
            print("uname [-a] — Print OS name (or full kernel banner)\n", 0x0B);
        } else if (strcmp(topic, "whoami") == 0) {
            print("whoami — Print the current user (root)\n", 0x0B);
        } else if (strcmp(topic, "hostname") == 0) {
            print("hostname — Print the machine hostname\n", 0x0B);
        } else if (strcmp(topic, "env") == 0) {
            print("env — List all exported environment variables\n", 0x0B);
        } else {
            print("man: no manual entry for '", 0x0C);
            print(topic, 0x0C);
            print("'\n", 0x0C);
        }
    }
    // --- UNKNOWN ---
    else if (cmd_b[0] != '\0') {
        print("Command not found: ", 0x0C);
        print(cmd_b, 0x0C);
        print("\n", 0x0C);
    }
    
    b_idx = 0;
}

void run_script(const char* f) {
    // A script containing `sh`/`source` would recurse here until the kernel
    // stack overflows — `is_script` was set but never read. Refuse nested
    // script execution outright.
    if (is_script) {
        print("sh: nested script execution is not allowed\n", 0x0C);
        return;
    }
    is_script = 1;
    
    // Find file in new VFS
    char buf[2048];
    int sz = vfs_read_file(f, buf, 2047);
    if (sz < 0) {
        print("sh: ", 0x0C);
        print(f, 0x0C);
        print(": No such file or directory\n", 0x0C);
        is_script = 0;
        return;
    }
    buf[sz] = '\0';
    
    // Parse line by line
    int i = 0, j = 0;
    while (buf[i]) {
        if (buf[i] == '\n' || buf[i] == '\r') {
            if (j > 0) {
                cmd_b[j] = '\0';
                b_idx = j;
                
                int start = 0;
                while (cmd_b[start] == ' ') start++;
                
                // Skip comments (#) and empty lines
                if (cmd_b[start] != '#' && cmd_b[start] != '\0') {
                    print("> ", 0x0A);
                    print(cmd_b + start, 0x0F);
                    if (start > 0) {
                        memmove(cmd_b, cmd_b + start, j - start + 1);
                        b_idx = j - start;
                    }
                    ex_cmd();
                }
                j = 0;
            }
        } else if (j < CMD_BUF_SIZE - 1) {
            cmd_b[j++] = buf[i];
        }
        i++;
    }
    if (j > 0) {
        cmd_b[j] = '\0';
        b_idx = j;
        int start = 0;
        while (cmd_b[start] == ' ') start++;
        if (cmd_b[start] != '#' && cmd_b[start] != '\0') {
            print("> ", 0x0A);
            print(cmd_b + start, 0x0F);
            if (start > 0) {
                memmove(cmd_b, cmd_b + start, j - start + 1);
                b_idx = j - start;
            }
            ex_cmd();
        }
    }
    is_script = 0;
}

// Kernel entry point for a background command: reached by iret from the
// forked child's patched frame (never returns to user mode). Reads its own
// command from its launch_arg (written before the child could run), executes
// it, then exits with status 0.
static void bg_child_entry(void) {
    const char* arg = task_get_launch_arg(get_current_task());
    if (arg && arg[0]) {
        int i = 0;
        for (; arg[i] && i < CMD_BUF_SIZE - 1; i++) cmd_b[i] = arg[i];
        cmd_b[i] = '\0';
        b_idx = i;
    }
    run_cmd_internal();
    task_exit_with_code(0);
}

void ex_cmd() {
    print("\n", 0x0F);
    cmd_b[b_idx] = '\0';

    // Lazy-init the env/alias tables. The other init site is
    // shell_print_prompt(), but the Ring-3 terminal renders its own prompt, so
    // defaults like $USER would stay unset there until the first `export`.
    // Guarded by the shared module-level flag so user-added vars are never
    // reset by a second init.
    if (!env_alias_initialized) {
        init_env_vars_and_aliases();
        env_alias_initialized = 1;
    }

    // Save to history (skip empty)
    if (b_idx > 0) {
        history_add(cmd_b);
        strcpy(hist_b, cmd_b);
        
        // Expand environment variables and aliases
        char expanded1[512];
        char expanded2[512];
        expand_env_vars(expanded1, cmd_b, 512);
        expand_alias(expanded2, expanded1, 512);
        
        strncpy(cmd_b, expanded2, CMD_BUF_SIZE - 1);
        cmd_b[CMD_BUF_SIZE - 1] = '\0';
        b_idx = strlen(cmd_b);
    }
    
    shell_reset_history_nav();
    
    // Background job: a trailing '&' (after trimming) runs the command in the
    // background. `run`/`jalankan` already spawn their own task, so those just
    // launch without grabbing the terminal; anything else is forked — the
    // child is a copy of this task that iret's straight into bg_child_entry,
    // runs the command in the kernel and exits.
    {
        int e = b_idx - 1;
        while (e >= 0 && (cmd_b[e] == ' ' || cmd_b[e] == '\t')) { cmd_b[e] = '\0'; e--; }
        if (e >= 0 && cmd_b[e] == '&') {
            cmd_b[e] = '\0';
            e--;
            while (e >= 0 && (cmd_b[e] == ' ' || cmd_b[e] == '\t')) { cmd_b[e] = '\0'; e--; }
            b_idx = (e >= 0) ? e + 1 : 0;

            int is_run = (strncmp(cmd_b, "run ", 4) == 0 || strncmp(cmd_b, "jalankan ", 9) == 0);
            if (is_run) {
                shell_bg_flag = 1;
                run_cmd_internal();
                shell_bg_flag = 0;
                b_idx = 0;
                return;
            }

            extern int task_fork_kernel(void (*entry)(void), const char* arg);
            extern int task_in_kernel_space(int);
            int me = get_current_task();
            if (me > 0 && !task_in_kernel_space(me)) {
                int child = task_fork_kernel(bg_child_entry, cmd_b);
                if (child > 0) {
                    int jn = register_job(child, cmd_b);
                    print("[", 0x0E); p_int(jn, 0x0E); print("] ", 0x0E);
                    print("background pid=", 0x0A); p_int(child, 0x0A);
                    print("\n", 0x0F);
                    b_idx = 0;
                    return;
                }
            }
            // Cannot fork (kernel task / no slots): run synchronously instead.
            run_cmd_internal();
            b_idx = 0;
            return;
        }
    }
    
    // ---- File redirection: > (truncate), >> (append), < (stdin) ----
    // Output redirection reuses the pipe capture mechanism (pipe_active ->
    // pipe_buffer): run the left-hand command with capture on, then write the
    // captured bytes to the file. '<' feeds the file's contents into a shell
    // stdin buffer that cat/grep consume when they would otherwise read the
    // pipe buffer.
    {
        int redir_gt = -1, redir_append = 0, redir_lt = -1;
        for (int i = 0; cmd_b[i]; i++) {
            if (cmd_b[i] == '>' && redir_gt < 0) {
                redir_gt = i;
                if (cmd_b[i + 1] == '>') { redir_append = 1; i++; }
            } else if (cmd_b[i] == '<' && redir_lt < 0) {
                redir_lt = i;
            }
        }

        if (redir_gt >= 0 || redir_lt >= 0) {
            // Split into: command part, redirect part
            int split_at = (redir_gt >= 0 && (redir_lt < 0 || redir_gt < redir_lt)) ? redir_gt : redir_lt;
            int second_at = (redir_gt >= 0 && (redir_lt < 0 || redir_gt < redir_lt)) ? redir_lt : redir_gt;

            // Command = everything before the first redirection token
            char redir_cmd[256];
            int clen = split_at;
            if (clen > 255) clen = 255;
            strncpy(redir_cmd, cmd_b, clen);
            redir_cmd[clen] = '\0';
            int ce = clen - 1;
            while (ce >= 0 && redir_cmd[ce] == ' ') { redir_cmd[ce] = '\0'; ce--; }

            // Target file = token after '>' (or '<')
            char redir_file[128];
            int fi = 0;
            int k = (redir_gt >= 0 && (redir_lt < 0 || redir_gt < redir_lt))
                        ? redir_gt + 1 + redir_append : redir_lt + 1;
            while (cmd_b[k] == ' ') k++;
            while (cmd_b[k] && cmd_b[k] != ' ' && cmd_b[k] != '>' && cmd_b[k] != '<' && fi < 127) {
                redir_file[fi++] = cmd_b[k++];
            }
            redir_file[fi] = '\0';
            (void)second_at;

            if (redir_cmd[0] == '\0' || redir_file[0] == '\0') {
                print("sh: syntax error in redirection\n", 0x0C);
                b_idx = 0;
                return;
            }

            // ---- REAL redirection for external apps: `run app > file` ----
            // The in-process capture below only works for builtins. An
            // external app is its own task writing via fds, so spawn a child
            // with its fd 1 (or fd 0) wired to the opened file via
            // task_fork_exec, then wait for it like fg.
            {
                int is_run = (strncmp(redir_cmd, "run ", 4) == 0 ||
                              strncmp(redir_cmd, "jalankan ", 9) == 0);
                if (is_run) {
                    extern int do_sys_open(const char* path, int mode);
                    extern int do_sys_close(int fd);
                    extern int task_fork_exec(int in_fd, int out_fd,
                                              const char* path, const char* arg);
                    extern int task_waitpid(int pid, int* status, int options);

                    // Parse app path + arg from "run /apps/x.mct arg"
                    const char* ap = (strncmp(redir_cmd, "run ", 4) == 0)
                                        ? redir_cmd + 4 : redir_cmd + 9;
                    char apath[128], aarg[64];
                    char acopy[192];
                    strncpy(acopy, ap, 191); acopy[191] = '\0';
                    char* sp = acopy; while (*sp == ' ') sp++;
                    int pi = 0;
                    while (sp[pi] && sp[pi] != ' ' && pi < 127) { apath[pi] = sp[pi]; pi++; }
                    apath[pi] = '\0';
                    int ai = 0; while (sp[pi] == ' ') pi++;
                    while (sp[pi] && ai < 63) { aarg[ai++] = sp[pi++]; }
                    aarg[ai] = '\0';

                    if (apath[0]) {
                        int in_fd = -1, out_fd = -1;
                        if (redir_gt >= 0) {
                            // Output redirection: ensure the file exists, open
                            // it, wire it as the child's stdout.
                            extern int vfs_get_node(const char* path);
                            extern int vfs_create_file(const char* path);
                            if (vfs_get_node(redir_file) < 0) vfs_create_file(redir_file);
                            out_fd = do_sys_open(redir_file, 0);
                        } else if (redir_lt >= 0) {
                            in_fd = do_sys_open(redir_file, 0);
                        }

                        if ((redir_gt >= 0 && out_fd >= 0) ||
                            (redir_lt >= 0 && in_fd >= 0)) {
                            extern void write_serial_string(const char*);
                            write_serial_string("[JOBS] fork_exec redir ");
                            write_serial_string(apath);
                            write_serial_string(" in=");
                            write_serial_hex(in_fd);
                            write_serial_string(" out=");
                            write_serial_hex(out_fd);
                            write_serial_string("\n");
                            int child = task_fork_exec(in_fd, out_fd, apath, aarg);
                            // Parent closes its copies of the file fds.
                            if (in_fd >= 0) do_sys_close(in_fd);
                            if (out_fd >= 0) do_sys_close(out_fd);
                            if (child > 0) {
                                int st = -1;
                                task_waitpid(child, &st, 0);
                                print("app ", 0x0A); print(apath, 0x0A);
                                print(" finished (redirected)\n", 0x0A);
                                write_serial_string("[JOBS] redir app done\n");
                                b_idx = 0;
                                return;
                            }
                            print("sh: could not spawn app for redirection\n", 0x0C);
                            b_idx = 0;
                            return;
                        }
                    }
                }
            }

            // ---- stdin redirection: load the file into the stdin buffer ----
            if (redir_lt >= 0) {
                extern char shell_stdin_buf[];
                extern int shell_stdin_len;
                shell_stdin_len = vfs_read_file(redir_file, shell_stdin_buf, 4095);
                if (shell_stdin_len < 0) {
                    print("sh: cannot open input file: ", 0x0C);
                    print(redir_file, 0x0C);
                    print("\n", 0x0C);
                    b_idx = 0;
                    return;
                }
                shell_stdin_buf[shell_stdin_len] = '\0';

                strcpy(cmd_b, redir_cmd);
                b_idx = strlen(cmd_b);
                run_cmd_internal();
                shell_stdin_len = 0;
                shell_stdin_buf[0] = '\0';
                b_idx = 0;
                return;
            }

            // ---- output redirection ----
            if (redir_gt >= 0) {
                extern int pipe_active;
                extern char pipe_buffer[];
                extern int pipe_buf_len;

                pipe_buf_len = 0;
                pipe_buffer[0] = '\0';
                pipe_active = 1;

                strcpy(cmd_b, redir_cmd);
                b_idx = strlen(cmd_b);
                run_cmd_internal();

                pipe_buffer[pipe_buf_len] = '\0';
                pipe_active = 0;

                // Ensure the target file exists before writing (vfs_write_file
                // requires an existing node). '>>' on a missing file creates it
                // just like '>'.
                if (vfs_get_node(redir_file) < 0) {
                    vfs_create_file(redir_file);
                }

                // '>' truncates, '>>' appends. Build the final content and write.
                int written = pipe_buf_len;
                int new_bytes = pipe_buf_len;   // bytes from this command (mirror)
                if (redir_append) {
                    char existing[4096];
                    int esz = vfs_read_file(redir_file, existing, 4095);
                    if (esz < 0) esz = 0;
                    existing[esz] = '\0';
                    if (esz + pipe_buf_len < 4096) {
                        for (int i = 0; i < pipe_buf_len; i++) existing[esz + i] = pipe_buffer[i];
                        existing[esz + pipe_buf_len] = '\0';
                        vfs_write_file(redir_file, existing, esz + pipe_buf_len);
                        written = esz + pipe_buf_len;
                    }
                } else {
                    vfs_write_file(redir_file, pipe_buffer, pipe_buf_len);
                }

                print("redirected ", 0x0A);
                p_int(written, 0x0A);
                print(" bytes -> ", 0x0A);
                print(redir_file, 0x0A);
                print("\n", 0x0F);
                // Serial mirror so automated tests can verify the write
                // (terminal output itself goes over IPC, not serial). Do this
                // BEFORE pipe_buffer is reset.
                extern void write_serial_string(const char*);
                write_serial_string("[SH] redirected ");
                write_serial_hex(written);
                write_serial_string(" bytes -> ");
                write_serial_string(redir_file);
                write_serial_string(" content='");
                int mi = 0;
                while (mi < new_bytes && mi < 80) {
                    char mc = pipe_buffer[mi++];
                    if (mc == '\n') write_serial_string("\\n");
                    else write_serial(mc);
                }
                write_serial_string("'\n");
                pipe_buf_len = 0;
                pipe_buffer[0] = '\0';
                b_idx = 0;
                return;
            }
        }
    }

    // Check for pipe '|'
    int pipe_idx = -1;
    for (int i = 0; cmd_b[i]; i++) {
        if (cmd_b[i] == '|') {
            pipe_idx = i;
            break;
        }
    }
    
    if (pipe_idx >= 0) {
        char cmd1[256];

        // ---- REAL pipeline for external apps: `run A | run B` ----
        // The in-process capture below only works for shell builtins (print()
        // goes through pipe_buffer). An external app is its own task writing
        // via fds, so a real pipe + fork/exec is required: create a kernel
        // pipe, spawn one child per side with its fd 1 / fd 0 wired to the
        // pipe ends (task_fork_exec), then wait for both.
        {
            char side[256];
            int slen = pipe_idx;
            if (slen > 255) slen = 255;
            strncpy(side, cmd_b, slen);
            side[slen] = '\0';
            int se = slen - 1;
            while (se >= 0 && side[se] == ' ') side[se--] = '\0';

            int is_run_l = (strncmp(side, "run ", 4) == 0 || strncmp(side, "jalankan ", 9) == 0);
            int k2 = pipe_idx + 1;
            while (cmd_b[k2] == ' ') k2++;
            int slen2 = 0;
            while (cmd_b[k2 + slen2]) { if (slen2 < 255) side[slen2] = cmd_b[k2 + slen2]; slen2++; }
            side[slen2 > 255 ? 255 : slen2] = '\0';
            int se2 = (slen2 > 255 ? 255 : slen2) - 1;
            while (se2 >= 0 && side[se2] == ' ') side[se2--] = '\0';
            int is_run_r = (strncmp(side, "run ", 4) == 0 || strncmp(side, "jalankan ", 9) == 0);

            if (is_run_l && is_run_r) {
                // Re-extract the two command strings cleanly.
                char lc[256], rc[256];
                int llen = pipe_idx;
                if (llen > 255) llen = 255;
                strncpy(lc, cmd_b, llen); lc[llen] = '\0';
                int le = llen - 1;
                while (le >= 0 && lc[le] == ' ') lc[le--] = '\0';
                int rp = pipe_idx + 1;
                while (cmd_b[rp] == ' ') rp++;
                int rlen = 0;
                while (cmd_b[rp + rlen] && rlen < 255) { rc[rlen] = cmd_b[rp + rlen]; rlen++; }
                rc[rlen] = '\0';
                int re = rlen - 1;
                while (re >= 0 && rc[re] == ' ') rc[re--] = '\0';

                // Strip the leading run/jalankan + split path and arg.
                const char* lp = (strncmp(lc, "run ", 4) == 0) ? lc + 4 : lc + 9;
                const char* rp2 = (strncmp(rc, "run ", 4) == 0) ? rc + 4 : rc + 9;
                char lpath[128], larg[64], rpath[128], rarg[64];
                char lcopy[192], rcopy[192];
                strncpy(lcopy, lp, 191); lcopy[191] = '\0';
                strncpy(rcopy, rp2, 191); rcopy[191] = '\0';
                char* sp = lcopy; while (*sp == ' ') sp++;
                int pi = 0; while (sp[pi] && sp[pi] != ' ' && pi < 127) { lpath[pi] = sp[pi]; pi++; }
                lpath[pi] = '\0';
                int ai = 0; while (sp[pi] == ' ') pi++;
                while (sp[pi] && ai < 63) { larg[ai++] = sp[pi++]; }
                larg[ai] = '\0';
                sp = rcopy; while (*sp == ' ') sp++;
                pi = 0; while (sp[pi] && sp[pi] != ' ' && pi < 127) { rpath[pi] = sp[pi]; pi++; }
                rpath[pi] = '\0';
                ai = 0; while (sp[pi] == ' ') pi++;
                while (sp[pi] && ai < 63) { rarg[ai++] = sp[pi++]; }
                rarg[ai] = '\0';

                if (lpath[0] && rpath[0]) {
                    extern int do_sys_pipe(int pipefd[2]);
                    extern int do_sys_close(int fd);
                    extern int task_fork_exec(int in_fd, int out_fd,
                                              const char* path, const char* arg);
                    extern int task_waitpid(int pid, int* status, int options);
                    int pfd[2];
                    if (do_sys_pipe(pfd) == 0) {
                        // Left child: stdout -> pipe write end.
                        int cl = task_fork_exec(-1, pfd[1], lpath, larg);
                        // Right child: stdin <- pipe read end.
                        int cr = task_fork_exec(pfd[0], -1, rpath, rarg);
                        if (cl > 0 && cr > 0) {
                            // Parent: close both pipe ends (the children hold
                            // their own references), then reap both children.
                            do_sys_close(pfd[0]);
                            do_sys_close(pfd[1]);
                            int st = -1;
                            task_waitpid(cl, &st, 0);
                            task_waitpid(cr, &st, 0);
                            print("[pipeline] both sides done\n", 0x0A);
                            write_serial_string("[JOBS] real pipeline ok\n");
                            b_idx = 0;
                            return;
                        }
                        // Spawn failed: clean up and fall through to the
                        // in-process capture (harmless for builtins).
                        if (cl > 0) { extern int task_kill(int tid); task_kill(cl); }
                        if (cr > 0) { extern int task_kill(int tid); task_kill(cr); }
                        do_sys_close(pfd[0]);
                        do_sys_close(pfd[1]);
                        print("sh: pipeline spawn failed, falling back\n", 0x0C);
                        write_serial_string("[JOBS] real pipeline spawn failed\n");
                    }
                }
            }
        }
        char cmd2[256];
        
        // Extract cmd1
        int len1 = pipe_idx;
        if (len1 > 255) len1 = 255;
        strncpy(cmd1, cmd_b, len1);
        cmd1[len1] = '\0';
        
        // Trim trailing spaces for cmd1
        int end1 = len1 - 1;
        while (end1 >= 0 && cmd1[end1] == ' ') {
            cmd1[end1] = '\0';
            end1--;
        }
        
        // Extract cmd2
        int len2 = 0;
        int k = pipe_idx + 1;
        while (cmd_b[k] == ' ') k++; // skip leading spaces
        while (cmd_b[k]) {
            if (len2 < 255) {
                cmd2[len2++] = cmd_b[k];
            }
            k++;
        }
        cmd2[len2] = '\0';
        
        // Trim trailing spaces for cmd2
        int end2 = len2 - 1;
        while (end2 >= 0 && cmd2[end2] == ' ') {
            cmd2[end2] = '\0';
            end2--;
        }
        
        // Execute cmd1 with pipe redirection active
        extern int pipe_active;
        extern char pipe_buffer[];
        extern int pipe_buf_len;
        
        pipe_buf_len = 0;
        pipe_buffer[0] = '\0';
        pipe_active = 1;
        
        strcpy(cmd_b, cmd1);
        b_idx = strlen(cmd_b);
        run_cmd_internal();
        
        pipe_buffer[pipe_buf_len] = '\0';
        pipe_active = 0; // Turn off redirection so cmd2 outputs to terminal
        
        // Execute cmd2
        strcpy(cmd_b, cmd2);
        b_idx = strlen(cmd_b);
        run_cmd_internal();
        
        // Reset pipe buffer
        pipe_buf_len = 0;
        pipe_buffer[0] = '\0';
        b_idx = 0;
        return;
    }
    
    // Normal execution (no pipe)
    run_cmd_internal();
    b_idx = 0;
}