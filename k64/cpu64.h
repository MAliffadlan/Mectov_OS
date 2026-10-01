/* Mectov OS 64-bit CPU layer (M2): shared types + declarations.
 *
 * Dependency-free on purpose: nothing from src/include (those headers are
 * still 32-bit and get widened in M3-M4). Shared by the k64 layer and
 * kernel64.c.
 */
#ifndef CPU64_H
#define CPU64_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define COM1 0x3F8u

/* Serial output (defined in kernel64.c, used by k64/isr64.c too). */
void s_putc(char c);
void s_puts(const char *s);

/* VGA-1 (M8): text console on the Multiboot2 linear framebuffer (defined in
 * k64/console64.c). cons_init() returns 0 for modes it cannot render (no FB
 * tag, bpp it cannot pack, silly geometry): the kernel then keeps running
 * serial-only, exactly as its log line says. cons_putc() is called from the
 * serial paths in kernel64.c, so every kernel/demo byte reaches both. */
int cons_init(u64 addr, u32 pitch, u32 w, u32 h, u32 bpp);
/* M9: console as a text VIEW over a framebuffer rectangle, so the desktop can
 * host it in a window. cons_rehome() moves a live console and carries the tail
 * of the log with it; the dirty hook reports every changed rect (the desktop
 * uses it to keep its composited mouse cursor on top of fresh text). */
int cons_init_at(u64 addr, u32 pitch, u32 ox, u32 oy, u32 w, u32 h, u32 bpp);
int cons_rehome(u32 ox, u32 oy, u32 w, u32 h);
void cons_view_cells(int *cols, int *rows);
void cons_set_colors(u32 fg, u32 bg);
void cons_set_dirty_hook(void (*fn)(int x0, int y0, int x1, int y1));
/* Called BEFORE a bulk mutation (scroll/wipe) so a composited cursor can be
 * lifted off the pixels first — a shift would otherwise copy the sprite and
 * leave a ghost behind. */
void cons_set_hide_hook(void (*fn)(void));
void cons_putc(char c);
/* Framebuffer geometry from the Multiboot2 tag: kernel64.c fills these in
 * while walking the tags (before mem64_init maps the FB in pass 5). */
extern u64 g_fb_addr;
extern u32 g_fb_pitch, g_fb_w, g_fb_h, g_fb_bpp;

/* M9 GUI-1 framebuffer core (k64/gfx64.c): the single place that knows the
 * pixel format + clipping. bg == -1 in the text calls means "transparent". */
int gfx_init(u64 addr, u32 pitch, u32 w, u32 h, u32 bpp);
int gfx_ready(void);
int gfx_width(void);
int gfx_height(void);
u64 gfx_read(int x, int y);
void gfx_px(int x, int y, u32 rgb);
void gfx_fill(int x, int y, int w, int h, u32 rgb);
void gfx_hline(int x, int y, int w, u32 rgb);
void gfx_vline(int x, int y, int h, u32 rgb);
void gfx_frame(int x, int y, int w, int h, u32 rgb);
void gfx_vgrad(int x, int y, int w, int h, u32 top, u32 bot);
void gfx_shift_up(int x, int y, int w, int h, int dy);
void gfx_cell(int x, int y, unsigned char ch, u32 fg, int bg);
void gfx_cell_scale(int x, int y, unsigned char ch, u32 fg, int bg, int scale);
void gfx_text(int x, int y, const char *s, u32 fg, int bg);
void gfx_text_scale(int x, int y, const char *s, u32 fg, int bg, int scale);

/* M9 GUI-1 PS/2 mouse (k64/mouse64.c): IRQ12 + 3-byte packets. ps2_drain64()
 * is called by both the keyboard and the mouse IRQ (shared 8042 buffer) and
 * routes each byte by the AUX status bit. */
void mouse64_init(void);
void mouse64_feed(u8 byte);
void ps2_drain64(void);
int mouse64_x(void);
int mouse64_y(void);
int mouse64_buttons(void);
void mouse64_set_move_hook(void (*fn)(int dx, int dy));

