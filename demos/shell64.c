/* M7.2 mct shell: foreground `run` (spawn+waitpid), ps/mem/ticks/echo/sleep.
 * Line editing: echo, backspace, enter. No job control, no quotes (M8?).
 */
#include "sys64.h"

static void put(const char *s) {
    DLINE(l);
    dl_s(&l, s);
    dl_nl(&l);
}

/* strcmp/strncmp/strcpy, freestanding. */
static int s_cmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void cmd_ps(void) {
    ps_entry_t list[32];
    long n = d_ps(list, 32);
    if (n < 0) { put("ps failed"); return; }
    DLINE(l);
    for (long i = 0; i < n; i++) {
        dl_s(&l, "PS id=");
        dl_u(&l, (u64)list[i].id);
        dl_s(&l, " st=");
        dl_u(&l, (u64)list[i].state);
        dl_s(&l, " par=");
        dl_u(&l, (u64)list[i].parent);
        dl_s(&l, " cpu=");
        dl_u(&l, (u64)list[i].cpu);
        dl_s(&l, " ");
        dl_s(&l, list[i].name);
        dl_nl(&l);
    }
    dl_s(&l, "PS-DONE");
    dl_nl(&l);
}

static void cmd_mem(void) {
    meminfo_t m;
    if (d_meminfo(&m) != 0) { put("mem failed"); return; }
    DLINE(l);
    dl_s(&l, "MEM total=");
    dl_u(&l, m.total_frames);
    dl_s(&l, " free=");
    dl_u(&l, m.free_frames);
    dl_nl(&l);
}

/* M10: kernel heap stats, plus a real alloc/write/read/free round trip. The
 * probe closes the loop that matters: it runs on THIS task's PML4, so an
 * arena mapped only in boot's tables would fault here instead of returning. */
static void cmd_kmem(void) {
    long probe = d_kmemprobe(8192);
    kmem64_t s;
    if (d_kmemstats(&s) != 0) { put("kmem failed"); return; }
    DLINE(l);
    dl_s(&l, "KMEM probe=");
    dl_s(&l, probe > 0 ? "ok" : "FAIL");
    dl_s(&l, " allocs=");
    dl_u(&l, s.allocs);
    dl_s(&l, " frees=");
    dl_u(&l, s.frees);
    dl_s(&l, " live=" );
    dl_u(&l, s.allocated);
    dl_s(&l, " freeblk=");
    dl_u(&l, s.free_blocks);
    dl_s(&l, " largest=");
    dl_u(&l, s.largest_free);
    dl_s(&l, " pages=");
    dl_u(&l, s.pages);
    dl_s(&l, " oom=");
    dl_u(&l, s.oom);
    dl_s(&l, " bad=");
    dl_u(&l, s.canary_failures + s.magic_failures);
    dl_s(&l, " probes=");
    dl_u(&l, s.probes);
    dl_nl(&l);
    dl_s(&l, "KMEM-DONE");
    dl_nl(&l);
}

/* M12: filesystem builtins. Mounts are fixed at boot (0 = the boot CD as
 * ISO9660, 1 = the ext2 test disk when attached), so there is no mount(2) yet —
 * `fs` lists what the kernel actually mounted, and that list is the truth the
 * other three commands index into.
 *
 * Requests go through ONE stack-allocated d_fsreq_t: the struct is ~230 bytes,
 * which is fine on a task stack, and putting it on the stack keeps concurrent
 * shells (SMP) from sharing request state. */
static void fs_report_fail(const char *what, long rc) {
    DLINE(l);
    dl_s(&l, what);
    dl_s(&l, " failed rc=");
    dl_u(&l, (u64)(-rc));
    dl_nl(&l);
}

static const char *fs_type_name(u64 t) {
    if (t == 1) return "iso9660";
    if (t == 2) return "ext2";
    return "?";
}

/* Args may name the mount as a bare single digit (`ls /boot 0`), so the same
 * commands reach both filesystems without a second syntax: the last lone digit
 * is the mount id, everything else is the path. */
static void fs_args(char **av, int ac, const char **path, long *mnt) {
    *path = "/";
    *mnt = 0;
    for (int i = 0; i < ac; i++) {
        if (av[i][0] >= '0' && av[i][0] <= '9' && !av[i][1])
            *mnt = av[i][0] - '0';
        else
            *path = av[i];
    }
}

