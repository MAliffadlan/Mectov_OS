// src/sys/shell/builtins/process_ops/cmd_run.c — the `run` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

int cmd_run(void) {
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
                    extern void term_app_key_clear(void);
                    // Drop any keys queued for a previous foreground app so
                    // the new app starts with a clean buffer (single-consumer
                    // keyboard, v38.9).
                    term_app_key_clear();
                    term_app_running = 1;
                    term_app_task_id = res;
                    task_set_fg_pgrp(res);   // foreground group owns the terminal
                    return 1; // DO NOT PRINT PROMPT
                }
            } else {
                print("[-] Failed to execute MCT.\n", 0x0C);
            }
        }
    return 0;
}
