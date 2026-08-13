// --- MECTOV OS v13.5 GUI Edition ---
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

volatile int pending_logout = 0;
volatile int needs_redraw = 1;
static int fps_val = 0;
static int fps_frames = 0;
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

void kernel_main(uint32_t magic, uint32_t addr) {
    extern void init_serial();
    init_serial();
    write_serial_string("[KERNEL] boot start\n");
    
    multiboot_info_t* mbi = (multiboot_info_t*)addr;
    uint32_t fb_p = 0, fb_s = 0;
    uint32_t mem_size = 32 * 1024 * 1024; // Default fallback 32MB

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

    write_serial_string("[K] gdbstub\n");
    extern void gdb_stub_init(void);
    gdb_stub_init();
    
    write_serial_string("[K] apic\n");
    extern void apic_init(void);
    extern void ioapic_init(void);
    apic_init();
    ioapic_init();

    write_serial_string("[K] smp\n");
    extern void smp_init(void);
    smp_init();

    write_serial_string("[K] syscalls\n");
    extern void init_syscalls(void);
    init_syscalls();
    write_serial_string("[K] timer\n");
    init_timer(1000); // 1000 Hz PIT for 1ms precision ticks
    write_serial_string("[K] kbd\n");
    init_keyboard();
    write_serial_string("[K] cpu\n");
    detect_cpu();
    write_serial_string("[K] pci\n");
    pci_scan();
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

    while (1) {
        // Keep the PIT tick rate calibrated against the RTC wall clock: the
        // desktop's double-click window is scaled by ticks_per_sec, and the
        // TCG virtual clock rate can drift, so re-measure once per second.
        extern void timer_update_rate_if_second(void);
        timer_update_rate_if_second();

        // Snapshot mouse state with interrupts off: x/y/btn are written by
        // IRQ12 and reading them separately can tear (x from one update, y
        // from the next), producing a wrong hit-test for a frame.
        int mx, my, btn;
        __asm__ volatile("cli");
        mx  = mouse_x;
        my  = mouse_y;
        btn = (int)(uint32_t)mouse_btn;
        __asm__ volatile("sti");

        if (mx != prev_mx || my != prev_my || btn != prev_btn) {
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
                int sm_h = 348;
                int sm_y = sm_ty - sm_h;
                int in_start_menu = (start_menu_open && mx >= 2 && mx <= 202 && my >= sm_y && my <= sm_ty);
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
                // 200ms cadence.
                wait_for_vsync();
                swap_buffers();
                fps_frames++;
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
            if (sc != 0) {
                // F12 = break into the in-kernel GDB stub (see gdb_stub.c)
                if (sc == 0x58) {
                    extern void gdb_stub_break(void);
                    gdb_stub_break();
                    needs_redraw = 1;
                }
                extern int keyboard_alt_held;
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
                    } else if (sc == 0x0F && keyboard_alt_held) {
                        wm_alt_tab_start();
                        needs_redraw = 1;
                    } else if (sc < 0x80 || sc == 0xE0) {
                        char c = scancode_to_char_mods(sc, kbd_mods);
                        // Single-consumer keyboard (v38.9): the main loop is
                        // the ONLY reader of kbd_buffer. When a foreground app
                        // owns the terminal, keys are queued to its buffer
                        // (SYS_GET_KEY pops them) instead of the focused
                        // window — the terminal must not swallow the app's
                        // keystrokes as its own input. Otherwise forward to
                        // the focused window as usual.
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

        // Answer GDB `target remote` handshake while the OS is running.
        extern void gdb_stub_poll(void);
        gdb_stub_poll();

        extern void net_poll();
        net_poll();

        uint32_t now = get_ticks();
        
        // 1000 Hz timer => 1000 ticks = 1 second
        if (now - last_clock_tick >= 1000) {
            last_clock_tick = now;
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
        if (needs_redraw && (now - last_frame_tick >= frame_interval)) {
            needs_redraw = 0;
            last_frame_tick = now;
            fps_frames++;
            full_redraw();
        }

        // Logout: kembali ke login screen
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

        // CPU friendly halt
        if (get_ticks() == now) {
            __asm__ __volatile__ ("hlt");
        }
    }
}

#include "src/include/syscall.h"

// Removed dummy user task
