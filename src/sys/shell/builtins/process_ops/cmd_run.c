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
            // Foreground handoff happens inside the loader's cli window
            // (load_mct_app_fg): pgrp + fg are final before the task can
            // run on any core, so authorization checks (SYS_FB_MAP) never
            // race the terminal handoff.
            int res = load_mct_app_fg(fname, arg, !shell_bg_flag);
            if (res >= 0) {
                // pgrp/fg handoff already happened inside the loader's cli
                // window (see load_mct_app_fg). A foreground app owns the
                // terminal (Ctrl+C/Z target its group); a background app
                // keeps a group != fg, so reading the terminal stops it.
                if (shell_bg_flag) {
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
                    return 1; // DO NOT PRINT PROMPT
                }
            } else {
                print("[-] Failed to execute MCT.\n", 0x0C);
            }
        }
    return 0;
}
