// ============================================================
// shell_core.c — Mectov OS Shell: dispatch, input handling, env/alias
// ============================================================
// Core of the shell, split out of the former monolithic shell.c:
//   run_cmd_internal() — command dispatcher (each command lives in
//                        builtins/<group>/cmd_<name>.c)
//   ex_cmd()          — entry point: background &, redirection, pipes
//   history / tab completion / prompt / env & alias tables / helpers
// Cross-module state is declared in shell_internal.h.

// ============================================================
// shell.c — Mectov OS Shell with Tab Completion & History
// ============================================================

#include "shell_internal.h"

// ---- Reentrant irqsave lock (kernel locking audit v38.4) ----
//
// The shell's global state (cmd_b, env, aliases, history, stdin buffer) is
// shared by every terminal: two terminals on two cores can run ex_cmd()
// concurrently (SYS_EXEC_CMD), so shell command execution must serialize.
// Reentrant because run_script() -> ex_cmd() nests inside SYS_EXEC_CMD's own
// ex_cmd(). Ordering: task_lock > shell_lock > fd_lock > vfs_lock > ata_lock.
//
// shell_lock_release_for_block()/shell_lock_reacquire() drop the lock across
// blocking operations (sleep, waitpid): a shell task killed while parked must
// not strand the lock and hang every other terminal. Reacquire restores the
// saved depth so nested holders' release accounting stays balanced.
spinlock_t shell_lock = SPINLOCK_INIT;
static uint32_t shell_eflags;
static int shell_lock_owner = -1;
static int shell_lock_depth = 0;
static int shell_saved_depth = 0;
// Set by shell_lock_release_for_block() on the owning task, cleared by
// shell_lock_reacquire(). A forked BACKGROUND child (task_fork_kernel copies
// the shell's task) must not release/re-acquire the parent's lock — it is a
// different task that does not own it, and dying with it re-acquired would
// strand the lock and hang every terminal on the next command.
static int shell_block_released = 0;

void shell_lock_acquire(void) {
    int tid = get_current_task();
    int key = (task_get_cid() << 16) | (tid & 0xFFFF);
    if (shell_lock_owner == key) { shell_lock_depth++; return; }
    shell_eflags = spin_lock_irqsave(&shell_lock);
    shell_lock_owner = key;
    shell_lock_depth = 1;
}

void shell_lock_release(void) {
    if (shell_lock_depth > 1) { shell_lock_depth--; return; }
    shell_lock_depth = 0;
    shell_lock_owner = -1;
    spin_unlock_irqrestore(&shell_lock, shell_eflags);
}

void shell_lock_release_for_block(void) {
    if (shell_lock_owner < 0) return;
    // Only the owning task may drop the lock. A forked background child is a
    // separate task sharing these globals; releasing the parent's lock here
    // and re-acquiring it below would strand the lock when the child exits.
    int tid = get_current_task();
    int key = (task_get_cid() << 16) | (tid & 0xFFFF);
    if (shell_lock_owner != key) return;
    shell_saved_depth = shell_lock_depth;
    shell_lock_depth = 0;
    shell_lock_owner = -1;
    shell_block_released = 1;
    spin_unlock_irqrestore(&shell_lock, shell_eflags);
}

void shell_lock_reacquire(void) {
    if (!shell_block_released) return;
    shell_block_released = 0;
    int tid = get_current_task();
    int key = (task_get_cid() << 16) | (tid & 0xFFFF);
    shell_eflags = spin_lock_irqsave(&shell_lock);
    shell_lock_owner = key;
    shell_lock_depth = (shell_saved_depth > 0) ? shell_saved_depth : 1;
    shell_saved_depth = 0;
}

// OS_VERSION lives in utils.h (single source of truth shared with /proc).

// --- Command buffer & state ---
char cmd_b[CMD_BUF_SIZE]; int b_idx = 0;

// File redirection stdin buffer: filled by '<' redirection, consumed by
// cat/grep when they would otherwise read the pipe buffer.
char shell_stdin_buf[4096];
int shell_stdin_len = 0;
char hist_b[256];
int is_script = 0;

