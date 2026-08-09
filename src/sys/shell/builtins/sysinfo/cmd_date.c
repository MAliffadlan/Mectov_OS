// src/sys/shell/builtins/sysinfo/cmd_date.c — the `date` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_date(void) {
        rtc_time_t tm = rtc_read_time();
        unsigned char h = (tm.hour + 7) % 24;
        print("Current time (WIB): ", 0x0B);
        p_int(h, 0x0F); print(":", 0x0F);
        if (tm.minute < 10) { print("0", 0x0F); }
        p_int(tm.minute, 0x0F); print(":", 0x0F);
        if (tm.second < 10) { print("0", 0x0F); }
        p_int(tm.second, 0x0F); print("\n", 0x0F);
}