static void cmd_fs(void) {
    /* The mount table is kernel state; the shell reads it through the same op
     * stream as everything else, so there is no second ABI to keep in sync. */
    d_fsreq_t q;
    int n = 0;
    for (u32 m = 0; m < 4; m++) {
        long rc = d_fs_mount(&q, m);
        if (rc < 0) {
            if (!n) fs_report_fail("fs", rc);
            break;
        }
        n++;
        DLINE(l);
        dl_s(&l, "FS mnt=");
        dl_u(&l, m);
        dl_s(&l, " ");
        dl_s(&l, fs_type_name(q.ent.id));
        dl_s(&l, " blocks=");
        dl_u(&l, q.ent.size);
        dl_s(&l, " label=\"");
        dl_s(&l, q.ent.name);
        dl_s(&l, "\"");
        dl_nl(&l);
    }
    DLINE(t);
    dl_s(&t, "FS-DONE mounts=");
    dl_u(&t, (u64)n);
    dl_nl(&t);
}

static void cmd_ls(char **av, int ac) {
    const char *path;
    long mnt;
    d_fsreq_t q;
    fs_args(av, ac, &path, &mnt);
    DLINE(l);
    dl_s(&l, "LS ");
    dl_s(&l, path);
    dl_nl(&l);
    int n = 0;
    for (u32 i = 0; i < 512; i++) {
        long rc = d_fs_ls(&q, (u32)mnt, path, i);
        if (rc == 0) break;
        if (rc < 0) { fs_report_fail("ls", rc); return; }
        n++;
        DLINE(e);
        dl_s(&e, "  ");
        dl_s(&e, q.ent.is_dir ? "d " : "- ");
        dl_u(&e, q.ent.size);
        dl_s(&e, "\t");
        dl_s(&e, q.ent.name);
        dl_nl(&e);
    }
    DLINE(t);
    dl_s(&t, "LS-DONE n=");
    dl_u(&t, (u64)n);
    dl_nl(&t);
}

static void cmd_cat(char **av, int ac) {
    if (ac < 1) { put("usage: cat <path> [mnt]"); return; }
    const char *path;
    long mnt;
    d_fsreq_t q;
    fs_args(av, ac, &path, &mnt);
    long rc = d_fs_stat(&q, (u32)mnt, path);
    if (rc < 0) { fs_report_fail("cat stat", rc); return; }
    if (q.ent.is_dir) { put("cat: is a directory"); return; }
    DLINE(l);
    dl_s(&l, "CAT ");
    dl_s(&l, q.ent.name);
    dl_s(&l, " size=");
    dl_u(&l, q.ent.size);
    dl_nl(&l);
    DLINE(hn);
    static char buf[2048]; /* one shell: reads are foreground, no re-entry */
    u64 off = 0;
    while (off < q.ent.size) {
        long got = d_fs_cat(&q, (u32)mnt, path, off, buf, sizeof(buf));
        if (got <= 0) { fs_report_fail("cat read", got ? got : -5); return; }
        sys2(1, (u64)buf, (u64)got); /* raw: file bytes must not be mangled */
        off += (u64)got;
    }
    dl_s(&hn, "CAT-DONE bytes=");
    dl_u(&hn, off);
    dl_nl(&hn);
}

/* Ring-3 hash of a file: the guest computes it through the same read path a
 * program would, and scripts/fs_test.py recomputes it from its own copy. */
static void cmd_hash64(char **av, int ac) {
    if (ac < 1) { put("usage: hash64 <path> [mnt]"); return; }
    const char *path;
    long mnt;
    d_fsreq_t q;
    fs_args(av, ac, &path, &mnt);
    DLINE(l);
    if (d_fs_hash(&q, (u32)mnt, path, 0) < 0) {
        dl_s(&l, "HASH64 rc=");
        dl_u(&l, (u64)(-q.rc));
        dl_nl(&l);
        return;
    }
    dl_s(&l, "HASH64 mnt=");
    dl_u(&l, (u64)mnt);
    dl_s(&l, " path=");
    dl_s(&l, path);
    dl_s(&l, " fnv=");
    dl_x(&l, q.hash);
    dl_nl(&l);
}

static void cmd_stat(char **av, int ac) {
    if (ac < 1) { put("usage: stat <path> [mnt]"); return; }
    const char *path;
    long mnt;
    d_fsreq_t q;
    fs_args(av, ac, &path, &mnt);
    long rc = d_fs_stat(&q, (u32)mnt, path);
    DLINE(l);
    if (rc < 0) {
        dl_s(&l, "STAT rc=");
        dl_u(&l, (u64)(-rc));
        dl_nl(&l);
        dl_s(&l, "STAT-DONE");
        dl_nl(&l);
        return;
    }
    dl_s(&l, "STAT name=");
    dl_s(&l, q.ent.name);
    dl_s(&l, " size=");
    dl_u(&l, q.ent.size);
    dl_s(&l, " id=");
    dl_u(&l, q.ent.id);
    dl_s(&l, " dir=");
    dl_u(&l, (u64)(q.ent.is_dir ? 1 : 0));
    dl_s(&l, " mnt=");
    dl_u(&l, (u64)mnt);
    dl_nl(&l);
    dl_s(&l, "STAT-DONE");
    dl_nl(&l);
}

