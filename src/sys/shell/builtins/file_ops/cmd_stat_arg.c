// src/sys/shell/builtins/file_ops/cmd_stat_arg.c — the `stat` shell command.
// v38.86: report a file's metadata (size, type, links, mode, uid). lstat
// semantics: a symlink is reported as itself, not as its target.
#include "../../shell_internal.h"

void cmd_stat_arg(void) {
        char* fpath = cmd_b + 5;   // past "stat "
        while (*fpath == ' ') fpath++;
        if (*fpath == '\0') {
                print("usage: stat path\n", 0x0C);
                return;
        }
        vfs_stat_t st;
        if (vfs_stat_nofollow(fpath, &st) < 0) {
                print("stat: not found: ", 0x0C);
                print(fpath, 0x0C);
                print("\n", 0x0C);
                return;
        }
        static const char tname[][8] = {"file", "dir", "dev", "ext2-f", "ext2-d", "f32-f", "f32-d", "proc", "symlink"};
        int t = st.type;
        const char* tn = (t >= 0 && t <= 8) ? tname[t] : "?";
        char line[96];
        int p = 0;
        // "  size=<n> type=<t> nlink=<n>"
        const char* pre = "  size=";
        for (int j = 0; pre[j]; j++) line[p++] = pre[j];
        char nb[12];
        int n = st.size, k = 0;
        if (n == 0) nb[k++] = '0';
        while (n > 0 && k < 10) { nb[k++] = (char)('0' + (n % 10)); n /= 10; }
        for (int j = k - 1; j >= 0; j--) line[p++] = nb[j];
        const char* mid = " type=";
        for (int j = 0; mid[j]; j++) line[p++] = mid[j];
        for (int j = 0; tn[j]; j++) line[p++] = tn[j];
        const char* nl = " nlink=";
        for (int j = 0; nl[j]; j++) line[p++] = nl[j];
        n = st.nlink; k = 0;
        if (n == 0) nb[k++] = '0';
        while (n > 0 && k < 10) { nb[k++] = (char)('0' + (n % 10)); n /= 10; }
        for (int j = k - 1; j >= 0; j--) line[p++] = nb[j];
        line[p++] = '\n';
        line[p] = '\0';
        print(line, 0x0F);
        // "  name=<name> uid=<u> mode=<octal>"
        p = 0;
        const char* nm = "  name=";
        for (int j = 0; nm[j]; j++) line[p++] = nm[j];
        for (int j = 0; st.name[j] && p < 80; j++) line[p++] = st.name[j];
        const char* ud = " uid=";
        for (int j = 0; ud[j]; j++) line[p++] = ud[j];
        n = st.uid; k = 0;
        if (n == 0) nb[k++] = '0';
        while (n > 0 && k < 10) { nb[k++] = (char)('0' + (n % 10)); n /= 10; }
        for (int j = k - 1; j >= 0; j--) line[p++] = nb[j];
        const char* md = " mode=";
        for (int j = 0; md[j]; j++) line[p++] = md[j];
        // octal, 4 digits
        line[p++] = (char)('0' + ((st.mode >> 9) & 7));
        line[p++] = (char)('0' + ((st.mode >> 6) & 7));
        line[p++] = (char)('0' + ((st.mode >> 3) & 7));
        line[p++] = (char)('0' + (st.mode & 7));
        line[p++] = '\n';
        line[p] = '\0';
        print(line, 0x0F);
}