/* M9 GUI-1 desktop (k64/gui64.c): wallpaper + bars + window, with the console
 * re-homed into the window's client rect and a composited mouse cursor. */
int gui64_init(void);
void gui64_dirty(int x0, int y0, int x1, int y1);
void s_hex64(u64 v);
void s_hex32(u32 v);
void s_dec64(u64 v);
void s_printf(const char *fmt, ...); /* M7: whole line, one lock hold */
void s_write(const char *buf, u64 len); /* M7: ditto, raw buffer */
void s_rawc(char c);  /* M7: lock-free serial (fatal dumps) */
/* M9: hold the console lock across a multi-step screen change (the desktop
 * draws and re-homes the console in one go). Never print while holding it. */
u64 console_lock(void);
void console_unlock(u64 f);
void s_raws(const char *s);
void s_rawx(u64 v);
void s_rawu(u64 v);

/* 64-bit interrupt frame built by k64/entry64.asm isr64_common.
 * Field order MUST match the stub push order:
 *   push vec, push err, push rax..r15  ->  rdi = rsp points at r15.
 * Then CPU frame: rip, cs, rflags, rsp, ss (always all five in 64-bit mode).
 */
typedef struct {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vec, err;
    u64 rip, cs, rflags, rsp, ss;
} regs64_t;

/* Init + IRQ (defined in k64/). */
void gdt64_init(void);
void gdt64_ap_load(int idx, u64 rsp0); /* M6: AP's own TR + shared GDTR */
void tss64_set_rsp0(u64 rsp0);
void tss64_set_rsp0_cpu(int cpu, u64 rsp0); /* M7: per-CPU RSP0 */
void idt64_init(void);
void idt64_load(void); /* M6: reload the shared IDT on an AP */
void pic_remap_mask_timer_kbd(void);
void pit_init_hz(u32 hz);
u64 isr64_handler(regs64_t *r);
u64 k64_ticks(void);

/* M4 task + syscall layer (defined in k64/task64.c, k64/syscall64.c).
 *
 * M4 ABI0 (via int $0x80, numbers mirror the 32-bit kernel where equal):
 *   EAX = number, EBX/ECX/EDX/ESI/EDI = args (full 64-bit regs, pointers
 *   are canonical user VAs). Returns in RAX (negative = -errno).
 *     1 PRINT  RBX=ptr RCX=len (<=2048, must be US-mapped) -> bytes
 *     8 GET_TICKS -> PIT ticks
 *     9 YIELD  -> 0 (voluntary reschedule)
 *    10 EXIT   RBX=status (noreturn; M5: zombie until reaped)
 *    19 SLEEP  RBX=ticks to sleep (M5: BLOCKED until deadline) -> 0
 *    20 GET_PID -> task id
 *    71 FORK   (M5: COW address-space clone) -> child id / 0
 *    72 WAITPID RBX=pid(-1 any) RCX=status_ptr RDX=WNOHANG -> pid/0/-ECHILD
 *    76 EXEC   RBX=name_ptr (M5: embedded image name) -> 0 (noreturn-ish)
 *   104 CLONE  RBX=func_va (child shares the address space, M4) -> child id
 * M5: every user task owns a private PML4 (kernel half shared by pointer);
 * fork() COW-shares user pages, exec() replaces the image (MCT64/ELF64).
 */
