// src/sys/shell/builtins/sysinfo/cmd_locktimeout.c — the `locktimeout`
// shell command (v38.51): arm the idle auto-lock.
//
// `locktimeout <seconds>` locks the desktop (same pending_lock flag as the
// `lock` builtin) once no keyboard or mouse input has arrived for that long;
// `locktimeout 0` disables it. Called with no argument, it prints the
// current setting. The check runs at 1 s granularity in the kernel main
// loop (security.c).
#include "../../shell_internal.h"

void cmd_locktimeout(void) {
    extern void security_set_auto_lock(uint32_t);
    extern volatile uint32_t auto_lock_secs;

    char* rest = cmd_b + 11;   // "locktimeout"
    while (*rest == ' ') rest++;

    if (*rest == '\0') {
        print("auto-lock: ", 0x07);
        p_int((int)auto_lock_secs, 0x0F);
        print(auto_lock_secs ? " s without input (0 = off)\n"
                             : " s (0 = off)\n", 0x07);
        return;
    }

    uint32_t secs = 0;
    for (; *rest >= '0' && *rest <= '9'; rest++) {
        secs = secs * 10 + (uint32_t)(*rest - '0');
        if (secs > 86400) { secs = 86400; break; }   // cap at 24 h
    }

    security_set_auto_lock(secs);
    if (secs) {
        print("auto-lock armed: lock after ", 0x0A);
        p_int((int)secs, 0x0F);
        print(" s without input\n", 0x07);
    } else {
        print("auto-lock disabled\n", 0x0A);
    }
}
