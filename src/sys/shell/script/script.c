// src/sys/shell/script/script.c — shell script interpreter (for/while).
// Split out of the former monolithic src/sys/shell.c.
#include "../shell_internal.h"

// ============================================================
// Script interpreter: for/while loops with $VAR expansion
// ============================================================
// Scripts are parsed once into NUL-terminated line offsets (blank and comment
// lines are dropped), then walked by a small interpreter that supports
// POSIX-ish control keywords:
//     for VAR in w1 w2 ...; do   (or `do` on its own line)
//     while true; do             (or false — the body is skipped)
//     done
//     break                      (leaves the innermost loop)
// Nesting is capped at SCRIPT_MAX_LOOP_DEPTH and total iterations at
// SCRIPT_MAX_ITERS so a script can never wedge the shell. The loop variable
// is stored as a regular env var, so `$VAR` expansion inside the body uses
// the same code path as every other command. All interpreter state lives on
// the stack of run_script() (NOT static globals) so a background job running
// `sh x.sh &` can never clobber a foreground script's buffers.
#define SCRIPT_MAX_LINES 128
#define SCRIPT_MAX_LOOP_DEPTH 4
#define SCRIPT_MAX_ITERS 4096

typedef struct {
    char var[ENV_NAME_LEN];      // for: loop variable name
    char items[16][40];          // for: iteration values (max 16)
    int  nitems;
    int  idx;                    // for: next item to assign
    int  is_while;               // 1 = while loop
    int  while_true;             // for while: condition value
    int  inline_do;              // '; do' was on the same line
    int  body_start;             // line index of first body line (-1 = pending 'do')
    int  done_line;              // line index of the matching 'done'
} script_loop_t;

// Trim leading spaces and trailing spaces/CR in place; returns the new start.
static char* script_trim(char* s) {
    while (*s == ' ') s++;
    int len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\r')) s[--len] = '\0';
    return s;
}

// Parse a "for VAR in w1 w2 ... [; do]" line into L. Returns 1 on success.
static int script_parse_for(char* t, script_loop_t* L) {
    char* p = t + 4; // skip "for "
    int v = 0;
    while (*p && *p != ' ' && *p != ';' && v < (int)ENV_NAME_LEN - 1) L->var[v++] = *p++;
    L->var[v] = '\0';
    while (*p == ' ') p++;
    if (strncmp(p, "in", 2) != 0) return 0;
    p += 2;
    L->nitems = 0;
    L->inline_do = 0;
    while (*p && L->nitems < 16) {
        while (*p == ' ') p++;
        if (*p == ';') { p++; break; }
        if (strncmp(p, "do", 2) == 0 && (p[2] == ' ' || p[2] == '\0')) break;
        if (!*p) break;
        int w = 0;
        while (*p && *p != ' ' && *p != ';' && w < 39) L->items[L->nitems][w++] = *p++;
        L->items[L->nitems][w] = '\0';
        L->nitems++;
    }
    while (*p == ' ') p++;
    if (*p == ';') p++;
    while (*p == ' ') p++;
    if (strncmp(p, "do", 2) == 0) L->inline_do = 1;
    return 1;
}

// Does the (trimmed) line contain '; do'?
static int script_inline_do(char* t) {
    const char* semi = 0;
    for (const char* p = t; *p; p++) if (*p == ';') semi = p;
    if (!semi) return 0;
    const char* q = semi + 1;
    while (*q == ' ') q++;
    return strncmp(q, "do", 2) == 0;
}

// Line index of the 'done' matching a loop opened at line `start`, or -1.
// The opener itself counts as depth 1, so the first 'done' at depth <= 1 is
// the one that closes `start`'s loop; deeper 'done's close inner loops.
static int script_find_done(int start, char* script_buf, uint16_t* script_off,
                             int script_lines) {
    int depth = 0;
    for (int i = start; i < script_lines; i++) {
        char* t = script_trim(script_buf + script_off[i]);
        if (strncmp(t, "for ", 4) == 0 || strncmp(t, "while ", 6) == 0) depth++;
        else if (strcmp(t, "done") == 0) {
            if (depth <= 1) return i;
            depth--;
        }
    }
    return -1;
}

