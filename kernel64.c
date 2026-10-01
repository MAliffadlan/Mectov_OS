/* Mectov OS 64-bit entry (M1: long-mode proof + Multiboot2 parse,
 * M2: GDT64/TSS64 + IDT64 + PIC/PIT IRQ).
 *
 * Shares k64/cpu64.h with the k64/ CPU layer; still no src/include
 * dependency (those headers get widened in M3-M4).
 * Called from boot64.asm as kernel64_main(magic, mb_info_phys).
 */
#include "k64/cpu64.h"
#include "k64/spin64.h"

/* M7: serial is shared by all CPUs (syscalls/demos on APs print too).
 * irqsave: timer/IPI handlers print with IF=0 already; a second take from
 * the same CPU can't happen (no print path re-enters). */
static spin64_t serial_lock = SPIN64_INIT;

#define MB2_BOOT_MAGIC 0x36D76289u

/* Multiboot2 framebuffer tag -> VGA-1 console geometry (declared in
 * k64/cpu64.h). Filled while walking the tags below, consumed by
 * cons_init() after mem64_init() has mapped the framebuffer UC (pass 5). */
u64 g_fb_addr;
u32 g_fb_pitch, g_fb_w, g_fb_h, g_fb_bpp;

static inline void outb(u16 port, u8 v) {
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline u8 inb(u16 port) {
    u8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline u64 read_cr3(void) {
    u64 v;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
    return v;
}
static inline u64 read_cr4(void) {
    u64 v;
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(v));
    return v;
}
static inline u64 read_cr0(void) {
    u64 v;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(v));
    return v;
}
static inline u64 read_efer(void) {
    u32 lo, hi;
    __asm__ __volatile__("mov $0xC0000080, %%ecx\n\trdmsr"
                         : "=a"(lo), "=d"(hi) : : "ecx");
    return ((u64)hi << 32) | lo;
}
static inline u16 read_cs(void) {
    u16 v;
    __asm__ __volatile__("mov %%cs, %0" : "=r"(v));
    return v;
}

static void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);   /* 38400 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1 */
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static void s_putc_locked(char c) {
    int t = 100000;
    while (!(inb(COM1 + 5) & 0x20) && t > 0) t--;
    outb(COM1, (u8)c);
    cons_putc(c); /* VGA-1: mirror every kernel/demo byte to the screen */
}

void s_putc(char c) {
    u64 f = spin64_lock_irqsave(&serial_lock);
    s_putc_locked(c);
    spin64_unlock_irqrestore(&serial_lock, f);
}
void s_puts(const char *s) {
    u64 f = spin64_lock_irqsave(&serial_lock);
    for (; *s; s++) {
        if (*s == '\n') s_putc_locked('\r');
        s_putc_locked(*s);
    }
    spin64_unlock_irqrestore(&serial_lock, f);
}
/* Whole-number printing holds the lock once (per-char locking would let
 * two CPUs interleave digits of one value). */
void s_hex64(u64 v) {
    u64 f = spin64_lock_irqsave(&serial_lock);
    s_putc_locked('0');
    s_putc_locked('x');
    for (int i = 15; i >= 0; i--) {
        unsigned d = (unsigned)((v >> (i * 4)) & 0xF);
        s_putc_locked((char)(d < 10 ? '0' + d : 'A' + d - 10));
    }
    spin64_unlock_irqrestore(&serial_lock, f);
}
void s_hex32(u32 v) {
    u64 f = spin64_lock_irqsave(&serial_lock);
    s_putc_locked('0');
    s_putc_locked('x');
    for (int i = 7; i >= 0; i--) {
        unsigned d = (unsigned)((v >> (i * 4)) & 0xF);
        s_putc_locked((char)(d < 10 ? '0' + d : 'A' + d - 10));
    }
    spin64_unlock_irqrestore(&serial_lock, f);
}
void s_dec64(u64 v) {
    char buf[21];
    int n = 0;
    if (v == 0) { s_putc('0'); return; }
    while (v > 0 && n < 20) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    u64 f = spin64_lock_irqsave(&serial_lock);
    while (n > 0) s_putc_locked(buf[--n]);
    spin64_unlock_irqrestore(&serial_lock, f);
}

