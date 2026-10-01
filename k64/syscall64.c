/* M4 Ring-3 syscall dispatch (M4 ABI0 over int $0x80; see cpu64.h).
 *
 * Runs with IF=0 (interrupt gate), so each handler is atomic against the
 * timer. Numbers mirror the 32-bit kernel where equal (PRINT/TICKS/YIELD/
 * EXIT/PID/CLONE) to keep the future libc mapping 1:1.
 */
#include "cpu64.h"

#define EFAULT 14
#define ENOSYS 38

static u64 sys_print(u64 ptr, u64 len) {
    if (!len) return 0;
    if (len > 2048) return (u64)(long)-EFAULT;
    if (!vmm_user_ok(ptr, len)) return (u64)(long)-EFAULT;
    s_write((const char *)ptr, len);
    return len;
}

/* M12: one op-selected filesystem call (see FS64_OP_* in cpu64.h). The request
 * struct lives in user memory and is read field by field through a volatile
 * pointer after a single range check. OP_CAT is the only op that writes through
 * the request (via req.buf): fs64_read() copies straight into the caller's
 * buffer, which is safe for the same reason sys_print() is — the syscall runs
 * on the caller's CR3, so a validated user VA is a real address.
 *
 * Note for callers: this runs with IF=0 like every int $0x80 handler, and the
 * FS layer holds a lock across its device reads, so a large cat() stalls this
 * CPU for the duration of the transfer. Chunked reads (the shell uses 2 KiB)
 * keep that bounded; a future async path is M13 territory. */
static u64 sys_fsop(u64 ptr) {
    if (!vmm_user_ok(ptr, sizeof(fs64_req_t))) return (u64)(long)-EFAULT;
    volatile fs64_req_t *rq = (volatile fs64_req_t *)ptr;
    if (rq->magic != FS64_REQ_MAGIC) {
        /* The magic exists precisely to catch a layout drift between the two
         * mirrored headers (cpu64.h vs demos/sys64.h), so say what arrived and
         * what was expected rather than reinterpreting fields. */
        s_puts("[K64] fsop: bad magic=");
        s_hex32(rq->magic);
        s_puts(" want=");
        s_hex32(FS64_REQ_MAGIC);
        s_puts(" size=");
        s_dec64(sizeof(fs64_req_t));
        s_puts(" pid=");
        s_dec64((u64)(long)task64_current_id());
        s_puts("\n");
        return (u64)(long)-22; /* EINVAL */
    }
    u32 op = rq->op;
    int mnt = (int)rq->mnt;

    /* Bounded copy of the path: 128 bytes, must be NUL-terminated inside the
     * field (an unterminated one would read past the struct). */
    char path[129];
    int n = 0;
    for (; n < 128; n++) {
        path[n] = rq->path[n];
        if (!path[n]) break;
    }
    path[128] = '\0';
    if (n == 128) {
        s_puts("[K64] fsop: unterminated path field\n");
        return (u64)(long)-22;
    }

    long ret;
    switch (op) {
    case FS64_OP_LS: {
        fs64_dirent_t e;
        ret = fs64_readdir(mnt, path, rq->index, &e);
        if (ret > 0) {
            for (u64 i = 0; i < sizeof(e.name); i++) rq->ent.name[i] = e.name[i];
            rq->ent.size = e.size;
            rq->ent.id = e.id;
            rq->ent.is_dir = e.is_dir;
        }
        break;
    }
    case FS64_OP_STAT: {
        fs64_node_t nd;
        ret = fs64_lookup(mnt, path, &nd);
        if (ret == 0) {
            const char *base = path;
            for (const char *p = path; *p; p++)
                if (*p == '/') base = p + 1;
            int i = 0;
            if (!base[0]) rq->ent.name[i++] = '/'; /* stat of the root itself */
            for (; base[i] && i < (int)sizeof(rq->ent.name) - 1; i++)
                rq->ent.name[i] = base[i];
            rq->ent.name[i] = '\0';
            rq->ent.size = nd.size;
            rq->ent.id = nd.id;
            rq->ent.is_dir = nd.is_dir;
        }
        break;
    }
    case FS64_OP_CAT: {
        u64 ubuf = rq->buf, len = rq->len;
        if (!len || len > 65536) {
            ret = -E64_EINVAL;
            break;
        }
        if (!vmm_user_ok(ubuf, len)) {
            ret = -EFAULT;
            break;
        }
        ret = fs64_read(mnt, path, rq->off, (void *)ubuf, (u32)len);
        break;
    }
    case FS64_OP_MOUNT: {
        /* Enumerate the mount table without a second ABI for it: the request
         * fields carry what a `fs` builtin needs (type, block count, label). */
        const fs64_mount_t *mt = fs64_mount((int)rq->index);
        if (!mt) {
            ret = -E64_ENOENT;
            break;
        }
        int i = 0;
        for (; mt->label[i] && i < (int)sizeof(rq->ent.name) - 1; i++)
            rq->ent.name[i] = mt->label[i];
        rq->ent.name[i] = '\0';
        rq->ent.id = (u32)mt->type;
        rq->ent.size = mt->blocks;
        rq->ent.is_dir = 1; /* a mount point is always traversable */
        ret = 0;
        break;
    }
    case FS64_OP_HASH: {
        /* FNV-1a 64 of the first `len` bytes (0 = whole file) into req.hash,
         * and rc stays a plain success/error: a hash is a u64 someone can
         * compute honestly with the top bit set. */
        long hrc = 0;
        u64 h = fs64_hash(mnt, path, rq->len, &hrc);
        if (hrc) {
            ret = hrc;
            break;
        }
        rq->hash = h;
        ret = 0;
        break;
    }
    default:
        /* An unknown op is an ABI drift between the two headers, and it is
         * worth a log line: the first symptom otherwise is a command that
         * silently reports "invalid" for no visible reason. */
        s_puts("[K64] fsop: bad op=");
        s_dec64((u64)op);
        s_puts(" mnt=");
        s_dec64((u64)(long)mnt);
        s_puts("\n");
        ret = -E64_EINVAL;
        break;
    }
    rq->rc = ret;
    return (u64)ret;
}