env_var_t env_vars[MAX_ENV_VARS];
int env_var_count = 0;

// Shared one-shot init guard for the env/alias tables (see ex_cmd and
// shell_print_prompt). Module-level so both init sites agree and never reset
// user-added variables.
static int env_alias_initialized = 0;

alias_t aliases[MAX_ALIASES];
int alias_count = 0;

// Previous directory for `cd -` (OLDPWD). Only updated on a successful cd
// to an explicit directory, so `cd -` always lands somewhere valid.
char shell_oldpwd[MAX_PATH];

// Print n right-aligned in a field of `width` columns (space-padded), POSIX
// `wc`/`cat -n` style, then a trailing space. Numbers beyond the field width
// just print unpadded rather than overflowing.
void print_num_field(int n, int width) {
    int digits = 1, t = n;
    while (t >= 10) { t /= 10; digits++; }
    for (int s = 0; s < width - digits; s++) print(" ", 0x0F);
    p_int(n, 0x0F);
    print(" ", 0x0F);
}

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
        if (in[i] == '\\' && in[i + 1] == '$') {
            // Escaped dollar: `\$` stays a literal `$` (no expansion). Lets
            // scripts written with echo keep a literal $VAR for a later pass.
            out[o++] = '$';
            i += 2;
        } else if (in[i] == '$') {
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


// ============================================================
// Command list (tab completion) + history + prompt + tab completion
// ============================================================
const char* cmd_list[] = {
    "help","clear","mfetch","mem","memstat","kmemstats","uptime","dmesg","vfsinfo",
    "ls","cd","pwd","mkdir","touch","cat","head","tree","rm","rmdir","cp","mv","df",
    "edit","nano",
    "sh","source","export","alias","unalias","history","ps","kill",
    "jobs","fg","bg",
    "echo","beep","tone","sleep","date","color","lock",
    "uname","whoami","passwd","hostname","env","seq","wc","type","yes",
    "printf","sort","uniq","tee","find",
    "run","snake","taskmgr","flappy","doom","lspci","ipconfig","man",
    "ping","host","fetch","grep",
    "shutdown","reboot", NULL
};

// --- History circular buffer ---
char history[HIST_MAX][CMD_BUF_SIZE];
int hist_count = 0;
int hist_pos = -1;

int hist_next_slot = 0; // next slot to overwrite (oldest)

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

int strstr_custom(const char* haystack, const char* needle) {
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

void sanitize_path(char* path) {
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
// NET_WAIT_MAX_SPINS is defined in shell_internal.h (shared with builtins).
int net_wait_for(volatile int* flag, uint32_t timeout_ms) {
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

// ---- Toolkit round 3 helpers (sort/uniq lines, find wildcard, printf hex) ----
// Line tables are stack locals (not statics): a forked background builtin
// (`sh x.sh &` whose script runs sort/uniq) executes run_cmd_internal on
// another core, so shared statics could race.

// Split src[0..src_len) into up to max_lines lines (pointers into src).
// A line ends at '\n' or '\r' (CRLF counts as one terminator); an empty
// line between two terminators is a real line (POSIX sort/uniq behavior);
// a trailing line without a terminator counts too. Returns the line count.
int split_lines(const char* src, int src_len, sh_line_t* lines, int max_lines) {
    int n = 0, start = 0, i = 0;
    while (i < src_len && n < max_lines) {
        if (src[i] == '\n' || src[i] == '\r') {
            // i >= start: an empty line between separators is kept.
            lines[n].off = start;
            lines[n].len = i - start;
            n++;
            if (src[i] == '\r' && i + 1 < src_len && src[i + 1] == '\n') i++; // CRLF = one sep
            i++;
            start = i;
        } else {
            i++;
        }
    }
    if (start < src_len && n < max_lines) {
        lines[n].off = start; lines[n].len = src_len - start; n++;
    }
    return n;
}

// Compare two lines (via src) like strcmp but bounded by each line's length.
int line_cmp(const char* src, const sh_line_t* a, const sh_line_t* b) {
    int n = (a->len < b->len) ? a->len : b->len;
    int r = 0;
    for (int i = 0; i < n; i++) {
        r = (unsigned char)src[a->off + i] - (unsigned char)src[b->off + i];
        if (r) return r;
    }
    return a->len - b->len;
}

// Glob match: '*' matches any run (incl. empty), '?' any single char.
// Greedy two-pointer scan (linear in n*m): the recursive backtracking
// version is exponential on patterns like "*a*a*a*...*b" against a long
// run of 'a's, and this runs in kernel context.
int wild_match(const char* pat, const char* str) {
    const char* star_pat = NULL;
    const char* star_str = NULL;
    while (*str) {
        if (*pat == '*') {
            star_pat = pat++;
            star_str = str;
        } else if (*pat == '?' || *pat == *str) {
            pat++;
            str++;
        } else if (star_pat) {
            // Backtrack: let the last '*' absorb one more char.
            pat = star_pat + 1;
            str = ++star_str;
        } else {
            return 0;
        }
    }
    while (*pat == '*') pat++;
    return *pat == '\0';
}

// Print v as lowercase hex without leading zeros (printf %x semantics).
void print_hex_value(uint32_t v) {
    int started = 0;
    for (int sh = 28; sh >= 0; sh -= 4) {
        int nib = (v >> sh) & 0xF;
        if (nib || started) {
            started = 1;
            p_char((nib < 10) ? ('0' + nib) : ('a' + nib - 10), 0x0F);
        }
    }
    if (!started) p_char('0', 0x0F);
}

// Grab the next space-delimited token from *pp (modifies the string in
// place), skipping spaces and stripping one pair of surrounding quotes.
// Returns NULL at end of input.
char* next_token(char** pp) {
    char* p = *pp;
    while (*p == ' ') p++;
    if (!*p) return NULL;
    char* tok = p;
    while (*p && *p != ' ') p++;
    if (*p) { *p = '\0'; p++; }
    *pp = p;
    int len = 0;
    while (tok[len]) len++;
    if (len >= 2 && ((tok[0] == '"' && tok[len - 1] == '"') ||
                     (tok[0] == '\'' && tok[len - 1] == '\''))) {
        tok[len - 1] = '\0';
        return tok + 1;
    }
    return tok;
}

// ============================================================
// Main command execution (dispatcher)
// ============================================================
// Every command's implementation lives in builtins/<group>/cmd_<name>.c;
// this dispatcher only matches the command string and forwards. The
// if/else chain, its order and its conditions are unchanged from the
// original monolithic shell.c, so matching precedence is identical.
void run_cmd_internal() {
// --- HELP ---
if (strcmp(cmd_b, "help") == 0) { cmd_help(); }
// --- CLEAR ---
else if (strcmp(cmd_b, "clear") == 0) { cmd_clear(); }
// --- MFETCH (ToaruOS sysinfo style) ---
else if (strcmp(cmd_b, "mfetch") == 0) { cmd_mfetch(); }
// --- MEM / KMEMSTATS ---
else if (strcmp(cmd_b, "mem") == 0) { cmd_mem(); }
else if (strcmp(cmd_b, "kmemstats") == 0) { cmd_kmemstats(); }
// --- MEMSTAT ---
else if (strcmp(cmd_b, "memstat") == 0) { cmd_memstat(); }
// --- UPTIME ---
else if (strcmp(cmd_b, "uptime") == 0) { cmd_uptime(); }
// --- DMESG ---
else if (strcmp(cmd_b, "dmesg") == 0) { cmd_dmesg(); }
// --- VFS INFO ---
else if (strcmp(cmd_b, "vfsinfo") == 0) { cmd_vfsinfo(); }
// --- CD ---
else if (strncmp(cmd_b, "cd ", 3) == 0 || strcmp(cmd_b, "cd") == 0) { cmd_cd(); }
// --- PWD ---
else if (strcmp(cmd_b, "pwd") == 0) { cmd_pwd(); }
// --- LS (new VFS version) ---
else if (strcmp(cmd_b, "ls") == 0) { cmd_ls(); }
else if (strncmp(cmd_b, "ls ", 3) == 0) { cmd_ls_arg(); }
// --- TREE ---
else if (strcmp(cmd_b, "tree") == 0) { cmd_tree(); }
else if (strncmp(cmd_b, "tree ", 5) == 0) { cmd_tree_arg(); }
// --- MKDIR ---
else if (strncmp(cmd_b, "mkdir ", 6) == 0) { cmd_mkdir_arg(); }
// --- TOUCH (create empty file) ---
else if (strncmp(cmd_b, "touch ", 6) == 0) { cmd_touch_arg(); }
// --- CAT (read file) ---
else if (strncmp(cmd_b, "cat ", 4) == 0 || strcmp(cmd_b, "cat") == 0) { cmd_cat(); }
// --- GREP ---
else if (strncmp(cmd_b, "grep ", 5) == 0) { cmd_grep_arg(); }
// --- UNAME (OS identity, Linux-style) ---
else if (strcmp(cmd_b, "uname") == 0 || strncmp(cmd_b, "uname ", 6) == 0) { cmd_uname(); }
// --- WHOAMI ---
else if (strcmp(cmd_b, "whoami") == 0) { cmd_whoami(); }
// --- PASSWD (change the login password in /etc/passwd) ---
// The terminal sends whole commands (no echo-off prompting), so POSIX
// style is two args: `passwd <current> <new>`. Verification goes through
// sys_get_password, so the hardcoded default is accepted until the file
// exists — first change bootstraps it.
else if (strncmp(cmd_b, "passwd", 6) == 0 && (cmd_b[6] == ' ' || cmd_b[6] == '\0')) { cmd_passwd(); }
// --- HOSTNAME ---
else if (strcmp(cmd_b, "hostname") == 0) { cmd_hostname(); }
// --- ENV (list exported environment variables) ---
else if (strcmp(cmd_b, "env") == 0) { cmd_env(); }
// --- SEQ (print a number sequence) ---
else if (strncmp(cmd_b, "seq ", 4) == 0 || strcmp(cmd_b, "seq") == 0) { cmd_seq(); }
// --- HEAD (print the first N lines of a file or stdin) ---
else if (strncmp(cmd_b, "head ", 5) == 0 || strcmp(cmd_b, "head") == 0) { cmd_head(); }
// --- WC (word count: lines, words, bytes) ---
else if (strncmp(cmd_b, "wc ", 3) == 0 || strcmp(cmd_b, "wc") == 0) { cmd_wc(); }
// --- TYPE (describe a command: alias / builtin / app / not found) ---
else if (strncmp(cmd_b, "type ", 5) == 0) { cmd_type_arg(); }
// --- RM (delete) ---
else if (strncmp(cmd_b, "rm ", 3) == 0) { cmd_rm_arg(); }
// --- RMDIR (remove empty directory) ---
else if (strncmp(cmd_b, "rmdir ", 6) == 0) { cmd_rmdir_arg(); }
// --- CP (copy file) ---
else if (strncmp(cmd_b, "cp ", 3) == 0) { cmd_cp_arg(); }
// --- MV (move / rename) ---
else if (strncmp(cmd_b, "mv ", 3) == 0) { cmd_mv_arg(); }
// --- DF (disk free) ---
else if (strcmp(cmd_b, "df") == 0) { cmd_df(); }
// --- SHUTDOWN / REBOOT ---
// Note: the Indonesian names below (and in the other legacy handlers) are
// fallbacks — in the normal flow expand_alias() already mapped them to the
// English commands. They stay reachable after `unalias <id-name>` or when a
// script runs before the alias table is initialized, so don't delete them.
else if (strcmp(cmd_b, "matikan") == 0 || strcmp(cmd_b, "shutdown") == 0) cmd_shutdown();
else if (strcmp(cmd_b, "mulaiulang") == 0 || strcmp(cmd_b, "reboot") == 0) cmd_reboot();
// --- LSPCI ---
else if (strcmp(cmd_b, "lspci") == 0) { cmd_lspci(); }
// --- IPCONFIG (runtime network config: DHCP or static) ---
else if (strcmp(cmd_b, "ipconfig") == 0) { cmd_ipconfig(); }
// --- SNAKE (ular) ---
else if (strcmp(cmd_b, "snake") == 0 || strcmp(cmd_b, "ular") == 0) cmd_snake();
// --- FLAPPY ---
else if (strcmp(cmd_b, "flappy") == 0) cmd_flappy();
// --- DOOM ---
else if (strcmp(cmd_b, "doom") == 0 || strncmp(cmd_b, "doom ", 5) == 0) { cmd_doom(); }
// --- TASKMGR ---
else if (strcmp(cmd_b, "taskmgr") == 0) cmd_taskmgr();
// --- LOCK (kunci) ---
else if (strcmp(cmd_b, "lock") == 0 || strcmp(cmd_b, "kunci") == 0) cmd_lock();
// --- DATE (waktu) ---
else if (strcmp(cmd_b, "date") == 0 || strcmp(cmd_b, "waktu") == 0) { cmd_date(); }
// --- COLOR (warna) ---
else if (strcmp(cmd_b, "color") == 0 || strcmp(cmd_b, "warna") == 0) { cmd_color(); }
// --- RUN / JALANKAN (run .mct app) ---
else if (strncmp(cmd_b, "run ", 4) == 0 || strncmp(cmd_b, "jalankan ", 9) == 0) {
        if (cmd_run()) return;  // foreground app: skip b_idx reset (was: return;)
    }
// --- Legacy: BUAT ---
else if (strncmp(cmd_b, "buat ", 5) == 0) { cmd_buat_arg(); }
// --- BACA ---
else if (strncmp(cmd_b, "baca ", 5) == 0) { cmd_baca_arg(); }
// --- Legacy: EDIT / TULIS / NANO ---
else if (strncmp(cmd_b, "edit ", 5) == 0 || strncmp(cmd_b, "tulis ", 6) == 0 || strncmp(cmd_b, "nano ", 5) == 0) { cmd_edit(); }
// --- Legacy: HAPUS ---
else if (strncmp(cmd_b, "hapus ", 6) == 0) { cmd_hapus_arg(); }
// --- PING ---
else if (strncmp(cmd_b, "ping ", 5) == 0) { cmd_ping_arg(); }
// --- HOST ---
else if (strncmp(cmd_b, "host ", 5) == 0) { cmd_host_arg(); }
// --- FETCH (HTTP GET over TCP, Ring 0 demo of the connection stack) ---
else if (strncmp(cmd_b, "fetch ", 6) == 0) { cmd_fetch_arg(); }
// --- SH / SOURCE ---
else if (strncmp(cmd_b, "sh ", 3) == 0 || strncmp(cmd_b, "source ", 7) == 0) { cmd_sh(); }
// --- EXPORT ---
else if (strncmp(cmd_b, "export ", 7) == 0 || strcmp(cmd_b, "export") == 0) { cmd_export(); }
// --- ALIAS ---
else if (strncmp(cmd_b, "alias ", 6) == 0 || strcmp(cmd_b, "alias") == 0) { cmd_alias(); }
// --- UNALIAS ---
else if (strncmp(cmd_b, "unalias ", 8) == 0) { cmd_unalias_arg(); }
// --- HISTORY ---
else if (strcmp(cmd_b, "history") == 0) { cmd_history(); }
// --- PS (Process Status) ---
else if (strcmp(cmd_b, "ps") == 0) { cmd_ps(); }
// --- JOBS ---
else if (strcmp(cmd_b, "jobs") == 0) { cmd_jobs(); }
// --- FG: bring a background job to the foreground (wait for it) ---
else if (strncmp(cmd_b, "fg", 2) == 0 && (cmd_b[2] == '\0' || cmd_b[2] == ' ')) { cmd_fg(); }
// --- BG: resume a stopped (Ctrl+Z) job in the background ---
else if (strncmp(cmd_b, "bg", 2) == 0 && (cmd_b[2] == '\0' || cmd_b[2] == ' ')) { cmd_bg(); }
// --- KILL ---
else if (strncmp(cmd_b, "kill ", 5) == 0) { cmd_kill_arg(); }
else if (strcmp(cmd_b, "kill") == 0) { cmd_kill(); }
// --- ECHO ---
else if (strncmp(cmd_b, "echo ", 5) == 0) { cmd_echo_arg(); }
// --- SLEEP (tunggu) ---
else if (strncmp(cmd_b, "sleep ", 6) == 0 || strncmp(cmd_b, "tunggu ", 7) == 0) { cmd_sleep(); }
// --- YES (repeat a string forever, `yes` / `yes hello`) ---
else if (strncmp(cmd_b, "yes", 3) == 0 && (cmd_b[3] == '\0' || cmd_b[3] == ' ')) { cmd_yes(); }
// --- TONE (nada, beep frequency) ---
else if (strncmp(cmd_b, "tone ", 5) == 0 || strncmp(cmd_b, "nada ", 5) == 0) { cmd_tone(); }
// --- BEEP ---
else if (strcmp(cmd_b, "beep") == 0) { cmd_beep(); }
// --- MAN ---
else if (strncmp(cmd_b, "man ", 4) == 0) { cmd_man_arg(); }
// --- PRINTF (formatted output) ---
else if (strncmp(cmd_b, "printf ", 7) == 0) { cmd_printf_arg(); }
// --- SORT (sort lines from a file or stdin) ---
else if (strncmp(cmd_b, "sort", 4) == 0 && (cmd_b[4] == '\0' || cmd_b[4] == ' ')) { cmd_sort(); }
// --- UNIQ (drop consecutive duplicate lines; -c prefixes counts) ---
else if (strncmp(cmd_b, "uniq", 4) == 0 && (cmd_b[4] == '\0' || cmd_b[4] == ' ')) { cmd_uniq(); }
// --- TEE (write stdin to a file AND stdout) ---
else if (strncmp(cmd_b, "tee ", 4) == 0) { cmd_tee_arg(); }
// --- FIND (walk a directory tree, print full paths, -name filter) ---
else if (strncmp(cmd_b, "find", 4) == 0 && (cmd_b[4] == '\0' || cmd_b[4] == ' ')) { cmd_find(); }
// --- UNKNOWN ---
else if (cmd_b[0] != '\0') {
        print("Command not found: ", 0x0C);
        print(cmd_b, 0x0C);
        print("\n", 0x0C);
}

    b_idx = 0;
}

// ============================================================
// ex_cmd — command entry point
// ============================================================
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
                                // Drop shell_lock while parked in waitpid: a
                                // killed shell must not strand the lock.
                                shell_lock_release_for_block();
                                task_waitpid(child, &st, 0);
                                shell_lock_reacquire();
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
                    // ex_cmd() can be re-entered from run_script() (one nested
                    // ex_cmd per script line), so this frame's stack budget
                    // matters: a 4KB local here plus run_script()'s ~6KB plus
                    // the outer ex_cmd overflowed the 16KB kernel stack on
                    // `sh script > file` (clean PANIC since the guard pages).
                    // The buffer is heap-allocated instead — same behavior.
                    char* existing = (char*)kmalloc(4096);
                    if (!existing) {
                        print("sh: out of memory appending\n", 0x0C);
                    } else {
                        int esz = vfs_read_file(redir_file, existing, 4095);
                        if (esz < 0) esz = 0;
                        existing[esz] = '\0';
                        if (esz + pipe_buf_len < 4096) {
                            for (int i = 0; i < pipe_buf_len; i++) existing[esz + i] = pipe_buffer[i];
                            existing[esz + pipe_buf_len] = '\0';
                            vfs_write_file(redir_file, existing, esz + pipe_buf_len);
                            written = esz + pipe_buf_len;
                        }
                        kfree(existing);
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
                            // Drop shell_lock across the waits (see above).
                            shell_lock_release_for_block();
                            task_waitpid(cl, &st, 0);
                            task_waitpid(cr, &st, 0);
                            shell_lock_reacquire();
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
