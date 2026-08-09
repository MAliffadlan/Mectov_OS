// src/sys/shell/builtins/sysinfo/cmd_uptime.c — the `uptime` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_uptime(void) {
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
