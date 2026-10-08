// --- MECTOV OS kernel entry (version: see OS_VERSION in src/include/utils.h) ---
#include "src/include/types.h"
#include "src/include/vga.h"
#include "src/include/keyboard.h"
#include "src/include/speaker.h"
#include "src/include/ata.h"
#include "src/include/vfs.h"
#include "src/include/security.h"
#include "src/include/shell.h"
#include "src/include/mem.h"
#include "src/include/utils.h"
#include "src/include/apps.h"
#include "src/include/io.h"
#include "src/include/idt.h"
#include "src/include/timer.h"
#include "src/include/multiboot.h"
#include "src/include/mouse.h"
#include "src/include/wm.h"
#include "src/include/desktop.h"
#include "src/include/taskbar.h"
#include "src/include/login.h"
#include "src/include/task.h"
#include "src/include/pci.h"
#include "src/include/serial.h"
#include "src/include/gdt.h"

// Forward declaration
extern void init_double_buffer(void);
extern volatile int power_overlay_active;
extern volatile int pending_lock;

volatile int pending_logout = 0;
volatile int needs_redraw = 1;
static int fps_val = 0;
static int fps_frames = 0;
/* v38.146: present-rate readout for the render task (taskbar HUD value =
 * full_redraw+swap completions per second). Logged in the perf line as
 * pres_fps; ~10 bytes per 100 frames, kept permanently for tuning. */
int kernel_present_fps(void) { return fps_val; }

/* v38.139: composite accounting while a window owns the mouse capture.
 *
 * The game task is the one paying for these composites, so it is the one that
 * reads the number, and it is read+cleared like wm_q3_times() — the perf line
 * reports a window, not a run. Counted only while a capture is active: that is
 * the share of the desktop's work that exists *because* the game is on screen.
 * Until this existed, "the desktop is charging me 19% of my frame" was an
 * inference from `other_ms`, a bucket that also holds serial writes and the
 * scheduler's own stolen time. */
static int cap_comp_calls, cap_comp_us;
void kernel_capture_comp_stats(int *calls, int *us) {
    if (calls) *calls = cap_comp_calls;
    if (us)    *us    = cap_comp_us;
    cap_comp_calls = 0;
    cap_comp_us = 0;
}
static uint32_t fps_last_tick = 0;
static uint32_t last_render_us = 0;

void full_redraw() {
    uint32_t start_us = timer_get_us();

    extern void taskbar_pre_draw(void);
    taskbar_pre_draw();

    desktop_draw();
    wm_draw_all();
    taskbar_draw();

    // Text buffer for FPS and Render Time
    char fps_buf[32];
    int fi = 0;
    
    // FPS value
    int tmp = fps_val;
    if (tmp == 0) { fps_buf[fi++] = '0'; }
    else {
        char rev[8]; int rl = 0;
        while (tmp > 0) { rev[rl++] = '0' + tmp % 10; tmp /= 10; }
        while (rl > 0) fps_buf[fi++] = rev[--rl];
    }
    fps_buf[fi++] = ' '; fps_buf[fi++] = 'F'; fps_buf[fi++] = 'P'; fps_buf[fi++] = 'S'; fps_buf[fi++] = ' ';
    fps_buf[fi++] = '|'; fps_buf[fi++] = ' ';
    
    // Render time value
    tmp = last_render_us;
    if (tmp == 0) { fps_buf[fi++] = '0'; }
    else {
        char rev[16]; int rl = 0;
        while (tmp > 0) { rev[rl++] = '0' + tmp % 10; tmp /= 10; }
        while (rl > 0) fps_buf[fi++] = rev[--rl];
    }
    fps_buf[fi++] = ' '; fps_buf[fi++] = 'u'; fps_buf[fi++] = 's';
    fps_buf[fi] = '\0';

    int fx = (int)fb_width - (fi * 8) - 8;
    // Redraw the FPS/render-time HUD only when the value changed (200ms
    // cadence) or the region was damaged this frame (window dragged over it,
    // etc.). Otherwise the copy on VRAM is still valid — skipping the glyph
    // re-render avoids ~2000 put_pixel calls and dirty-rect pollution per frame.
    extern int d_min_x, d_min_y, d_max_x, d_max_y;
    static int last_fps_val = -1;
    int hud_x0 = (int)fb_width - 200;
    int hud_damaged = (d_max_x >= hud_x0 && d_min_x < (int)fb_width &&
                       d_max_y >= 22 && d_min_y < 40);
    if (hud_damaged || last_fps_val != fps_val) {
        last_fps_val = fps_val;
        // Clear a fixed width area (e.g. 200 pixels) to prevent old characters
        // from remaining when the string shrinks in length
        draw_rect(hud_x0, 22, 200, 18, 0x00000000);
        draw_string_px(fx, 23, fps_buf, 0x0000FF00, 0x00000000);
    }

    extern int cursor_draw_x, cursor_draw_y;
    cursor_draw_x = mouse_x;
    cursor_draw_y = mouse_y;
    wait_for_vsync();
    swap_buffers();

    uint32_t end_us = timer_get_us();
    last_render_us = end_us - start_us;
}

/* v38.150: ONE window owns the back buffer at a time.
 *
 * Why this exists, measured. The Q3 frame loop renders at an even 40 Hz
 * (fps=40, idle_ms=1696 per 100 frames = 17 ms of the 25 ms frame spent
 * waiting, so ~9 ms of real work per frame). The swap, however, happened in
 * the DESKTOP loop's capture branch (wm_draw_window + wait_for_vsync +
 * swap_buffers), and that loop only reaches the swap ~29-34 times a second
 * (pres_fps=29..34, comp_s=28..30, comp_ms~320 per 100 frames). So roughly
 * ten rendered frames a second never reached the glass, and the picture
 * advanced in one-or-two-rendered-frame steps: "fps stays 40 but the camera
 * goes patah-patah". Nothing is wrong with the renderer or the input path —
 * wait_for_vsync() is a no-op in this port (0x3DA polling is disabled under
 * QEMU, and it measured 10 us), so there is no vblank beat either. Frames were
 * simply being dropped between render and present.
 *
 * So the game task presents its own frame (see desktop_present_window below),
 * which is what v38.139's comment already said should happen; the swap just
 * never moved out of the desktop loop. Both presenters draw into the SAME
 * back buffer, so they take this lock: whoever holds it owns the back buffer
 * for the duration of draw+swap, and the loser skips (its invalidate still
 * stands, so the desktop loop presents the window a moment later). */
static volatile int present_busy;

