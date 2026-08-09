// src/sys/shell/builtins/text_tools/cmd_printf_arg.c — the `printf_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_printf_arg(void) {
        // printf FORMAT ARGS...  — %s string, %d decimal, %x hex, %c char,
        // %% literal percent, \n newline, \t tab. Args are space-delimited
        // tokens after the format (quotes stripped).
        char* p = cmd_b + 7;
        char* fmt = next_token(&p);
        if (!fmt) {
            print("printf: usage: printf FORMAT [ARGS...]\n", 0x0E);
        } else {
            extern void write_serial_string(const char*);
            char mirror[256];
            int mi = 0;
            for (int i = 0; fmt[i] && i < 200; i++) {
                char c = fmt[i];
                if (c == '\\' && (fmt[i + 1] == 'n' || fmt[i + 1] == 't')) {
                    c = (fmt[i + 1] == 'n') ? '\n' : '\t';
                    i++;
                } else if (c == '%') {
                    char spec = fmt[++i];
                    if (spec == '%') { c = '%'; }
                    else {
                        char* arg = next_token(&p);
                        if (!arg) arg = "";
                        if (spec == 's') {
                            print(arg, 0x0F);
                            for (int k = 0; arg[k] && mi < 255; k++) mirror[mi++] = arg[k];
                            continue;
                        } else if (spec == 'd') {
                            int v = atoi(arg);
                            p_int(v, 0x0F);
                            // Split into sign + unsigned magnitude so INT_MIN
                            // doesn't overflow (-INT_MIN is UB on i386).
                            uint32_t mag = (v < 0) ? (uint32_t)(-(v + 1)) + 1u : (uint32_t)v;
                            if (v < 0 && mi < 255) mirror[mi++] = '-';
                            char db[12]; int di = 0;
                            do { db[di++] = '0' + (int)(mag % 10u); mag /= 10u; } while (mag);
                            while (di > 0 && mi < 255) mirror[mi++] = db[--di];
                            continue;
                        } else if (spec == 'x') {
                            // Parse as unsigned decimal (strtoul-style): atoi is
                            // signed, so values >= 2^31 would wrap through UB.
                            uint32_t v = 0;
                            for (int k = 0; arg[k] >= '0' && arg[k] <= '9'; k++) {
                                v = v * 10u + (uint32_t)(arg[k] - '0');
                            }
                            print_hex_value(v);
                            int started = 0;
                            for (int sh = 28; sh >= 0 && mi < 255; sh -= 4) {
                                int nib = (v >> sh) & 0xF;
                                if (nib || started) {
                                    started = 1;
                                    mirror[mi++] = (nib < 10) ? ('0' + nib) : ('a' + nib - 10);
                                }
                            }
                            if (!started && mi < 255) mirror[mi++] = '0';
                            continue;
                        } else if (spec == 'c') {
                            c = arg[0];
                        } else {
                            // Unknown spec: echo literally.
                            print("%", 0x0F);
                            c = spec;
                        }
                    }
                }
                p_char(c, 0x0F);
                if (c != '\n' && c != '\t' && mi < 255) mirror[mi++] = c;
                else if (c == '\n' && mi < 255) mirror[mi++] = ' ';
            }
            mirror[mi] = '\0';
            write_serial_string("[SH] printf: ");
            write_serial_string(mirror);
            write_serial_string("\n");
        }
}
