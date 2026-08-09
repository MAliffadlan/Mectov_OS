// src/sys/shell/builtins/app_launch/cmd_doom.c — the `doom` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_doom(void) {
        // DOOM is SILENT by default: SB16 DMA/IRQ activity while DOOM streams
        // audio froze the display on some hosts (guest stays alive, QEMU
        // window stalls). `doom -sound` opts back in to the SB16 module.
        extern int doom_sound_enabled;
        /* '-sound' enables sound, but '-nosound' (substring of '-sound')
         * must keep it off — the old escape hatch stays honored. */
        doom_sound_enabled = (strstr_custom(cmd_b, "-sound") >= 0
                              && strstr_custom(cmd_b, "-nosound") < 0) ? 1 : 0;
        if (!doom_sound_enabled) write_serial_string("[DOOM] sound off (use 'doom -sound' to enable)\n");
        print("Starting DOOM...\n", 0x0C);
        extern void doom_start(void);
        doom_start();
        // Self-cleared inside doom_start(); nothing to reset here.
}