/* v38.150: when the game last put its own frame on the glass (µs clock).
 * The desktop loop's capture branch consults this to avoid re-presenting a
 * frame the game task just presented: same pixels, full composite cost
 * (~3 ms here, more on a slow host), twice per frame at worst. A stale
 * timestamp means the game stopped presenting (loading, pause, quit) and the
 * desktop resumes — self-healing, nothing to reset. */
static volatile uint32_t last_game_present_us;

int desktop_present_window(int id) {
    int drew;
    if (id < 0) return 0;
    if (!vga_fullscreen_active() && id != wm_capture_owner()) {
        /* Only the window that owns the screen may be presented alone: without
         * the capture the desktop behind it has to be composed too, or the
         * swap would push a hole in the picture. */
        return 0;
    }
    if (__sync_lock_test_and_set(&present_busy, 1)) return 0;  /* other presenter */
    drew = wm_draw_window(id);
    if (drew) {
        wait_for_vsync();
        swap_buffers();
        fps_frames++;   /* the present counter in this file counts every swap
                         * (the 200 ms window below divides by it), so the
                         * game's own presents must count too — without this
                         * the on-glass FPS number read 4 while the game was
                         * swapping 40 frames a second itself. */
        last_game_present_us = timer_get_us();
    }
    __sync_lock_release(&present_busy);
    return drew;
}

/* v38.137: present the whole screen from the caller's own task, damage
 * included.
 *
 * Why this exists: mark_dirty() only RECORDS damage while the back buffer is
 * the active render target (see vga.c), and swap_buffers() resets the damaged
 * rect every time it runs. So a composite that happens while some window's
 * content buffer is installed, or a swap that lands between a draw and its
 * damage marking, can leave a rectangle unmarked — and "unmarked" means the
 * swap never copies it, so that rectangle keeps whatever was on the glass
 * before. That is the loading screen that stayed on screen for minutes while
 * the game ran behind it (the user's screenshot: `loading 8s` on the glass,
 * `frame 380 drawn=799` in the serial log).
 *
 * Damaging the whole screen first makes the swap unconditional, which is what
 * a caller who is about to be looked at wants. It is the same trick the
 * login/logout/lock paths already use. */
void desktop_present_now(void) {
    mark_dirty(0, 0, (int)fb_width, (int)fb_height);
    needs_redraw = 0;
    full_redraw();
}

/* v38.150: a full compose requested FOR LATER, not now. The game task used to
 * call desktop_present_now() directly once a second, which put a whole
 * full-screen composite (tens of ms on a slow, cluttered desktop) inside its
 * own frame — a metronome hitch folded into "kadang patah". Now it sets this
 * flag and the desktop loop spends its own time on it below. Single writer
 * (the game task, 1 Hz), single reader (the loop); the only race leaves the
 * flag set, which just means one more compose. If the loop never gets around
 * to it, the next tick sets it again — nothing is lost, the chrome is only
 * ever late, never wrong. */
volatile int want_full_present = 0;

/* v38.139: take one mouse packet out of the PS/2 accumulator and hand it to
 * whichever window owns the relative-motion capture.
 *
 * Why this is a function and no longer three lines inside the main loop: the
 * rate this runs at IS the look rate. The game's view angle is
 * `delta_angles + q3vm_cmd_yaw`, and the camera reads it live (v38.143), so the
 * aim only moves when this runs — and until now the only caller was the
 * desktop's idle loop. That tied aiming to the compositor's schedule instead of
 * to the picture: on the player's host (28 fps, frame times 27-44 ms, the 40 Hz
 * pacer permanently late) a slow mouse turn arrived in clumps, which is the
 * "nengok pelan pake mouse patah patah" report. The Q3 frame loop now calls
 * this once per rendered frame, before it builds its usercmd, so the aim is
 * never older than the frame it is drawn into.
 *
 * Two callers cannot lose or double-count a packet: mouse_take_delta() drains
 * with an atomic exchange (xchg, v38.150 — the old cli/read/clear was only
 * atomic on one core, and SMP delivery measured 4x motion), and the wire edge
 * below is a single shared static, so whoever runs first takes the packet and
 * the other one sees zero.
 *
 * Returns the button state it saw — the main loop keeps its own prev_* edge
 * detector in sync with it — or -1 when no window is capturing. */
int desktop_capture_pump(void) {
    extern int wm_capture_owner(void);
    extern int mouse_take_delta(int *dx, int *dy);
    extern int wm_capture_event(int dx, int dy, int btn);
    static int cap_prev_btn = -1;
    uint32_t eflags;
    int btn, cdx = 0, cdy = 0, pinx = 0, piny = 0;

    if (wm_capture_owner() < 0) { cap_prev_btn = -1; return -1; }

    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    btn = (int)(uint32_t)mouse_btn;
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));

    if (mouse_take_delta(&cdx, &cdy) || btn != cap_prev_btn) {
        cap_prev_btn = btn;
        wm_capture_event(cdx, cdy, btn);
    }
    /* The arrow is pinned inside the capturing window and hidden, so this is
     * bookkeeping — but it must happen for whichever task drained last, or a
     * later non-capture hover would resolve from a stale position. */
    if (wm_capture_center(&pinx, &piny)) {
        extern int cursor_draw_x, cursor_draw_y;
        mouse_x = pinx;
        mouse_y = piny;
        cursor_draw_x = pinx;
        cursor_draw_y = piny;
    }
    return btn;
}

/* v38.134: let a long-running kernel task hand the screen a frame.
 *
 * The desktop is composited in the idle loop, and a kernel task that never
 * blocks outranks it for as long as it stays runnable. The Q3 loader is
 * exactly that: ~10 s of Com_Init, and the whole VM/GAME_INIT phase (40 s on
 * the busy host whose session started this release), so the loading screen sat
 * on ONE image for the length of the phase and the wait read as a hang.
 * Measured with monitor screendumps: three shots four seconds apart with ZERO
 * changed pixels, then the texture bar moving, then zero again until the game
 * appeared. wm_invalidate() cannot fix that — the invalidate is what the
 * desktop loop reacts to, and the desktop loop is the thing that is not
 * running. So the loader pumps the composite itself, from the points it owns
 * (one texture, one engine file read), and the screen keeps moving.
 *
 * Same 60 fps ceiling the idle loop uses, and never while the game owns the
 * screen (its fullscreen path presents the frame itself). */