u64 syscall64_dispatch(regs64_t *r) {
    u32 n = (u32)r->rax;
    u64 a = r->rbx, b = r->rcx, c = r->rdx;
    u64 ret = (u64)(long)-ENOSYS;
    switch (n) {
    case SYS64_PRINT:
        ret = sys_print(a, b);
        break;
    case SYS64_TICKS:
        ret = k64_ticks();
        break;
    case SYS64_YIELD:
        r->rax = 0;
        return task64_schedule(r);
    case SYS64_EXIT:
        task64_exit((int)a);
        return task64_schedule(r); /* noreturn for the caller */
    case SYS64_SLEEP:
        return task64_sleep(a, r); /* blocks: rewinds RIP, schedules away */
    case SYS64_BRK:
        return task64_brk(a, r);
    case SYS64_GETBASE: {
        task64_t *self = 0;
        /* current task via helper (no direct cur access outside task64.c) */
        extern task64_t *task64_self(void);
        self = task64_self();
        ret = self ? self->user_base : 0;
        break;
    }
    case SYS64_PID:
        ret = (u64)(long)task64_current_id();
        break;
    case SYS64_FORK:
        ret = (u64)(long)task64_fork(r);
        break;
    case SYS64_MEMINFO: {
        if (!vmm_user_ok(a, 16)) { ret = (u64)(long)-14; break; }
        volatile meminfo_t *mi = (volatile meminfo_t *)a;
        mi->total_frames = pmm_total_frames();
        mi->free_frames = pmm_free_frames();
        ret = 0;
        break;
    }
    case SYS64_GETCPU:
        ret = (u64)(long)smp_cpu_index();
        break;
    case SYS64_KMEMSTATS: {
        /* M10: kernel heap snapshot. 152 bytes, one page is plenty. */
        if (!vmm_user_ok(a, sizeof(kmem64_t))) {
            ret = (u64)(long)-14;
            break;
        }
        heap64_stats((kmem64_t *)a);
        ret = 0;
        break;
    }
    case SYS64_KMEMPROBE: {
        /* M10: a real alloc/write/read/free round trip performed for the
         * CALLER, so it runs on the caller's CR3 — if the arena were mapped
         * only in boot's tables this would fault instead of returning. */
        if (a == 0 || a > (1ULL << 20)) {
            ret = (u64)(long)-22;
            break;
        }
        u64 got = heap64_probe(a);
        ret = got ? got : (u64)(long)-12; /* -ENOMEM */
        break;
    }
    case SYS64_PS: {
        /* a = buf (RBX), b = max (RCX). (A past bug read max from RDX and
         * buf from RCX — every ps failed with -EINVAL/-EFAULT.) */
        u64 max = b;
        if (max == 0 || max > 64) { ret = (u64)(long)-22; break; }
        if (!vmm_user_ok(a, max * sizeof(ps_entry_t))) {
            ret = (u64)(long)-14;
            break;
        }
        ret = (u64)(long)task64_ps((ps_entry_t *)a, (int)max);
        break;
    }
    case SYS64_GETCHAR:
        ret = (u64)(long)kbd_try_get();
        break;
    case SYS64_SPAWN: {
        /* a = name, b = argc, c = argv (all user). Bounded kernel copies. */
        int argc = (int)b;
        if (argc < 0 || argc > 16) { ret = (u64)(long)-22; break; }
        if (!vmm_user_ok(a, 17)) { ret = (u64)(long)-14; break; }
        char kname[17];
        int i = 0;
        for (; i < 16; i++) {
            kname[i] = ((volatile const char *)a)[i];
            if (!kname[i]) break;
        }
        kname[16] = '\0';
        if (i == 16) { ret = (u64)(long)-22; break; }
        if (argc > 0) {
            if (!vmm_user_ok(c, (u64)argc * 8)) {
                ret = (u64)(long)-14;
                break;
            }
        }
        char kargs[16][129]; /* 2KB on the 16KB kstack: safe, and private
                             * per call (no cross-CPU sharing like static) */
        char *kptrs[16];
        int bad = 0;
        for (int k = 0; k < argc && !bad; k++) {
            u64 uptr;
            /* u64 load may tear? aligned u64 user read: single mov. */
            uptr = ((volatile const u64 *)c)[k];
            if (!vmm_user_ok(uptr, 129)) { bad = 1; break; }
            int j = 0;
            for (; j < 128; j++) {
                kargs[k][j] = ((volatile const char *)uptr)[j];
                if (!kargs[k][j]) break;
            }
            kargs[k][128] = '\0';
            if (j == 128) { bad = 1; break; }
            kptrs[k] = kargs[k];
        }
        if (bad) { ret = (u64)(long)-14; break; }
        ret = (u64)(long)task64_spawn_args(kname, argc, kptrs);
        break;
    }
    case SYS64_EXEC:
        return task64_exec((const char *)a, r);
    case SYS64_WAITPID:
        return task64_waitpid((int)a, b, (int)c, r);
    case SYS64_CLONE:
        ret = (u64)(long)task64_clone(a);
        break;
    case SYS64_FSOP:
        ret = sys_fsop(a);
        break;
    default:
        break;
    }
    r->rax = ret;
    return (u64)r;
}
