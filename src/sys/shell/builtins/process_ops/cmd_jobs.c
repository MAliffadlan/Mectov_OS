// src/sys/shell/builtins/process_ops/cmd_jobs.c — the `jobs` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_jobs(void) {
        print_jobs();
        if (job_count == 0) print("No background jobs.\n", 0x07);
}