/* Whole-line printf under ONE lock hold (multi-s_puts sequences from two
 * CPUs interleave otherwise). Minimal verbs: %s %u %x %c %%. Uses
 * __builtin_va_* (freestanding-safe, no headers). */static void emit_u64_locked(u64 v) {
    char buf[21];
    int n = 0;
    if (!v) { s_putc_locked('0'); return; }
    while (v && n < 20) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) s_putc_locked(buf[--n]);
}
static void emit_x64_locked(u64 v) {
    s_putc_locked('0');
    s_putc_locked('x');
    for (int i = 15; i >= 0; i--) {
        unsigned d = (unsigned)((v >> (i * 4)) & 0xF);
        s_putc_locked((char)(d < 10 ? '0' + d : 'A' + d - 10));
    }
}
void s_printf(const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    u64 f = spin64_lock_irqsave(&serial_lock);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            if (*fmt == '\n') s_putc_locked('\r');
            s_putc_locked(*fmt);
            continue;
        }
        fmt++;
        if (*fmt == 's') {
            const char *s = __builtin_va_arg(ap, const char *);
            for (; s && *s; s++) {
                if (*s == '\n') s_putc_locked('\r');
                s_putc_locked(*s);
            }
        } else if (*fmt == 'u') {
            emit_u64_locked(__builtin_va_arg(ap, u64));
        } else if (*fmt == 'x') {
            emit_x64_locked(__builtin_va_arg(ap, u64));
        } else if (*fmt == 'c') {
            s_putc_locked((char)__builtin_va_arg(ap, int));
        } else if (*fmt == '%') {
            s_putc_locked('%');
        } else {
            s_putc_locked('?');
        }
    }
    spin64_unlock_irqrestore(&serial_lock, f);
    __builtin_va_end(ap);
}

/* Raw serial output: NO locking (fatal-dump paths, where another halted CPU
 * may own serial_lock, or tables may be too broken for anything fancy —
 * direct port writes only). */
void s_rawc(char c) {
    int t = 100000;
    while (!(inb(COM1 + 5) & 0x20) && t > 0) t--;
    outb(COM1, (u8)c);
    cons_putc(c); /* VGA-1: fatal dumps must be readable on the screen too */
}

/* M9: the console lock, for operations that must not be interleaved with any
 * other CPU's output — the desktop draws its chrome and re-homes the console
 * in one critical section, otherwise an AP printing mid-draw scrolls the
 * half-finished desktop away (its view is still the full screen until the
 * re-home lands). Printers (s_puts/s_printf/s_write) take this same lock, so
 * holding it freezes the screen exactly as the caller finds it. Callers must
 * NOT print while holding it: the lock is a plain spinlock, not recursive. */
u64 console_lock(void) { return spin64_lock_irqsave(&serial_lock); }
void console_unlock(u64 f) { spin64_unlock_irqrestore(&serial_lock, f); }
void s_raws(const char *s) {
    for (; *s; s++) {
        if (*s == '\n') s_rawc('\r');
        s_rawc(*s);
    }
}
void s_rawx(u64 v) {
    s_raws("0x");
    for (int i = 15; i >= 0; i--) {
        unsigned d = (unsigned)((v >> (i * 4)) & 0xF);
        s_rawc((char)(d < 10 ? '0' + d : 'A' + d - 10));
    }
}
void s_rawu(u64 v) {
    char buf[21];
    int n = 0;
    if (!v) { s_rawc('0'); return; }
    while (v && n < 20) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) s_rawc(buf[--n]);
}

/* Whole-buffer write under ONE lock hold (M7.1 SMP: per-char locking lets
 * concurrent PRINTs interleave mid-line; each syscall must be atomic). */
void s_write(const char *buf, u64 len) {
    if (!buf || !len) return;
    u64 f = spin64_lock_irqsave(&serial_lock);
    for (u64 i = 0; i < len; i++) {
        if (buf[i] == '\n') s_putc_locked('\r');
        s_putc_locked(buf[i]);
    }
    spin64_unlock_irqrestore(&serial_lock, f);
}