static void cmd_run(char **av, int ac) {
    if (ac < 1) { put("usage: run <name> [args...]"); return; }
    /* Unix-style: argv[0] is the command name itself. */
    long id = d_spawn(av[0], (long)ac, (const char **)av);
    if (id < 0) {
        DLINE(l);
        dl_s(&l, "spawn failed ");
        dl_u(&l, (u64)(-id));
        dl_nl(&l);
        return;
    }
    /* Foreground: reap it (serializes output, no zombie leak). */
    long st = 0;
    long w = d_wait(id, &st, 0);
    DLINE(l);
    dl_s(&l, "reaped ");
    dl_u(&l, (u64)w);
    dl_s(&l, " status=");
    dl_u(&l, (u64)st);
    dl_nl(&l);
}

void demo_main(void) {
    put("MCT SHELL (help for commands)");
    static char line[128];
    for (;;) {
        /* Prompt (no newline): raw print of "mct> ". */
        sys2(1, (u64)"mct> ", 5);
        int n = 0;
        for (;;) {
            long c = d_getchar();
            if (c < 0) { d_sleep(2); continue; }
            if (c == '\n' || c == '\r') { d_puts("\n"); break; }
            if (c == '\b' || c == 127) {
                if (n > 0) { n--; d_puts("\b \b"); }
                continue;
            }
            if (c < 32 || c > 126) continue;
            if (n < 127) {
                line[n++] = (char)c;
                char tmp[2] = { (char)c, '\0' };
                d_puts(tmp);
            }
        }
        line[n] = '\0';
        /* Tokenize (max 8 args, spaces only). */
        char *av[9];
        int ac = 0;
        for (int i = 0; i < n && ac < 8;) {
            while (i < n && line[i] == ' ') i++;
            if (i >= n) break;
            av[ac++] = &line[i];
            while (i < n && line[i] != ' ') i++;
            if (i < n) line[i++] = '\0';
        }
        av[ac] = 0;
        if (ac == 0) continue;
        if (!s_cmp(av[0], "exit")) return;
        if (!s_cmp(av[0], "help")) {
            put("help ps run exec ticks mem kmem fs ls cat stat hash64 echo "
                "sleep cpu exit");
            continue;
        }
        if (!s_cmp(av[0], "ps")) { cmd_ps(); continue; }
        if (!s_cmp(av[0], "fs")) { cmd_fs(); continue; }
        if (!s_cmp(av[0], "ls")) { cmd_ls(av + 1, ac - 1); continue; }
        if (!s_cmp(av[0], "cat")) { cmd_cat(av + 1, ac - 1); continue; }
        if (!s_cmp(av[0], "stat")) { cmd_stat(av + 1, ac - 1); continue; }
        if (!s_cmp(av[0], "hash64")) { cmd_hash64(av + 1, ac - 1); continue; }
        if (!s_cmp(av[0], "mem")) { cmd_mem(); continue; }
        if (!s_cmp(av[0], "kmem")) { cmd_kmem(); continue; }
        if (!s_cmp(av[0], "ticks")) {
            DLINE(l);
            dl_s(&l, "ticks=");
            dl_u(&l, (u64)d_ticks());
            dl_nl(&l);
            continue;
        }
        if (!s_cmp(av[0], "cpu")) {
            DLINE(l);
            dl_s(&l, "cpu=");
            dl_u(&l, (u64)d_getcpu());
            dl_nl(&l);
            continue;
        }
        if (!s_cmp(av[0], "echo")) {
            DLINE(l);
            for (int i = 1; i < ac; i++) {
                if (i > 1) dl_s(&l, " ");
                dl_s(&l, av[i]);
            }
            dl_nl(&l);
            continue;
        }
        if (!s_cmp(av[0], "sleep")) {
            long t = 0;
            if (ac > 1) {
                for (const char *p = av[1]; *p >= '0' && *p <= '9'; p++)
                    t = t * 10 + (*p - '0');
            }
            d_sleep((u64)t);
            continue;
        }
        if (!s_cmp(av[0], "run")) { cmd_run(av + 1, ac - 1); continue; }
        if (!s_cmp(av[0], "exec")) {
            if (ac < 2) { put("usage: exec <name>"); continue; }
            d_exec(av[1]);
            put("exec failed");
            continue;
        }
        put("unknown command (try help)");
    }
}
