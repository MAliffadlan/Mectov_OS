// src/sys/shell/job/job.c — background job control.
// `cmd &` forks a copy of the shell's task to run the command (or, for
// `run`, just launches without grabbing the terminal). Jobs are tracked
// here so the shell can list them (jobs), wait on them (fg) or kill them
// (kill %n). Split out of the former monolithic src/sys/shell.c.
#include "../shell_internal.h"

// --- Job control (background processes) ---
// `cmd &` forks a copy of the shell's task to run the command (or, for `run`,
// just launches without grabbing the terminal). Jobs are tracked here so the
// shell can list them (jobs), wait on them (fg) or kill them (kill %n).
shell_job_t jobs[MAX_JOBS];
int job_count = 0;
int shell_bg_flag = 0;   // set while run_cmd_internal() runs a `&` command

int register_job(int tid, const char* cmd) {
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

void print_jobs(void) {
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
int find_job_tid(int num) {
    refresh_jobs();
    int count = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].tid <= 0) continue;
        count++;
        if (count == num) return jobs[i].tid;
    }
    return -1;
}

void bg_child_entry(void) {
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