/* Multiboot2 tags (packed, u32 type/size, payload, padded to 8). */
struct mb2_tag { u32 type; u32 size; };
#define MB2_TAG_END 0
#define MB2_TAG_CMDLINE 1
#define MB2_TAG_BOOTNAME 2
#define MB2_TAG_MEMMAP 6
#define MB2_TAG_FB 8

/* We run identity-mapped low (boot64.asm maps 0-8MB), so the Multiboot2
 * info pointer (low phys) is directly dereferenceable here. */
void kernel64_main(u64 magic, u64 mb_info) {
    serial_init();
    s_puts("\n[KERNEL64] boot start (long mode)\n");
    /* M7.3 ASLR seed: TSC varies per boot (weak entropy, documented). */
    {
        u32 tsc_lo, tsc_hi;
        __asm__ __volatile__("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));
        aslr_seed(((u64)tsc_hi << 32) | tsc_lo);
    }
    s_puts("[K64] magic=");
    s_hex32((u32)magic);
    s_puts(" mb_info=");
    s_hex64(mb_info);
    s_puts(" cs=");
    s_hex32(read_cs());
    s_puts("\n");

    if ((u32)magic != MB2_BOOT_MAGIC) {
        s_puts("[K64] FAIL: bad multiboot2 magic (want 0x36D76289)\n");
        for (;;) __asm__ __volatile__("cli; hlt");
    }

    u64 cr0 = read_cr0(), cr4 = read_cr4(), cr3 = read_cr3(), efer = read_efer();
    s_puts("[K64] CR3="); s_hex64(cr3);
    s_puts(" EFER="); s_hex64(efer);
    s_puts("\n[K64] CR0.PG=");
    s_putc((cr0 >> 31) & 1 ? '1' : '0');
    s_puts(" CR4.PAE=");
    s_putc((cr4 >> 5) & 1 ? '1' : '0');
    s_puts(" EFER.LME=");
    s_putc((efer >> 8) & 1 ? '1' : '0');
    s_puts("\n");

    u32 total = *(volatile u32 *)(mb_info);
    s_puts("[K64] mb_total_size=");
    s_dec64(total);
    s_puts("\n");

    /* Walk tags. */
    u64 p = mb_info + 8;
    u64 end = mb_info + total;
    u64 usable_ram = 0;
    const char *cmdline = 0;
    int tags = 0;
    while (p + 8 <= end) {
        u32 type = *(volatile u32 *)(p);
        u32 size = *(volatile u32 *)(p + 4);
        if (size < 8 || p + size > end) break;
        if (type == MB2_TAG_END) break;
        tags++;
        if (type == MB2_TAG_CMDLINE) {
            cmdline = (const char *)(p + 8);
        } else if (type == MB2_TAG_MEMMAP) {
            u32 entry_size = *(volatile u32 *)(p + 8);
            /* u32 entry_version at p+12 (ignored) */
            for (u64 e = p + 16; e + entry_size <= p + size; e += entry_size) {
                u64 base = *(volatile u64 *)(e);
                u64 len = *(volatile u64 *)(e + 8);
                u32 etype = *(volatile u32 *)(e + 16);
                if (etype == 1) { /* usable RAM */
                    if (base + len > usable_ram) usable_ram = base + len;
                }
            }
        } else if (type == MB2_TAG_FB) {
            g_fb_addr = *(volatile u64 *)(p + 8);
            g_fb_pitch = *(volatile u32 *)(p + 16);
            g_fb_w = *(volatile u32 *)(p + 20);
            g_fb_h = *(volatile u32 *)(p + 24);
            g_fb_bpp = *(volatile u8 *)(p + 28);
            /* +29 framebuffer_type, +32 colour info (masks) unread: the
             * console assumes GRUB's type=1 RGB packing (VGA-1 note). */
        }
        p += (size + 7) & ~7ULL;
    }
    s_puts("[K64] tags=");
    s_dec64((u64)tags);
    s_puts(" usable_ram_top=");
    s_hex64(usable_ram);
    s_puts("\n[K64] fb addr=");
    s_hex64(g_fb_addr);
    s_puts(" ");
    s_dec64(g_fb_w);
    s_putc('x');
    s_dec64(g_fb_h);
    s_puts("x");
    s_dec64(g_fb_bpp);
    s_puts(" pitch=");
    s_dec64(g_fb_pitch);
    s_puts("\n");
    if (cmdline && cmdline < (const char *)(end)) {
        s_puts("[K64] cmdline=\"");
        /* Print bounded: never run past the info struct on a bad string. */
        for (u64 i = 0; i < 256; i++) {
            const char *c = cmdline + i;
            if ((u64)c >= end) break;
            if (*c == '\0') break;
            s_putc(*c);
        }
        s_puts("\"\n");
    }
    /* M7: "noap" keeps APs parked (BSP-only scheduling, M6 behavior) for
     * bisecting SMP-vs-core bugs. Substring match, bounded like the print. */
    int flag_noap = 0;
    if (cmdline && cmdline < (const char *)(end)) {
        for (u64 i = 0; i < 256; i++) {
            const char *c = cmdline + i;
            if ((u64)(c + 4) >= end) break;
            if (*c == '\0') break;
            if (c[0] == 'n' && c[1] == 'o' && c[2] == 'a' && c[3] == 'p')
                flag_noap = 1;
        }
    }
    s_puts("[K64] BOOTED KERNEL64 LOOP\n");

    /* --- M3: full physical memory + paging (needs mb_info for memmap) --- */
    mem64_init(mb_info);

    /* --- VGA-1: text console on the Multiboot2 framebuffer. UC mapping
     * already exists (mem64 pass 5); unsupported modes fall back to
     * serial-only with a clear log line (system stays fully usable). */
    if (cons_init(g_fb_addr, g_fb_pitch, g_fb_w, g_fb_h, g_fb_bpp))
        s_puts("[K64] cons: framebuffer console live\n");
    else
        s_puts("[K64] cons: no usable framebuffer, serial only\n");

    /* --- M10: kernel heap on the M3 PMM (see k64/heap64.c). Nothing in the
     * kernel allocates yet; the milestones above M10 (VFS buffers, window
     * backing stores, IPC queues) are the consumers. --- */
    heap64_init();

    /* --- M11: block layer (k64/blk64.c). Legacy ATA/ATAPI on the pc machine
     * run64.sh boots: the ISO is the ATAPI device, blkdisk.img the ATA one.
     * Probing is pure polling, so it is safe before the IDT is even loaded. */
    blk64_init();

    /* --- M5: tasks (boot context becomes task[0] idle) + embedded image
     * registry (MCT2/ELF64, parsed by the loader at spawn/exec) --- */
    extern char _binary_demos_hello64_mct_start[];
    extern char _binary_demos_hello64_mct_end[];
    extern char _binary_demos_fpu64_mct_start[];
    extern char _binary_demos_fpu64_mct_end[];
    extern char _binary_demos_clone64_mct_start[];
    extern char _binary_demos_clone64_mct_end[];
    extern char _binary_demos_forkdemo64_mct_start[];
    extern char _binary_demos_forkdemo64_mct_end[];
    extern char _binary_demos_execdemo64_mct_start[];
    extern char _binary_demos_execdemo64_mct_end[];
    extern char _binary_demos_execchild64_elf_start[];
    extern char _binary_demos_execchild64_elf_end[];
    extern char _binary_demos_shell64_mct_start[];
    extern char _binary_demos_shell64_mct_end[];
    extern char _binary_demos_argdemo64_mct_start[];
    extern char _binary_demos_argdemo64_mct_end[];
    extern char _binary_demos_shelltest64_mct_start[];
    extern char _binary_demos_shelltest64_mct_end[];
    extern char _binary_demos_brkdemo64_mct_start[];
    extern char _binary_demos_brkdemo64_mct_end[];
    extern char _binary_demos_nxtest64_mct_start[];
    extern char _binary_demos_nxtest64_mct_end[];
    extern char _binary_demos_asldemo64_mct_start[];
    extern char _binary_demos_asldemo64_mct_end[];
    extern char _binary_demos_smptest64_mct_start[];
    extern char _binary_demos_smptest64_mct_end[];
#define REG(n_, s_)                                                     \
    exec_register(n_, _binary_demos_##s_##_start,                        \
                  (u64)(_binary_demos_##s_##_end - _binary_demos_##s_##_start))
    REG("hello", hello64_mct);
    REG("fpu", fpu64_mct);
    REG("clone", clone64_mct);
    REG("forkdemo", forkdemo64_mct);
    REG("execdemo", execdemo64_mct);
    REG("execchild", execchild64_elf);
    REG("shell", shell64_mct);
    REG("argdemo", argdemo64_mct);
    REG("shelltest", shelltest64_mct);
    REG("brkdemo", brkdemo64_mct);
    REG("nxtest", nxtest64_mct);
    REG("asldemo", asldemo64_mct);
    REG("smptest", smptest64_mct);
#undef REG
    task64_init();
    int dh = task64_spawn_image("hello");
    int df = task64_spawn_image("fpu");
    int dc = task64_spawn_image("clone");
    int dfk = task64_spawn_image("forkdemo");
    int de = task64_spawn_image("execdemo");
    int dsh = task64_spawn_image("shell");
    int dst = task64_spawn_image("shelltest");
    int dbr = task64_spawn_image("brkdemo");
    int dnx = task64_spawn_image("nxtest");
    int das = task64_spawn_image("asldemo");
    int dsm = task64_spawn_image("smptest");
    if (dh < 0 || df < 0 || dc < 0 || dfk < 0 || de < 0 || dsh < 0 ||
        dst < 0 || dbr < 0 || dnx < 0 || das < 0 || dsm < 0) {
        s_puts("[K64] FAIL: demo spawn broke\n");
        for (;;) __asm__ __volatile__("cli; hlt");
    }
    s_puts("[K64] M4 TASK OK (11 user demos)\n");

    /* --- M2: CPU tables + IRQs --- */
    s_puts("[K64] M2 init GDT64...\n");
    gdt64_init();
    s_puts("[K64] M2 GDT64 OK (CS reloaded, TSS+RSP0+IST1 live)\n");
    idt64_init();
    /* PIC+PIT first (M7): LAPIC calibration inside smp_init needs a
     * running PIT; IRQs stay masked (IF=0) until the end of boot. */
    pic_remap_mask_timer_kbd();
    pit_init_hz(100);
    kbd_init(); /* drain stale PS/2 bytes; IRQ1 unmasked in the PIC above */
    mouse64_init(); /* M9: PS/2 aux + IRQ12 (needs the IDT, runs IF=0) */
    /* --- M6: APs to long mode + IPI/TLB plumbing (needs IDT + MMIO) --- */
    smp_init();
    int smp_ok = smp_selftest();
    s_puts("[K64] M6 SMP ");
    s_puts(smp_ok ? "OK" : "DEGRADED");
    s_puts(" ncpus=");
    s_dec64((u64)smp_cpu_count());
    s_puts("\n");
    s_puts("[K64] M2 IDT64 OK, testing int3...\n");
    __asm__ __volatile__("int3");
    s_puts("[K64] EXC3 OK (int3 returned)\n");
    mem64_selftest(); /* pre-STI: any bug faults into the #PF dumper */
    heap64_selftest(); /* M10: same reason, and it must run before any
                        * task can allocate (tasks themselves do not yet) */
    blk64_selftest();  /* M11: reads into heap buffers, so it needs M10 first */
    __asm__ __volatile__("sti");
    sched_set_running(); /* from here AP TLBs matter: shootdowns live */
    if (flag_noap) {
        s_puts("[K64] noap: APs stay parked (BSP-only scheduling)\n");
    } else {
        smp_wake_aps(); /* M7: parked APs join the scheduler */
    }
    /* --- M9 GUI-1: desktop shell. Draws the wallpaper/bars/window, re-homes
     * the console into the window (the boot log's tail comes with it) and puts
     * the mouse cursor on top. Serial stays the primary log: the desktop only
     * mirrors it. --- */
    gui64_init();
    s_puts("[K64] IRQs on (100Hz PIT+LAPIC), waiting ticks...\n");
    for (;;) __asm__ __volatile__("hlt");
}