// Set an env var (the loop variable path), reusing the export table.
static void shell_set_env(const char* name, const char* value) {
    for (int i = 0; i < env_var_count; i++) {
        if (strcmp(env_vars[i].name, name) == 0) {
            strncpy(env_vars[i].value, value, ENV_VAL_LEN - 1);
            env_vars[i].value[ENV_VAL_LEN - 1] = '\0';
            return;
        }
    }
    if (env_var_count < MAX_ENV_VARS) {
        strncpy(env_vars[env_var_count].name, name, ENV_NAME_LEN - 1);
        env_vars[env_var_count].name[ENV_NAME_LEN - 1] = '\0';
        strncpy(env_vars[env_var_count].value, value, ENV_VAL_LEN - 1);
        env_vars[env_var_count].value[ENV_VAL_LEN - 1] = '\0';
        env_var_count++;
    }
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

    // All interpreter state is stack-local: a background `sh x.sh &` runs in
    // a forked task with its own kernel stack, so it must not share buffers
    // with a foreground script that may be mid-flight.
    char script_buf[2048];
    uint16_t script_off[SCRIPT_MAX_LINES];
    int script_lines = 0;
    script_loop_t loop_stack[SCRIPT_MAX_LOOP_DEPTH];
    int loop_depth = 0;
    int script_iters = 0;

    int sz = vfs_read_file(f, script_buf, (int)sizeof(script_buf) - 1);
    if (sz < 0) {
        print("sh: ", 0x0C);
        print(f, 0x0C);
        print(": No such file or directory\n", 0x0C);
        is_script = 0;
        return;
    }
    script_buf[sz] = '\0';

    // Parse the script into NUL-terminated line offsets, dropping blank and
    // comment lines so the interpreter only ever sees real statements.
    {
        int i = 0;
        while (script_buf[i] && script_lines < SCRIPT_MAX_LINES) {
            int start = i;
            while (script_buf[i] && script_buf[i] != '\n') i++;
            if (script_buf[i] == '\n') { script_buf[i] = '\0'; i++; }
            char* t = script_trim(script_buf + start);
            if (t[0] != '\0' && t[0] != '#') {
                script_off[script_lines++] = (uint16_t)(t - script_buf);
            }
        }
    }

    int ip = 0;
    int aborted = 0;
    while (ip < script_lines && !aborted) {
        char* t = script_trim(script_buf + script_off[ip]);

        // --- for VAR in w1 w2 ... [; do] ---
        if (strncmp(t, "for ", 4) == 0) {
            script_loop_t L;
            memset(&L, 0, sizeof(L));
            if (!script_parse_for(t, &L)) {
                print("sh: bad for line: ", 0x0C); print(t, 0x0C); print("\n", 0x0C);
                ip++;
                continue;
            }
            L.done_line = script_find_done(ip, script_buf, script_off, script_lines);
            if (L.done_line < 0) {
                print("sh: missing 'done'\n", 0x0C);
                aborted = 1;
                break;
            }
            if (loop_depth >= SCRIPT_MAX_LOOP_DEPTH) {
                print("sh: loop nesting too deep\n", 0x0C);
                aborted = 1;
                break;
            }
            L.idx = 1;
            L.body_start = L.inline_do ? ip + 1 : -1; // -1 = wait for a 'do' line
            if (L.nitems == 0) {
                ip = L.done_line + 1; // empty list: skip the body
                continue;
            }
            loop_stack[loop_depth++] = L;
            shell_set_env(L.var, L.items[0]);
            ip++;
            continue;
        }

        // --- while true|false [; do] ---
        if (strncmp(t, "while ", 6) == 0) {
            const char* c = t + 6;
            while (*c == ' ') c++;
            // The condition is the first word — `while true; do` and the
            // two-line `while true` form both land here with c = "true...".
            int cond = -1;
            if (strncmp(c, "true", 4) == 0 &&
                (c[4] == ';' || c[4] == ' ' || c[4] == '\0')) cond = 1;
            else if (strncmp(c, "false", 5) == 0 &&
                     (c[5] == ';' || c[5] == ' ' || c[5] == '\0')) cond = 0;
            if (cond < 0) {
                print("sh: while needs 'true' or 'false'\n", 0x0C);
                ip++;
                continue;
            }
            int dl = script_find_done(ip, script_buf, script_off, script_lines);
            if (dl < 0) {
                print("sh: missing 'done'\n", 0x0C);
                aborted = 1;
                break;
            }
            if (!cond) {
                ip = dl + 1; // while false: skip the body entirely
                continue;
            }
            if (loop_depth >= SCRIPT_MAX_LOOP_DEPTH) {
                print("sh: loop nesting too deep\n", 0x0C);
                aborted = 1;
                break;
            }
            script_loop_t L;
            memset(&L, 0, sizeof(L));
            L.is_while = 1;
            L.while_true = 1;
            L.done_line = dl;
            L.inline_do = script_inline_do(t);
            L.body_start = L.inline_do ? ip + 1 : -1;
            loop_stack[loop_depth++] = L;
            ip++;
            continue;
        }

        // --- do (two-line form) ---
        if (strcmp(t, "do") == 0) {
            if (loop_depth > 0 && loop_stack[loop_depth - 1].body_start < 0) {
                loop_stack[loop_depth - 1].body_start = ip + 1;
                ip++;
                continue;
            }
            print("sh: unexpected 'do'\n", 0x0C);
            ip++;
            continue;
        }

        // --- done ---
        if (strcmp(t, "done") == 0) {
            if (loop_depth == 0) {
                print("sh: unexpected 'done'\n", 0x0C);
                ip++;
                continue;
            }
            script_loop_t* L = &loop_stack[loop_depth - 1];
            if (L->body_start < 0) L->body_start = ip + 1; // safety: no 'do' seen
            int redo = L->is_while ? L->while_true : (L->idx < L->nitems);
            if (redo) {
                if (++script_iters > SCRIPT_MAX_ITERS) {
                    print("sh: loop iteration limit exceeded\n", 0x0C);
                    aborted = 1;
                    break;
                }
                if (!L->is_while) {
                    shell_set_env(L->var, L->items[L->idx]);
                    L->idx++;
                }
                ip = L->body_start;
                continue;
            }
            loop_depth--;
            ip++;
            continue;
        }

        // --- break ---
        if (strcmp(t, "break") == 0) {
            if (loop_depth == 0) {
                print("sh: break outside loop\n", 0x0C);
                ip++;
                continue;
            }
            ip = loop_stack[loop_depth - 1].done_line + 1;
            loop_depth--;
            continue;
        }

        // --- regular command ---
        strncpy(cmd_b, t, CMD_BUF_SIZE - 1);
        cmd_b[CMD_BUF_SIZE - 1] = '\0';
        b_idx = strlen(cmd_b);
        print("> ", 0x0A);
        print(cmd_b, 0x0F);
        ex_cmd();
        ip++;
    }
    is_script = 0;
}

// Kernel entry point for a background command: reached by iret from the
// forked child's patched frame (never returns to user mode). Reads its own
// command from its launch_arg (written before the child could run), executes
// it, then exits with status 0.