#define SYS64_PRINT 1
#define SYS64_TICKS 8
#define SYS64_YIELD 9
#define SYS64_EXIT 10
#define SYS64_SLEEP 19
#define SYS64_PID 20
#define SYS64_FORK 71
#define SYS64_WAITPID 72
#define SYS64_EXEC 76
#define SYS64_CLONE 104
#define SYS64_MEMINFO 131 /* M7.2: RBX=ptr{total,free u64} -> 0 */
#define SYS64_GETBASE 132 /* M7.3: -> own image base (ASLR proof) */
#define SYS64_GETCPU 133  /* M7.2: -> current cpu index */
#define SYS64_PS 134      /* M7.2: RBX=ptr RCX=max -> count filled */
#define SYS64_GETCHAR 135 /* M7.2: nonblocking key, -1 if empty */
#define SYS64_SPAWN 136   /* M7.2: RBX=name RCX=argc RDX=argv -> id */
#define SYS64_BRK 120     /* M7.3: RBX=new_brk (0 = query) -> brk */
#define SYS64_KMEMSTATS 137 /* M10: RBX=kmem64_t* -> 0 (kernel heap snapshot) */
#define SYS64_KMEMPROBE 138 /* M10: RBX=bytes (<=1MB, in one go) -> bytes/0 */

/* ps/meminfo shared layouts (kernel + demos/libc, fixed sizes). */
typedef struct {
    int id, state, parent, cpu;
    char name[16];
} ps_entry_t;
typedef struct {
    u64 total_frames, free_frames;
} meminfo_t;
/* M10 kernel heap snapshot (SYS64_KMEMSTATS / dev shell `kmem`). The 32-bit
 * kmalloc_stats_t is a 32-bit struct; this arena has 64-bit addresses and
 * counters, so the layout is its own. */
typedef struct {
    u64 arena_base, arena_max, arena_used, arena_mapped;
    u64 pages, growths;
    u64 allocated, free_bytes, blocks, free_blocks, largest_free;
    u64 allocs, frees, oom, canary_failures, magic_failures, probes,
        probe_failures;
} kmem64_t;
void heap64_init(void);
void heap64_selftest(void);
void *kmalloc(u64 size);
void kfree(void *p);
void *krealloc(void *p, u64 size);
void *kcalloc(u64 n, u64 size);
void heap64_stats(kmem64_t *out);
u64 heap64_probe(u64 bytes); /* Ring-3 reachable round trip; bytes or 0 */

/* M11 block layer (k64/blk64.c): legacy ATA PIO + ATAPI, read-only, polling.
 * Slots are 0..3 = ide0 master/slave, ide1 master/slave. blk64_read() is the
 * single entry point the layers above need; callers must consult sector_size
 * (512 for an ATA disk, 2048 for the boot CD) instead of assuming. */
typedef struct {
    int present;
    int atapi;
    int lba48;
    u32 sector_size;
    u64 sectors;
    char model[41];
    char serial[21];
} blk64_dev_t;
void blk64_init(void);
void blk64_selftest(void);
int blk64_count(void);
const blk64_dev_t *blk64_dev(int slot);
int blk64_iso_slot(void);  /* first ATAPI device (the boot CD), -1 if none */
int blk64_disk_slot(void); /* first ATA device, -1 if none */
int blk64_read(int slot, u64 lba, u32 count, void *buf);
int blk64_read28(int slot, u64 lba, u32 count, void *buf); /* force LBA28 */
int blk64_read48(int slot, u64 lba, u32 count, void *buf); /* force LBA48 */

/* Shared negative-errno codes: FS layer, syscalls, and anything that has to
 * say "why" instead of just failing. */
#define SYS64_PRINT 1
#define SYS64_TICKS 8
#define SYS64_YIELD 9
#define SYS64_EXIT 10
#define SYS64_PID 20
#define SYS64_CLONE 104

/* Ring-3 layout (M5: every task owns its PML4; user ranges never alias the
 * low identity map, so shared kernel tables are never written by tasks). */
#define USTACK_BASE 0x50000000ULL /* per-task user stacks live here */
#define USTACK_SLOT 0x5000ULL     /* 16KB stack + 4KB unmapped guard hole */
#define USTACK_SIZE 0x4000ULL
#define DEMO_THREAD_EXIT_OFF 0x40ULL /* _thread_exit offset in demo images */
#define DEMO_START_ARGS_OFF 0x80ULL  /* M7.2 _start_args (argc/argv) offset */

