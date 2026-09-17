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
    default:
        break;
    }
    r->rax = ret;
    return (u64)r;
}