void desktop_pump(void) {
    static uint32_t pump_last_tick;
    extern volatile uint32_t ticks_per_sec;
    extern int vga_fullscreen_active(void);
    uint32_t now, interval;

    if (vga_fullscreen_active()) return;

    now = get_ticks();
    interval = (ticks_per_sec * 16) / 1000;
    if (interval < 1) interval = 1;
    if (now - pump_last_tick < interval) return;
    pump_last_tick = now;

    wm_tick_all();
    /* v38.137: the composite AND the swap come from here now, with the whole
     * screen damaged first (see desktop_present_now above). The old shape
     * cleared the idle loop's flag and drew without it, which is how the
     * loading screen ended up on the glass while the game ran. */
    desktop_present_now();
}

void kernel_main(uint32_t magic, uint32_t addr) {
    extern void init_serial();
    init_serial();
    write_serial_string("[KERNEL] boot start\n");
    
    multiboot_info_t* mbi = (multiboot_info_t*)addr;
    uint32_t fb_p = 0, fb_s = 0;
    uint32_t mem_size = 32 * 1024 * 1024; // Default fallback 32MB

    // Parse the GRUB command line BEFORE paging is enabled: cmdline sits in
    // low physical memory (still identity-mapped at this point). Enables
    // `panic=reboot` (CI: QEMU exits instead of hanging on a kernel panic),
    // `panic_self_test` (deliberate panic once the desktop is up),
    // `wd_self_test` (deliberate AP hard-lockup for the watchdog, v38.64) and
    // `tlb_self_test` (TLB-shootdown machinery check, v38.66).
    extern void panic_parse_cmdline(const char* cmd);
    extern void watchdog_parse_cmdline(const char* cmd);
    extern void vmm_parse_cmdline(const char* cmd);
    if (magic == 0x2BADB002 && mbi != NULL && (mbi->flags & 4) && mbi->cmdline) {
        panic_parse_cmdline((const char*)mbi->cmdline);
        watchdog_parse_cmdline((const char*)mbi->cmdline);
        vmm_parse_cmdline((const char*)mbi->cmdline);
    }

    write_serial_string("1\n");
    if (magic == 0x2BADB002 && mbi != NULL) {
        // Auto-detect RAM size from GRUB Multiboot header
        write_serial_string("2\n");
        if (mbi->flags & 1) {
            // mem_upper is in KB and starts at 1MB
            mem_size = (mbi->mem_upper * 1024) + (1024 * 1024);
        }
        write_serial_string("3\n");

        if (mbi->flags & (1 << 12)) {
            write_serial_string("4\n");
            fb_p = (uint32_t)mbi->framebuffer_addr;
            fb_s = mbi->framebuffer_height * mbi->framebuffer_pitch;
            write_serial_string("5\n");
            init_vbe(fb_p, mbi->framebuffer_width, mbi->framebuffer_height, mbi->framebuffer_pitch, mbi->framebuffer_bpp);
            write_serial_string("6\n");
        }
    }
    write_serial_string("[K] gdt\n");
    extern void init_gdt();
    init_gdt();

    write_serial_string("[K] mem\n");
    init_mem(mem_size);
    write_serial_string("[K] paging\n");
    paging_init(fb_p, fb_s);
    // v38.62: sector-level LRU read cache for the ATA block layer (ext2 /
    // FAT32 / over-pcache reads). Static .bss, usable immediately; the call
    // only prints the banner.
    {
        extern void blkcache_init(void);
        blkcache_init();
    }
    // CR3 is now live: point the #DF task-gate TSS at the kernel page tables
    // so a double-fault task switch can actually run its handler.
    {
        uint32_t cr3;
        __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
        gdt_set_df_cr3(cr3);
    }
    
    write_serial_string("[K] acpi\n");
    extern void acpi_init(void);
    acpi_init();
    
    write_serial_string("[K] idt\n");
    idt_init();

    // Eager FPU/SSE context switching (v38.41): enable the FPU on the BSP
    // and build the clean-state template BEFORE tasking — the scheduler
    // fxsave/fxrstor's on every switch. APs do the same in ap_main().
    write_serial_string("[K] fpu\n");
    extern void fpu_init_cpu(void);
    fpu_init_cpu();

    write_serial_string("[K] gdbstub\n");
    extern int cmdline_nogdb(void);
    if (!cmdline_nogdb()) {
        extern void gdb_stub_init(void);
        gdb_stub_init();
    } else {
        write_serial_string("[GDB] disabled (nogdb)\n");
    }
    
    write_serial_string("[K] apic\n");
    extern void apic_init(void);
    extern void ioapic_init(void);
    apic_init();
    ioapic_init();

    write_serial_string("[K] smp\n");
    extern void smp_init(void);
    smp_init();

    // CI self-test: booted with `panic_self_test`, fire a deliberate kernel
    // panic here — AFTER every AP is awake and has loaded the shared IDT (so
    // the NMI handler is live on all 4 cores) but BEFORE the blocking
    // gui_login(), which would otherwise never return in a headless boot.
    // Exercises the multi-core register dump + panic=reboot end to end.
    extern void panic_self_test_tick(void);
    panic_self_test_tick();

    write_serial_string("[K] syscalls\n");
    extern void init_syscalls(void);
    init_syscalls();
    write_serial_string("[K] entropy\n");
    extern void entropy_init(void);
    entropy_init();
    write_serial_string("[K] timer\n");
    init_timer(TIMER_HZ); // 100 Hz PIT (Linux-HZ style); timer_ticks counts ms
    write_serial_string("[K] kbd\n");
    init_keyboard();
    write_serial_string("[K] cpu\n");
    detect_cpu();
    write_serial_string("[K] pci\n");
    pci_scan();
    // Bus-mastering DMA for the IDE controller (v38.26): detect the BMIDE
    // BAR + enable bus mastering, and route the controller's completion
    // interrupt (IRQ14/15 -> INT 46/47) to the ATA driver. Must run after
    // pci_scan and after the PIC/IOAPIC routing above.
    write_serial_string("[K] ata_dma\n");
    extern void ata_dma_init(void);
    ata_dma_init();
    extern void register_interrupt_handler(uint8_t n, isr_t handler);
    extern void ata_dma_irq_primary(registers_t*);
    extern void ata_dma_irq_secondary(registers_t*);
    register_interrupt_handler(46, ata_dma_irq_primary);
    register_interrupt_handler(47, ata_dma_irq_secondary);
    // SATA (v38.50): bring up any AHCI controller next to the IDE one. Ports
    // with disks become drives 4+ on the same sector API, so ext2/FAT32 and
    // mount() work on SATA volumes unchanged. Absent controller = log only.
    write_serial_string("[K] ahci\n");
    extern void ahci_init(void);
    ahci_init();
    // USB 3.0 (v38.56): any xHCI controller + attached mass-storage. BOT
    // units become drives 8+ on the same sector API — `mount /usb fat32 8`
    // works on a USB stick exactly like the SATA flow. Absent controller
    // or empty ports = log only. Must run after pci_scan and ahci_init.
    write_serial_string("[K] xhci\n");
    extern void xhci_init(void);
    xhci_init();
    // VirtIO-Blk (v38.78): transitional/legacy PCI block disks become
    // drives 12+ on the same sector API (`mount /vblk fat32 12`). Absent
    // device = log only. Must run after pci_scan (shares nothing with
    // ahci/xhci — own queue memory, own lock, poll-only so no IRQ wiring).
    write_serial_string("[K] virtio\n");
    extern void virtio_blk_init(void);
    virtio_blk_init();
    // VirtIO-GPU (v38.115): a MODERN virtio-pci device — the legacy interface
    // virtio_blk drives above does not exist for the gpu (measured: no I/O BAR
    // at all, even with disable-modern=on). Optional device: present only when
    // the run attaches one (MECTOV_GPU=1 ./run.sh, scripts/virtiogpu_test.py),
    // and absent is a one-line log. Poll-only, own static rings, no IRQ.
    extern void virtio_gpu_init(void);
    virtio_gpu_init();
    write_serial_string("[K] sb16\n");
    extern void sb16_init(void);
    sb16_init();
    write_serial_string("[K] rtl\n");
    extern void init_rtl8139();
    init_rtl8139();
    write_serial_string("[K] net\n");
    extern void net_init();
    net_init();
    // IRQ-driven RX: wire the RTL8139 IRQ (INT 43) to the network layer.
    // net_poll() stays as a fallback for shell busy-waits.
    extern void register_interrupt_handler(uint8_t n, isr_t handler);
    extern void net_irq_handler(registers_t*);
    register_interrupt_handler(43, net_irq_handler);
    // init_serial already called at top of kernel_main
    write_serial_string("[K] uptime\n");
    init_uptime();
    write_serial_string("[K] vfs\n");
    vfs_init();

    write_serial_string("[K] clipboard\n");
    extern void clipboard_init(void);
    clipboard_init();

    write_serial_string("[K] dbuf\n");
    init_double_buffer();
    write_serial_string("[K] tasking\n");
    init_tasking();

    __asm__ __volatile__ ("sti");
    
    write_serial_string("[K] sti done\n");

    // Removed dummy task creation

    // Calibrate the PIT tick rate against the CMOS RTC (wall clock). Under
    // QEMU TCG the emulated timer runs faster than real time, so the desktop's
    // double-click window (800 ms in ticks) must be scaled or it collapses to
    // a fraction of a second — GUI timeouts would miss under TCG / CI.
    write_serial_string("[K] cal\n");
    extern void timer_calibrate_ticks_per_sec(void);
    timer_calibrate_ticks_per_sec();

    // v38.64 watchdog self-test: booted with `wd_self_test`, hang the first
    // AP with a directed fixed IPI (it cli-spins forever). The BSP's timer
    // watchdog — live since init_timer — must detect the stall ~3 s later,
    // NMI-dump every core (the hung AP answers from inside its spin) and
    // reboot via `panic=reboot`. With QEMU -no-reboot the reset exits CI
    // cleanly instead of hanging until the workflow timeout.
    extern void watchdog_self_test_tick(void);
    watchdog_self_test_tick();

    // v38.66 TLB-shootdown self-test: booted with `tlb_self_test`, park the
    // first AP and verify the shootdown IPI handler's conditional CR3 reload
    // + ack protocol (matching page_dir reloads, non-matching skips), then
    // panic_finish() to exit CI cleanly. All APs are up at this point.
    extern void vmm_self_test_tick(void);
    vmm_self_test_tick();

    write_serial_string("[K] mouse\n");
    init_mouse();
    write_serial_string("[K] startup_logo\n");
    draw_startup_logo();
    write_serial_string("[K] nada\n");
    nada(440, 150); nada(523, 150); nada(659, 300);

    write_serial_string("[K] wm\n");
    wm_init();
    cursor_saved_x = -1;
    write_serial_string("[K] login\n");
    gui_login();
    
    write_serial_string("BOOTED KERNEL LOOP\n");

    extern int load_mct_app(const char*);

    nada(659, 80); nada(784, 80); nada(1047, 150);
    mark_dirty(0, 0, fb_width, fb_height);
    full_redraw();
    
    // Kalkulator akan dibuka jika user mengklik ikonnya di desktop

    // extern int load_mct_app(const char*);
    // load_mct_app("gcalc.mct");
    
    // Auto test browser.mct to capture issue
    // load_mct_app("apps/browser.mct");

    // ---- Main GUI Event Loop ----
    int prev_btn  = 0;
    int prev_mx   = mouse_x, prev_my = mouse_y;
    uint32_t last_clock_tick = 0;
    uint32_t last_frame_tick = 0;
    // Ring 3 scanout handback detector: set when fb_scanout_active() was true
    // last iteration and is false now (release / exit / exec of the owner).
    int fb_was_active = 0;

    while (1) {
        // USB HID (v38.60): drain xHC interrupt-IN reports into the shared
        // keyboard scancode queue + mouse state BEFORE the frame reads input
        // below. No-op (a compare + a couple of reads) when there is no xHCI
        // controller or no HID device, so the PS/2-only boot is unaffected.
        extern void xhci_hid_poll(void);
        xhci_hid_poll();

        // v38.55: drain deferred sem/futex waiter cleanups for dead tasks.
        // Teardown only sets a pending bit (see sync.c for why it cannot take
        // sync_lock under task_lock); the BSP main loop is the lock-free
        // context that performs the actual removal, well under a millisecond
        // after death.
        extern void sync_drain_pending(void);
        sync_drain_pending();

        // v38.61: periodic write-back. Dirty file-backed mmap pages are the
        // only write-behind data in Mectov, so this bounds power-cut loss to
        // one interval even when no app ever calls msync/fsync/sync. Runs
        // every ~5 s in this lock-free main-loop context (the flush itself
        // takes task_lock internally). Task 0's own mapping can never be
        // dirty, and a region whose owner is running elsewhere is skipped and
        // retried next interval.
        {
            static uint32_t last_writeback_tick = 0;
            extern int task_sync_all(void);
            uint32_t now_wb = get_ticks();
            if (now_wb - last_writeback_tick >= 5000) {
                last_writeback_tick = now_wb;
                task_sync_all();
            }
        }

        // Keep the PIT tick rate calibrated against the RTC wall clock: the
        // desktop's double-click window is scaled by ticks_per_sec, and the
        // TCG virtual clock rate can drift, so re-measure once per second.
        extern void timer_update_rate_if_second(void);
        timer_update_rate_if_second();

        // ---- Ring 3 scanout takeover ----
        // While a compositor holds the framebuffer (SYS_FB_MAP), the desktop
        // stops presenting frames entirely: no full_redraw, no cursor, no WM
        // input processing. The holder paints every pixel from Ring 3 and
        // reads keystrokes via SYS_GET_KEY (pumped below) plus raw mouse
        // state via SYS_GET_MOUSE. Kernel duties that are not pixel-bound
        // keep running here so the takeover cannot starve them.
        extern int fb_scanout_active(void);
        int fb_active = fb_scanout_active();
        if (fb_was_active && !fb_active) {
            // Handback: one forced repaint brings the desktop back.
            write_serial_string("[FBMAP] desktop restored\n");
            needs_redraw = 1;
            prev_mx = -1; prev_my = -1; prev_btn = -1;   // force cursor redraw
        }
        fb_was_active = fb_active;

        if (fb_active) {
            // Pump resolved keystrokes to the scanout holder's queue
            // (SYS_GET_KEY pops them). Same feed-time modifier snapshot as
            // the normal path; Ctrl+C/Z combos are deliberately NOT
            // intercepted during a takeover — the compositor sees the chars.
            uint8_t kbd_mods_fb = 0;
            uint8_t sc_fb = k_get_scancode_ex(&kbd_mods_fb);
            if (sc_fb != 0) {
                extern void security_note_input(void);
                security_note_input();
                if ((kbd_mods_fb & 2) && !(kbd_mods_fb & 1) && sc_fb == 0x2E) {
                    // Ctrl+C still interrupts the foreground job even while
                    // it owns the screen — killing a wedged compositor must
                    // not require another console.
                    extern int task_get_fg_pgrp(void);
                    extern int task_signal_pgrp(int pgrp, int sig);
                    int fg_pgrp = task_get_fg_pgrp();
                    if (fg_pgrp > 0) task_signal_pgrp(fg_pgrp, SIGINT);
                } else if (sc_fb < 0x80 || sc_fb == 0xE0) {
                    char c_fb = scancode_to_char_mods(sc_fb, kbd_mods_fb);
                    extern void term_app_push_key(unsigned char);
                    if (c_fb != 0) term_app_push_key((unsigned char)c_fb);
                }
            }
            extern void gdb_stub_poll(void);
            gdb_stub_poll();
            extern void net_poll();
            net_poll();
            uint32_t now_fb = get_ticks();
            if (now_fb - last_clock_tick >= 1000) {
                last_clock_tick = now_fb;
                extern void security_auto_lock_tick(uint32_t);
                security_auto_lock_tick(now_fb);
                extern void task_reap_zombies(void);
                task_reap_zombies();
            }
            __asm__ __volatile__ ("hlt");
            continue;
        }

        // Snapshot mouse state with interrupts off: x/y/btn are written by
        // IRQ12 and reading them separately can tear (x from one update, y
        // from the next), producing a wrong hit-test for a frame.
        int mx, my, btn;
        __asm__ volatile("cli");
        mx  = mouse_x;
        my  = mouse_y;
        btn = (int)(uint32_t)mouse_btn;
        __asm__ volatile("sti");

        // ---- v38.103: game mouse capture ----
        // While a window owns relative mouse motion (wm_capture_mouse), the
        // desktop stops resolving hover/drag/resize/taskbar/icon hits from the
        // absolute cursor: raw packet deltas go to that window's mouse_fn and
        // the cursor is pinned inside it (and hidden by vga.c). Without this
        // branch a crosshair would stop at the screen edge and hovering would
        // never generate a single mouse event.
        //
        // v38.139: the drain itself moved into desktop_capture_pump() (above)
        // because the Q3 frame loop is now a second caller — it must be, or the
        // aim only moves as often as this loop happens to run.
        extern int wm_capture_owner(void);
        int cap_id = wm_capture_owner();
        int cap_btn = desktop_capture_pump();
        if (cap_btn >= 0) {
            mouse_scroll = 0;   // wheel is not routed while captured
            prev_btn = cap_btn; prev_mx = mx; prev_my = my;
        } else if (mx != prev_mx || my != prev_my || btn != prev_btn) {
            // Idle auto-lock (v38.51): any mouse activity restarts the
            // `locktimeout` countdown.
            extern void security_note_input(void);
            security_note_input();
            extern int cursor_draw_x, cursor_draw_y;
            cursor_draw_x = mx;
            cursor_draw_y = my;

            if (mx != prev_mx || my != prev_my) {
                mark_dirty(prev_mx, prev_my, 24, 24);
                mark_dirty(mx, my, 24, 24);
                // Track titlebar button hover on pure mouse moves (no button)
                if (!(btn & 1)) {
                    extern void wm_track_mouse(int, int);
                    wm_track_mouse(mx, my);
                    // Taskbar hover (Start button + window buttons + Start
                    // menu items) — was dead code before v38.39: this function
                    // was never called, so buttons never highlighted.
                    extern void taskbar_track_mouse(int, int, int, int);
                    taskbar_track_mouse(mx, my, prev_mx, prev_my);
                }
            }

            int in_taskbar = (my >= (int)fb_height - TASKBAR_H_PX);
            // Check if any taskbar popup is open (volume, calendar, start menu)
            extern int start_menu_open;
            extern int calendar_open;
            int popup_open = start_menu_open || calendar_open || taskbar_volume_popup_open();
            
            int handled = 0;
            extern int alt_tab_active;

            // Dismiss popups on ANY click outside their area (press, not just release)
            // This must run BEFORE wm_handle_mouse to prevent the WM from eating the event
            if (popup_open && (btn & 1) && !(prev_btn & 1) && !in_taskbar) {
                // Mouse press on desktop area while popup open → close all popups
                int sm_ty = (int)fb_height - TASKBAR_H_PX;
                int sm_h = START_MENU_H;
                int sm_y = sm_ty - sm_h;
                int in_start_menu = (start_menu_open && mx >= 2 && mx <= 2 + SM_W && my >= sm_y && my <= sm_ty);
                if (!in_start_menu) {
                    // Click is outside all popups - close them
                    if (start_menu_open) { start_menu_open = 0; needs_redraw = 1; }
                    if (calendar_open) { calendar_open = 0; needs_redraw = 1; }
                    extern int taskbar_volume_popup_open(void);
                    // Note: volume_popup_open is static, handled by taskbar_handle_click
                }
            }

            if (alt_tab_active) {
                if (btn != prev_btn) {
                    handled = 1;
                } else {
                    handled = 0;
                }
            } else {
                handled = wm_handle_mouse(mx, my, btn, prev_btn);
                if (!handled) {
                    extern int desktop_drag_active(void);
                    if (in_taskbar && !btn && prev_btn && !desktop_drag_active()) {
                        taskbar_handle_click(mx, my);
                    } else if (!in_taskbar && !btn && prev_btn && popup_open) {
                        // Route release above taskbar to taskbar if a popup was open
                        taskbar_handle_click(mx, my);
                    } else if (!in_taskbar || desktop_drag_active()) {
                        // A live icon drag keeps its events even over the taskbar
                        desktop_handle_mouse(mx, my, btn, prev_btn);
                    }
                }
            }
            
            // Always trigger full redraw if needs_redraw was set by any handler
            // (desktop_handle_mouse sets it during icon drag, wm sets it during window drag, etc.)
            if (btn != prev_btn || handled || needs_redraw) {
                needs_redraw = 1;
            } else {
                // Pure mouse move with no state change: just update cursor on VRAM.
                // This is a real presented frame — count it so the FPS HUD shows
                // the live rate while the cursor moves instead of the HUD's own
                // 200ms cadence. Not while a game owns the present path: the
                // cursor is hidden then (mouse captured), and swapping would
                // push the game's half-drawn frame.
                //
                // v38.150: ...and that "not while" was only a comment. The code
                // below it checked vga_fullscreen_active() but never the
                // capture, so every pure-motion event swapped while a game held
                // the mouse: up to ~100 wasted swaps/s (each counted in
                // pres_fps, which is how a 32 fps game reported pres_fps=92),
                // each one racing the game task's own present and capable of
                // landing mid-frame — judder exactly while the mouse moves.
                // Skip it whenever any window holds the capture; the capturer
                // presents for itself, and there is no cursor to draw anyway.
                extern int vga_fullscreen_active(void);
                extern int wm_capture_owner(void);
                if (!vga_fullscreen_active() && wm_capture_owner() < 0) {
                    wait_for_vsync();
                    swap_buffers();
                    fps_frames++;
                }
            }
            prev_btn = btn; prev_mx = mx; prev_my = my;
        }

        // ---- Scroll wheel handling ----
        {
            int8_t scroll = mouse_scroll;
            if (scroll != 0) {
                mouse_scroll = 0; // consume
                extern int alt_tab_active;
                if (!alt_tab_active) {
                    wm_handle_scroll(mx, my, (int)scroll);
                }
                needs_redraw = 1;
            }
        }

        extern volatile int doom_fullscreen;
        if (!doom_fullscreen) {
            // Resolve the character against the modifier snapshot captured at
            // feed time: the live shift_p may already have the shift RELEASE
            // applied (the whole shift-<key> sequence is fed in one IRQ), so
            // reading it here turns "shift-7" into '7'. See keyboard.c.
            uint8_t kbd_mods = 0;
            uint8_t sc = k_get_scancode_ex(&kbd_mods);
            // Idle auto-lock (v38.51): any key press (incl. auto-repeat and
            // the Ctrl+Alt+L combo itself) restarts the countdown.
            if (sc != 0) {
                extern void security_note_input(void);
                security_note_input();
            }
            // Windowed DOOM (v38.29): while the DOOM window holds focus, every
            // raw scancode — press AND release — goes straight to DOOM's key
            // queue. The normal character path below only forwards press
            // scancodes and deliberately drops releases, which would leave
            // DOOM's keys stuck down. Legacy fullscreen DOOM never reaches
            // here: it sets doom_fullscreen, which skips this whole block.
            extern int doom_window_has_focus(void);
            if (sc != 0 && doom_window_has_focus()) {
                extern void doom_handle_scancode(uint8_t);
                doom_handle_scancode(sc);
            } else if (sc != 0) {
                // F12 = break into the in-kernel GDB stub (see gdb_stub.c)
                if (sc == 0x58) {
                    extern void gdb_stub_break(void);
                    gdb_stub_break();
                    needs_redraw = 1;
                }
                extern int alt_tab_active;
                extern void wm_alt_tab_start(void);
                extern void wm_alt_tab_next(void);
                extern void wm_alt_tab_end(void);

                if (alt_tab_active) {
                    if (sc == 0x0F) { // Tab press: cycle
                        wm_alt_tab_next();
                        needs_redraw = 1;
                    } else if (sc == 0xB8) { // Alt release: select
                        wm_alt_tab_end();
                        needs_redraw = 1;
                    } else if (sc == 0x01) { // Escape press: cancel without focus change
                        alt_tab_active = 0;
                        extern void mark_dirty(int, int, int, int);
                        mark_dirty(0, 0, fb_width, fb_height); // Erase HUD card cleanly
                        needs_redraw = 1;
                    } else if (sc == 0x1C) { // Enter press: select
                        wm_alt_tab_end();
                        needs_redraw = 1;
                    }
                } else {
                    // Ctrl+C (no Shift) interrupts the terminal's foreground
                    // app (SIGINT to the whole job tree). The scancode is 0x2E
                    // for 'C'; the key is consumed here so it never reaches the
                    // focused window as a normal character. Ctrl+Shift+C is
                    // deliberately NOT consumed — it falls through to the
                    // window so the terminal can use it for clipboard copy.
                    if ((kbd_mods & 2) && !(kbd_mods & 1) && sc == 0x2E) {
                        extern int term_app_running;
                        extern int term_app_task_id;
                        extern int task_signal_group(int root_tid, int sig);
                        extern int task_get_fg_pgrp(void);
                        extern int task_signal_pgrp(int pgrp, int sig);
                        if (term_app_running && term_app_task_id > 0) {
                            // Snapshot the tid: signalling the job can reap it
                            // (which resets term_app_task_id to -1), and the log
                            // below must report the tid we actually sent to.
                            int fg_tid = term_app_task_id;
                            // Fase 2: signal the foreground PROCESS GROUP, not
                            // a task subtree — the app and every task that
                            // inherited its pgrp (pipeline sides, children)
                            // get SIGINT together.
                            int fg_pgrp = task_get_fg_pgrp();
                            int n = (fg_pgrp > 0) ? task_signal_pgrp(fg_pgrp, SIGINT)
                                                  : task_signal_group(fg_tid, SIGINT);
                            write_serial_string("[JOBS] Ctrl+C -> SIGINT to fg pgrp ");
                            write_serial_hex(fg_pgrp);
                            write_serial_string(" (tid ");
                            write_serial_hex(fg_tid);
                            write_serial_string(", ");
                            write_serial_hex(n);
                            write_serial_string(" tasks)\n");
                        } else {
                            // No foreground app: classic ^C — cancel whatever
                            // was being typed. Delivered as a key event to the
                            // focused window; the terminal prints "^C" and
                            // clears its input line.
                            extern void push_event(int, int, int, int, int);
                            extern int wm_focused;
                            if (wm_focused >= 0) push_event(wm_focused, 2, 0, 0, 3);
                            write_serial_string("[JOBS] Ctrl+C -> ^C (no fg app)\n");
                        }
                        needs_redraw = 1;
                    } else if ((kbd_mods & 2) && !(kbd_mods & 1) && sc == 0x2C) {
                        // Ctrl+Z (no Shift) suspends the terminal's foreground app
                        // (SIGTSTP to the whole job tree). Unlike Ctrl+C the
                        // job survives: it is registered as a stopped job so
                        // `jobs` lists it and `bg`/`fg` can resume it. The
                        // scancode is 0x2C for 'Z'; the key is consumed here
                        // so it never reaches the focused window.
                        extern int term_app_running;
                        extern int term_app_task_id;
                        extern int task_signal_group(int root_tid, int sig);
                        extern int task_get_fg_pgrp(void);
                        extern int task_signal_pgrp(int pgrp, int sig);
                        extern int shell_register_stopped_job(int tid);
                        if (term_app_running && term_app_task_id > 0) {
                            int fg_tid = term_app_task_id;
                            int fg_pgrp = task_get_fg_pgrp();
                            int n = (fg_pgrp > 0) ? task_signal_pgrp(fg_pgrp, SIGTSTP)
                                                  : task_signal_group(fg_tid, SIGTSTP);
                            write_serial_string("[JOBS] Ctrl+Z -> SIGTSTP to fg pgrp ");
                            write_serial_hex(fg_pgrp);
                            write_serial_string(" (tid ");
                            write_serial_hex(fg_tid);
                            write_serial_string(", ");
                            write_serial_hex(n);
                            write_serial_string(" tasks)\n");
                            // The app is suspended, not dead: hand control back
                            // to the terminal so the user can type jobs/bg/fg.
                            term_app_running = 0;
                            term_app_task_id = -1;
                            extern void term_app_key_clear(void);
                            term_app_key_clear();
                            shell_register_stopped_job(fg_tid);
                        }
                        needs_redraw = 1;
                    } else if ((kbd_mods & 6) == 6 && sc == 0x26) {
                        // Ctrl+Alt+L: lock the desktop (same as Start menu ->
                        // Lock / the `lock` shell command). Consumed here so it
                        // never reaches the focused window as 'L'.
                        pending_lock = 1;
                        needs_redraw = 1;
                    } else if (sc == 0x0F && (kbd_mods & 4)) {
                        wm_alt_tab_start();
                        needs_redraw = 1;
                    } else if (wm_scancode_focus() >= 0) {
                        // v38.103: a game window that asked for raw scancodes
                        // (wm_request_scancodes) gets press AND release here —
                        // the press-only character path below would leave its
                        // movement keys stuck down and could never deliver ESC,
                        // arrows or ctrl. Ctrl+C/Z and Ctrl+Alt+L were already
                        // consumed above, so the desktop stays recoverable.
                        char cs = (sc < 0x80) ? scancode_to_char_mods(sc, kbd_mods) : 0;
                        wm_handle_scancode(sc, cs);
                        needs_redraw = 1;
                    } else if (sc < 0x80 || sc == 0xE0) {
                        char c = scancode_to_char_mods(sc, kbd_mods);
                        // Start menu search-as-you-type (v38.40): while the
                        // menu is open, printable keys, Backspace, Escape and
                        // Enter drive the menu's filter/launch instead of the
                        // focused window.
                        extern int start_menu_open;
                        if (start_menu_open) {
                            extern void taskbar_handle_key(int, char);
                            taskbar_handle_key((int)sc, c);
                            needs_redraw = 1;
                        } else {
                            // Single-consumer keyboard (v38.9): the main loop
                            // is the ONLY reader of kbd_buffer. When a
                            // foreground app owns the terminal, keys are
                            // queued to its buffer (SYS_GET_KEY pops them)
                            // instead of the focused window — the terminal
                            // must not swallow the app's keystrokes as its
                            // own input. Otherwise forward to the focused
                            // window as usual.
                            extern int term_app_running;
                            extern int term_app_task_id;
                            extern void term_app_push_key(unsigned char);
                            if (term_app_running && term_app_task_id > 0) {
                                if (c != 0) term_app_push_key((unsigned char)c);
                            } else {
                                wm_handle_key(c, sc);
                            }
                            needs_redraw = 1;
                        }
                    }
                }
            }
        }

        // Answer GDB `target remote` handshake while the OS is running.
        extern void gdb_stub_poll(void);
        gdb_stub_poll();

        extern void net_poll();
        net_poll();

        uint32_t now = get_ticks();
        
        // 1000 Hz timer => 1000 ticks = 1 second
        if (now - last_clock_tick >= 1000) {
            last_clock_tick = now;
            // Idle auto-lock (v38.51): 1 s granularity check next to the
            // clock tick — fires pending_lock after `locktimeout` seconds of
            // no keyboard/mouse input.
            extern void security_auto_lock_tick(uint32_t);
            security_auto_lock_tick(now);
            wm_tick_all();
            // Reap zombie processes whose parent never waited for them
            // (safety net; waitpid() reaps them normally when it is used).
            extern void task_reap_zombies(void);
            task_reap_zombies();
            mark_dirty((int)fb_width - 240, (int)fb_height - TASKBAR_H_PX, 240, TASKBAR_H_PX);
            needs_redraw = 1;
        }

        // Update FPS counter every 200ms (real-time responsive feel).
        // elapsed is in ticks, so convert to real seconds via the measured
        // tick rate (ticks_per_sec drifts under TCG — see timer.c). When the
        // window presented nothing new (static desktop) keep the last reading
        // instead of decaying to the HUD's own 200ms cadence, and only force
        // a redraw when the number actually changed.
        if (now - fps_last_tick >= 200) {
            uint32_t elapsed_ticks = now - fps_last_tick;
            int new_val = fps_val;
            if (fps_frames > 0 && elapsed_ticks > 0 && ticks_per_sec > 0) {
                // 32-bit safe: fps_frames*ticks_per_sec << 2^32 in practice.
                new_val = (int)((fps_frames * ticks_per_sec) / elapsed_ticks);
            }
            fps_frames = 0;
            fps_last_tick = now;
            if (new_val != fps_val) {
                fps_val = new_val;
                needs_redraw = 1;
            }
        }

        // Limit composition rate to maximum 60 FPS (16ms per frame) to prevent
        // CPU choking. Scale the tick window by the measured rate so the cap is
        // a real 60 FPS in wall time even when the PIT drifts under TCG (a raw
        // 16 ticks at 549 ticks/s would cap at ~34 FPS).
        extern volatile uint32_t ticks_per_sec;
        uint32_t frame_interval = (ticks_per_sec * 16) / 1000;
        if (frame_interval < 1) frame_interval = 1;
        /* v38.131: per-window tick callbacks. Nothing called this before —
         * tick_fn existed in the WM since v38.103 but had no driver — and the
         * q3arena boot window uses it to animate its loading progress while
         * the world builds (its draw fn runs only when needs_redraw fires,
         * and nothing else invalidates a window that has no frames yet). */
        wm_tick_all();
        /* v38.150: deferred full compose (see want_full_present). The game
         * task asks for it once a second instead of paying for it inside its
         * own frame; doing it here spends desktop time, not frame time. Runs
         * even when the freshness skip below would otherwise skip everything,
         * because chrome (taskbar clock, corner readout, terminal text) has
         * no other refresh path while a game presents for itself. */
        if (want_full_present) {
            want_full_present = 0;
            desktop_present_now();
        }
        if (needs_redraw && (now - last_frame_tick >= frame_interval)) {
            /* v38.150: while a capturing game presents for itself, this whole
             * composite is redundant — the glass already shows this content
             * (see last_game_present_us, stamped on every present). Skipping
             * it here (not just in the capture branch below) is what stops the
             * ~2x present inflation (pres_fps reading 68-110 for a 38 fps
             * game) and hands the saved milliseconds back to the frame that
             * is actually rendering. Stale timestamp (>100 ms) or no capture
             * resumes normal compositing, so a stalled game, a closed window
             * or plain desktop use never notice this branch exists. Chrome
             * (taskbar clock, corner readout) refreshes through the game's
             * own 1 Hz desktop_present_now. */
            extern uint32_t timer_get_us(void);
            extern volatile uint32_t last_game_present_us;
            extern int wm_capture_owner(void);
            if (last_game_present_us != 0 &&
                timer_get_us() - last_game_present_us < 100000u &&
                wm_capture_owner() >= 0) {
                /* fresh — the glass is current. Do NOT consume needs_redraw:
                 * other windows may have dirtied it meanwhile, and dropping
                 * their request would lose (not delay) their update. It stays
                 * set until a real composite consumes it. */
                last_frame_tick = now;
            } else {
            needs_redraw = 0;
            last_frame_tick = now;

            fps_frames++;
            /* v38.119: while a game owns the present path (q3arena's
             * fullscreen mode, vga_fullscreen_enter), the desktop is not
             * composited at all — desktop_draw + wm_draw_all + taskbar_draw
             * for a screen nobody is looking at is pure guest CPU, and it is
             * the same single core the game renders on. The game has already
             * written its frame into the back buffer, so presenting it is
             * exactly the swap the desktop would have ended with. */
            extern int vga_fullscreen_active(void);
            if (vga_fullscreen_active()) {
                wait_for_vsync();
                swap_buffers();
            } else if (cap_id >= 0) {
                /* v38.139: a window owns the capture, so the only thing on this
                 * screen that has to move is that window — and it is the window
                 * the game is rendering into, so stealing less of the frame
                 * here goes straight back to the game's frames.
                 *
                 * Measured on the player's own 11:20 session: other_ms med 665
                 * per 100 frames (6.7 ms of a 35 ms frame, ~19%) with the
                 * desktop compositing the whole screen ~30x a second — the
                 * background fill, every other window and the taskbar — none of
                 * which is moving while a game holds the capture. draw_one()
                 * blits the game's content buffer into the back buffer and
                 * damages its own rectangle, and the swap below then copies
                 * exactly that rectangle.
                 *
                 * Nothing is lost: the game's own 1 Hz desktop_present_now()
                 * still does a full compose, which is what keeps the taskbar
                 * clock and the corner readout live, and the fallback below
                 * covers the window disappearing mid-frame. */
                int t_c0 = (int)timer_get_us();
                /* v38.150: the game task presents its own frame now (see
                 * desktop_present_window), and it draws into the same back
                 * buffer, so this path goes through the one lock inside that
                 * function. Returns 0 in two cases: the window vanished (then
                 * the desktop needs a full compose), or the game task is inside
                 * the back buffer right now (then it is already presenting this
                 * very window and a full compose would only steal its frame).
                 * (The outer freshness check above already skipped the case
                 * where the glass is current, so reaching here means a real
                 * composite is due.) */
                if (!desktop_present_window(cap_id)) {
                    extern int wm_is_open(int id);
                    if (!wm_is_open(cap_id)) full_redraw();
                }
                cap_comp_calls++;
                cap_comp_us += (int)timer_get_us() - t_c0;
            } else {
                full_redraw();
            }
            } /* !game_fresh */
        }

        // Logout: kembali ke login screen (session di-reset)
        if (pending_logout) {
            pending_logout = 0;
            start_menu_open = 0;
            calendar_open = 0;
            extern void wm_reset_session(void);
            wm_reset_session();
            gui_login();
            mark_dirty(0, 0, fb_width, fb_height);
            full_redraw();
        }

        // Lock screen: jalankan login gate TANPA reset session — windows &
        // task tetap hidup di belakangnya, dan full_redraw() memulihkan
        // desktop saat unlock.
        if (pending_lock) {
            pending_lock = 0;
            start_menu_open = 0;
            calendar_open = 0;
            gui_login();
            // Restart the idle countdown after unlocking, so an armed
            // `locktimeout` does not re-lock the freshly-unlocked desktop
            // (the password keystrokes happen inside gui_login, outside the
            // main-loop input hooks).
            extern void security_reset_idle(void);
            security_reset_idle();
            mark_dirty(0, 0, fb_width, fb_height);
            full_redraw();
        }

        // CPU friendly halt — UNCONDITIONAL. Every IRQ (timer tick included)
        // wakes the core, so no wakeup is ever lost: flags set by handlers
        // are re-checked at the top of the next iteration. The old
        // same-tick guard only served the 1 kHz era and starved hlt
        // entirely whenever the (TCG) clock ran faster than wall time,
        // spinning a full core at idle.
        __asm__ __volatile__ ("hlt");
    }
}

#include "src/include/syscall.h"

// Removed dummy user task
