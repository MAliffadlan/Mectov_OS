// src/sys/shell/builtins/sysinfo/cmd_help.c — the `help` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_help(void) {
        print("======================================================================\n", 0x0B);
        print("                ⚡ MECTOV OS v", 0x0F); print(OS_VERSION, 0x0F); print(" - COMMAND CENTER ⚡                \n", 0x0F);
        print("======================================================================\n", 0x0B);
        print(" SYSTEM  : ", 0x0B); print("mfetch, date, color, clear, mem, memstat, kmemstats, uptime, lock, ps, kill\n", 0x0F);
        print(" FILE VFS: ", 0x0B); print("ls, cd, pwd, mkdir, touch, cat, head, tree, rm, rmdir, cp, mv, df\n", 0x0F);
        print(" IDENTITY: ", 0x0B); print("uname [-a], whoami, passwd [OLD] NEW, hostname, env, seq [FIRST] LAST\n", 0x0F);
        print(" EDITOR  : ", 0x0B); print("nano, edit\n", 0x0F);
        print(" SHELL   : ", 0x0B); print("export [NAME=VAL], alias [NAME=VAL], unalias, history, sh\n", 0x0F);
        print(" JOBS    : ", 0x0B); print("cmd & (background), jobs, fg [n], bg [n], kill [%n]\n", 0x0F);
        print(" REDIR   : ", 0x0B); print("cmd > file (truncate), cmd >> file (append), cmd < file (stdin)\n", 0x0F);
        print(" APPS GUI: ", 0x0B); print("flappy, doom, taskmgr, snake, run [app.mct], run [app.mct] &\n", 0x0A);
        print(" NET & HW: ", 0x0B); print("ping [ip], host [domain], fetch [domain], lspci\n", 0x0F);
        print(" UTILS   : ", 0x0B); print("echo [msg], sleep [sec], wc [file], cat -n, cd -, type [cmd], yes [str] &\n", 0x0F);
        print(" TOOLKIT : ", 0x0B); print("printf FMT [args], sort [file], uniq [-c] [file], tee FILE, find [dir] [-name GLOB]\n", 0x0F);
        print(" POWER   : ", 0x0B); print("reboot, shutdown\n", 0x0C);
        print("----------------------------------------------------------------------\n", 0x07);
        print(" SHORTCUT: ", 0x0E); print("Tab=Autocomplete  |  Up/Down=History  |  Pipes: cmd1 | cmd2\n", 0x0F);
        print(" LANG    : ", 0x0E); print("English UI; legacy Indonesian aliases (buat, hapus, ular) still work\n", 0x0F);
        print("======================================================================\n", 0x0B);
}
