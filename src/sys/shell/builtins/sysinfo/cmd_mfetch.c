// src/sys/shell/builtins/sysinfo/cmd_mfetch.c — the `mfetch` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_mfetch(void) {
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
        print("  OS: ", 0x0B); print("Mectov OS v", 0x0F); print(OS_VERSION, 0x0F); print("\n", 0x0F);
        // Row 4: color blocks + Kernel
        print("  ", 0x00);
        print("## ## ## ## ", 0x0E); print("## ## ## ## ", 0x06);
        print("  Kernel: ", 0x0B); print("Mectov ", 0x0F); print(OS_VERSION, 0x0F); print(".0\n", 0x0F);
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
