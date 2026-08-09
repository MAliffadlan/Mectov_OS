// src/sys/shell/builtins/sysinfo/cmd_man_arg.c — the `man_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_man_arg(void) {
        char* topic = cmd_b + 4;
        if (strcmp(topic, "cd") == 0) {
            print("cd [dir]  — Change directory\n", 0x0B);
            print("  cd /   = go to root\n", 0x0F);
            print("  cd ..  = go up one level\n", 0x0F);
            print("  cd home/user = relative path\n", 0x0F);
        } else if (strcmp(topic, "ls") == 0) {
            print("ls [dir]  — List directory contents\n", 0x0B);
        } else if (strcmp(topic, "mkdir") == 0) {
            print("mkdir [path] — Create directory\n", 0x0B);
        } else if (strcmp(topic, "rm") == 0) {
            print("rm [path] — Remove file or directory\n", 0x0B);
        } else if (strcmp(topic, "touch") == 0) {
            print("touch [path] — Create empty file\n", 0x0B);
        } else if (strcmp(topic, "cat") == 0) {
            print("cat [path] — Display file contents\n", 0x0B);
        } else if (strcmp(topic, "cp") == 0) {
            print("cp [src] [dst] — Copy a file (VFS or /ext2)\n", 0x0B);
        } else if (strcmp(topic, "mv") == 0) {
            print("mv [src] [dst] — Move/rename a file\n", 0x0B);
        } else if (strcmp(topic, "rmdir") == 0) {
            print("rmdir [path] — Remove an empty directory\n", 0x0B);
        } else if (strcmp(topic, "df") == 0) {
            print("df — Show disk usage for mectovfs and /ext2\n", 0x0B);
        } else if (strcmp(topic, "head") == 0) {
            print("head [-n N] [file] — Print the first N lines (default 10)\n", 0x0B);
        } else if (strcmp(topic, "seq") == 0) {
            print("seq [FIRST] LAST — Print a sequence of numbers\n", 0x0B);
        } else if (strcmp(topic, "uname") == 0) {
            print("uname [-a] — Print OS name (or full kernel banner)\n", 0x0B);
        } else if (strcmp(topic, "whoami") == 0) {
            print("whoami — Print the current user (root)\n", 0x0B);
        } else if (strcmp(topic, "passwd") == 0) {
            print("passwd [current] [new] — Change the login password (stored in /etc/passwd)\n", 0x0B);
        } else if (strcmp(topic, "hostname") == 0) {
            print("hostname — Print the machine hostname\n", 0x0B);
        } else if (strcmp(topic, "env") == 0) {
            print("env — List all exported environment variables\n", 0x0B);
        } else if (strcmp(topic, "printf") == 0) {
            print("printf FORMAT [ARGS...] — %s string, %d decimal, %x hex, %c char\n", 0x0B);
        } else if (strcmp(topic, "sort") == 0) {
            print("sort [FILE] — Sort lines alphabetically (stdin if no file)\n", 0x0B);
        } else if (strcmp(topic, "uniq") == 0) {
            print("uniq [-c] [FILE] — Drop consecutive duplicate lines; -c prefixes counts\n", 0x0B);
        } else if (strcmp(topic, "tee") == 0) {
            print("tee FILE — Write stdin to FILE and stdout (echo | tee file)\n", 0x0B);
        } else if (strcmp(topic, "find") == 0) {
            print("find [DIR] [-name GLOB] — List files recursively; GLOB supports * and ?\n", 0x0B);
        } else {
            print("man: no manual entry for '", 0x0C);
            print(topic, 0x0C);
            print("'\n", 0x0C);
        }
}