/* Task states. */
#define T_FREE 0
#define T_READY 1
#define T_RUNNING 2
#define T_ZOMBIE 3   /* M5: exited, exit_code kept until waitpid reaps */
#define T_BLOCKED 4  /* M5: waitpid (waiting_for) or sleep (wakeup_tick) */
#define T_NEW 5      /* M7: slot reserved, not yet runnable (SMP publish) */

/* M6: hard cap for per-CPU tables (GDT TSS slots, AP stacks, ack slots).
 * qemu/run64 boots -smp 4; larger -smp values park the extras unstarted. */
#define NCPU_MAX 4

typedef struct task64 {
    u64 rsp;          /* saved kernel rsp (points at a regs64_t frame) */
    u64 kstack_top;
    u64 user_base;    /* demo image base (clone inherits for thread_exit) */
    u64 cr3;          /* M5: private PML4 phys (task[0] = boot PML4) */
    int id;
    int state;
    int parent;       /* M5: waiter id (-1 none) */
    int waiting_for;  /* M5: waitpid target (-1 any, -2 none) */
    u64 wakeup_tick;  /* M5: sleep deadline (0 = waitpid-blocked) */
    int exit_code;
    int last_cpu;     /* M7: cpu that last ran it (ps display) */
    u64 heap_base;    /* M7.3: demand-heap [base, brk) */
    u64 heap_brk;
    char name[16];
    u8 fx[512] __attribute__((aligned(16))); /* eager FPU image */
} task64_t;

void task64_init(void);
int task64_spawn_image(const char *kname); /* kernel-resident name */
int task64_clone(u64 func);
int task64_fork(regs64_t *r);
void task64_exit(int status);
u64 task64_waitpid(int pid, u64 status_ptr, int wnohang, regs64_t *r);
u64 task64_sleep(u64 ticks, regs64_t *r);
u64 task64_brk(u64 nw, regs64_t *r);      /* M7.3 */
int task64_demand(u64 va);                /* M7.3, fault context */
u64 task64_kill_fault(regs64_t *r, const char *kind); /* M7.3: exit 139 */
u64 task64_schedule(regs64_t *r);
u64 task64_on_tick(regs64_t *r);
u64 task64_ap_tick(regs64_t *r); /* M7: AP timer (schedule only, no ticks) */
void sched_set_running(void);    /* M7: shootdown gate (mem64.c) */
int task64_current_id(void);
task64_t *task64_self(void); /* current TCB (for base/getpid paths) */
void kbd_init(void);
void kbd_push(u8 sc);
int kbd_try_get(void);
int task64_spawn_args(const char *kname, int argc, char **kargv);
int task64_ps(ps_entry_t *out, int max);
u64 task64_exec(const char *uname, regs64_t *r); /* user-space name pointer */
void exec_register(const char *name, const void *data, u64 len);
const void *exec_lookup(const char *name, u64 *len_out);
void aslr_seed(u64 s); /* M7.3: seed ELF slide PRNG (TSC at boot) */
int loader_map_image(u64 target, const u8 *img, u64 len, u64 *entry_out,
                     u64 *base_out, u64 *end_out);
u64 syscall64_dispatch(regs64_t *r);
/* M7 CR3 forensic ring (mem64.c): every CR3 load/teardown traced; the FATAL
 * PF handler dumps it without touching LAPIC or task state. */
