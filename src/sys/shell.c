// ============================================================
// shell.c — Mectov OS Shell dengan Tab Completion & History
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

// --- Command buffer & state ---
char cmd_b[CMD_BUF_SIZE]; int b_idx = 0;
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
    
    alias_count = 4;
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

const char* cmd_list[] = {
    "help","clear","mfetch","mem","memstat","kmemstats","uptime","vfsinfo",
    "ls","cd","pwd","mkdir","touch","cat","tree","rm",
    "buat","tulis","edit","nano","baca","hapus",
    "sh","source","export","alias","unalias","history","ps","kill",
    "echo","beep","nada","tunggu","waktu","warna","kunci",
    "jalankan","ular","taskmgr","flappy","doom","lspci","man",
    "ping","host","grep",
    "matikan","mulaiulang","shutdown","reboot", NULL
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
    static int initialized = 0;
    if (!initialized) {
        init_env_vars_and_aliases();
        initialized = 1;
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
        while (tab_matches[0][k]) {
            cmd_b[j++] = tab_matches[0][k++];
        }
        // If directory, add trailing /
        int node = vfs_get_node(tab_matches[0]);
        if (node >= 0 && vfs_is_dir(node)) {
            cmd_b[j++] = '/';
        } else if (is_first_word) {
            // Add trailing space after command name for convenience
            cmd_b[j++] = ' ';
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
        print("                ⚡ MECTOV OS v27.0 - COMMAND CENTER ⚡                \n", 0x0F);
        print("======================================================================\n", 0x0B);
        print(" SYSTEM  : ", 0x0B); print("mfetch, waktu, warna, clear, mem, memstat, kmemstats, uptime, kunci, ps, kill\n", 0x0F);
        print(" FILE VFS: ", 0x0B); print("ls, cd, pwd, mkdir, touch, cat, tree, rm, buat, hapus\n", 0x0F);
        print(" EDITOR  : ", 0x0B); print("nano, edit, tulis, baca\n", 0x0F);
        print(" SHELL   : ", 0x0B); print("export [NAME=VAL], alias [NAME=VAL], unalias, history, sh\n", 0x0F);
        print(" APPS GUI: ", 0x0B); print("flappy, doom, taskmgr, ular, jalankan [app.mct]\n", 0x0A);
        print(" NET & HW: ", 0x0B); print("ping [ip], host [domain], lspci\n", 0x0F);
        print(" UTILS   : ", 0x0B); print("echo [msg], tunggu [detik], nada [freq], beep, man [cmd]\n", 0x0F);
        print(" POWER   : ", 0x0B); print("reboot, shutdown (mulaiulang, matikan)\n", 0x0C);
        print("----------------------------------------------------------------------\n", 0x07);
        print(" SHORTCUT: ", 0x0E); print("Tab=Autocomplete  |  Up/Down=History  |  Pipes: cmd1 | cmd2\n", 0x0F);
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
        print("  OS: ", 0x0B); print("Mectov OS v27.0\n", 0x0F);
        // Row 4: color blocks + Kernel
        print("  ", 0x00);
        print("## ## ## ## ", 0x0E); print("## ## ## ## ", 0x06);
        print("  Kernel: ", 0x0B); print("Mectov 27.0.0\n", 0x0F);
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
            if (pipe_buf_len > 0) {
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
        if (pipe_buf_len > 0) {
            int i = 0;
            char line[256];
            int line_len = 0;
            while (i < pipe_buf_len) {
                char c = pipe_buffer[i++];
                if (c == '\n' || c == '\r') {
                    line[line_len] = '\0';
                    if (line_len > 0) {
                        if (strstr_custom(line, pattern) >= 0) {
                            print(line, 0x0A); // Tampilkan yang cocok dengan warna hijau
                            print("\n", 0x0F);
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
    // --- RM (delete) ---
    else if (strncmp(cmd_b, "rm ", 3) == 0) {
        char* fpath = cmd_b + 3;
        sanitize_path(fpath);
        int res = vfs_delete_node(fpath);
        if (res < 0) {
            print("rm: failed\n", 0x0C);
        }
    }
    // --- SHUTDOWN / REBOOT ---
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
    // --- ULAR ---
    else if (strcmp(cmd_b, "ular") == 0) start_ular();
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
    // --- KUNCI ---
    else if (strcmp(cmd_b, "kunci") == 0) lock_screen();
    // --- WAKTU ---
    else if (strcmp(cmd_b, "waktu") == 0) {
        rtc_time_t tm = rtc_read_time();
        unsigned char h = (tm.hour + 7) % 24;
        print("Waktu sekarang (WIB): ", 0x0B);
        p_int(h, 0x0F); print(":", 0x0F);
        if (tm.minute < 10) { print("0", 0x0F); }
        p_int(tm.minute, 0x0F); print(":", 0x0F);
        if (tm.second < 10) { print("0", 0x0F); }
        p_int(tm.second, 0x0F); print("\n", 0x0F);
    }
    // --- WARNA ---
    else if (strcmp(cmd_b, "warna") == 0) {
        print("Fitur warna hanya untuk TTY (Text Mode).\n", 0x0E);
    }
    // --- JALANKAN (run .mct app) ---
    else if (strncmp(cmd_b, "jalankan ", 9) == 0) {
        char* fname = cmd_b + 9;
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
                print("[+] User Mode Task Created! (Task ID: ", 0x0A); p_int(res, 0x0A); print(")\n", 0x0A);
                
                extern int term_app_running;
                extern int term_app_task_id;
                term_app_running = 1;
                term_app_task_id = res;
                return; // DO NOT PRINT PROMPT
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
    // --- KILL ---
    else if (strncmp(cmd_b, "kill ", 5) == 0) {
        int tid = atoi(cmd_b + 5);
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
    else if (strcmp(cmd_b, "kill") == 0) {
        print("Usage: kill [PID]  — Terminate a process\n", 0x0E);
    }
    // --- ECHO ---
    else if (strncmp(cmd_b, "echo ", 5) == 0) {
        print(cmd_b + 5, 0x0F);
        print("\n", 0x0F);
    }
    // --- TUNGGU (sleep) ---
    else if (strncmp(cmd_b, "tunggu ", 7) == 0) {
        int ms = atoi(cmd_b + 7);
        if (ms > 0 && ms < 60000) {
            // Must NOT busy-wait on get_ticks(): ex_cmd()'s live entry point is
            // SYS_EXEC_CMD, and int 0x80 is an interrupt gate (IF=0), so IRQ0
            // never fires and the tick count can never reach the target — the
            // box wedges with no way to kill the task. task_sleep() re-enables
            // interrupts and hands us to the scheduler, which is what actually
            // makes time pass here. PIT runs at 1000 Hz, so 1 tick == 1 ms.
            task_sleep(ms);
        }
    }
    // --- NADA (beep frequency) ---
    else if (strncmp(cmd_b, "nada ", 5) == 0) {
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

void ex_cmd() {
    print("\n", 0x0F);
    cmd_b[b_idx] = '\0';
    
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