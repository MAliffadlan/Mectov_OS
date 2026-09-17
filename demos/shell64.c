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
            put("help ps run exec ticks mem kmem echo sleep cpu exit");
            continue;
        }
        if (!s_cmp(av[0], "ps")) { cmd_ps(); continue; }
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