void trace_cr3(char ev, int task, u64 cr3);
void trace_cr3_dump(void);
/* M6 SMP (defined in k64/smp64.c). Test/TLB IPI vectors + spurious. */
#define VEC_IPI_TEST 96
#define VEC_IPI_TLB 97
#define VEC_AP_TIMER 80
#define VEC_SPURIOUS 255
void smp_init(void);
void smp_wake_aps(void); /* M7: release parked APs into the scheduler */
int smp_selftest(void);
int smp_shootdown(u64 va); /* single VA on all APs; ~0ULL = full flush */
void smp_shootdown_all(void);
void smp_halt_others(void); /* NMI-freeze others for a clean fatal dump */
int smp_in_shootdown(void); /* nonzero while this CPU waits out a shoot */
int smp_cpu_by_stack(u64 rsp); /* LAPIC-free stack->cpu guess for dumps */
void lapic_eoi(void);
int smp_cpu_count(void);
int smp_cpu_index(void); /* LAPIC ID -> 0..ncpus-1, -1 unknown */
void smp_ack_test(void);
void smp_ack_tlb(u64 va);
void smp_ack_spurious(void);

/* M3 physical memory + paging (defined in k64/mem64.c). */
void mem64_init(u64 mb_info);
void mem64_selftest(void);
u64 pmm_alloc(void);
void pmm_free(u64 pa);
u64 mem_lock_acquire(void); /* hold across multi-step wiring sequences */
void mem_lock_release(u64 f);
u64 __pmm_alloc(void);      /* mem_lock held by caller */
int __frame_ref_put(u64 pa); /* mem_lock held by caller */
void pmm_free_raw(u64 pa); /* page-table frames: bitmap only, no refcount */
void frame_ref_inc(u64 pa);
int frame_ref_put(u64 pa); /* -1 ref, bitmap-free at 0, returns remainder */
u64 pmm_free_frames(void);
u64 pmm_total_frames(void);
void vmm_set_root(u64 *root); /* mapping root for vmm_map/unmap */
u64 *vmm_get_root(void);
int __vmm_map_page(u64 va, u64 pa, u64 flags); /* caller holds mem_lock */
int __vmm_unmap_page(u64 va);                    /* caller holds mem_lock */
void vmm_publish(void); /* flush APs after wiring (lock-free) */
u64 vmm_clone_space(u64 src_pml4); /* M5: COW-clone an address space */
u64 vmm_share_space(u64 src_pml4); /* M5: table-copy, pages truly shared */
void vmm_teardown_space(u64 pml4_pa); /* M5: free a task's private space */
int vmm_cow_resolve(u64 va); /* M5: resolve a COW write fault, 0 = ok */
int vmm_map_page(u64 va, u64 pa, u64 flags);
int vmm_map_page_in(u64 *root, u64 va, u64 pa, u64 flags); /* M10: explicit root */
int vmm_unmap_page(u64 va);
u64 vmm_translate(u64 va);
u64 __vmm_translate(u64 va); /* caller holds mem_lock, walks live CR3 */
int vmm_probe(u64 va, u64 *flags); /* P|RW|US|(NX as bit3), -1 unmapped */
int vmm_user_ok(u64 va, u64 len);
int vmm_is_canonical(u64 va);
int cpu_nx_enabled(void);
void paging_enable_nxe_ap(void); /* M7: APs need NXE for user NX pages */

/* Flag bits for vmm_map_page callers (translated to PTE bits inside). */
#define VMM_RW  (1ULL << 0)  /* writable (default read-only) */
#define VMM_US  (1ULL << 1)  /* user-accessible (default supervisor) */
#define VMM_NX  (1ULL << 2)  /* no-execute (needs EFER.NXE) */
#define VMM_UC  (1ULL << 3)  /* uncacheable MMIO (PCD|PWT) */

/* Small CPU readers shared by k64 C files. */
static inline u64 cpu_read_efer(void) {
    u32 lo, hi;
    __asm__ __volatile__("mov $0xC0000080, %%ecx\n\trdmsr"
                         : "=a"(lo), "=d"(hi) : : "ecx");
    return ((u64)hi << 32) | lo;
}
static inline u64 cpu_read_cr2(void) {
    u64 v;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(v));
    return v;
}
static inline u64 cpu_read_cr3(void) {
    u64 v;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
    return v;
}
static inline void cpu_load_cr3(u64 cr3) {
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

#endif
