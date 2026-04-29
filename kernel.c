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

// Forward declaration
extern void init_double_buffer(void);

static int fps_val = 0;
static int fps_frames = 0;
static uint32_t fps_last_tick = 0;
static uint32_t last_render_us = 0;

static void full_redraw() {
    uint32_t start_us = timer_get_us();

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
    draw_rect(fx - 4, 22, fi * 8 + 8, 18, 0x00000000);
    draw_string_px(fx, 23, fps_buf, 0x0000FF00, 0x00000000);

    draw_mouse_cursor(mouse_x, mouse_y);
    
    // Mark the whole screen dirty since full_redraw redraws everything
    mark_dirty(0, 0, fb_width, fb_height);
    
    wait_for_vsync();
    swap_buffers();

    uint32_t end_us = timer_get_us();
    last_render_us = end_us - start_us;
}

void kernel_main(uint32_t magic, uint32_t addr) {
    multiboot_info_t* mbi = (multiboot_info_t*)addr;
    uint32_t fb_p = 0, fb_s = 0;
    uint32_t mem_size = 32 * 1024 * 1024; // Default fallback 32MB

    if (magic == 0x2BADB002 && mbi != NULL) {
        // Auto-detect RAM size from GRUB Multiboot header
        if (mbi->flags & 1) {
            // mem_upper is in KB and starts at 1MB
            mem_size = (mbi->mem_upper * 1024) + (1024 * 1024);
        }

        if (mbi->flags & (1 << 12)) {
            fb_p = (uint32_t)mbi->framebuffer_addr;
            fb_s = mbi->framebuffer_height * mbi->framebuffer_pitch;
            init_vbe(fb_p, mbi->framebuffer_width, mbi->framebuffer_height, mbi->framebuffer_pitch, mbi->framebuffer_bpp);
        }
    }
    
    extern void init_gdt();
    init_gdt();

    init_mem(mem_size);
    paging_init(fb_p, fb_s);
    idt_init();
    extern void init_syscalls(void);
    init_syscalls();
    init_timer(1000); // 1000 Hz PIT for 1ms precision ticks
    init_keyboard();
    detect_cpu();
    pci_scan();
    extern void init_rtl8139();
    init_rtl8139();
    extern void net_init();
    net_init();
    extern void init_serial();
    init_serial();
    init_uptime();
    vfs_init();

    init_double_buffer();
    init_tasking();

    __asm__ __volatile__ ("sti");
    
    // Removed dummy task creation

    init_mouse();
    draw_startup_logo();
    nada(440, 150); nada(523, 150); nada(659, 300);

    wm_init();
    cursor_saved_x = -1;
    gui_login();

    nada(659, 80); nada(784, 80); nada(1047, 150);
    full_redraw();

    // Kalkulator akan dibuka jika user mengklik ikonnya di desktop
    // extern int load_mct_app(const char*);
    // load_mct_app("gcalc.mct");

    // ---- Main GUI Event Loop ----
    int prev_btn  = 0;
    int prev_mx   = mouse_x, prev_my = mouse_y;
    uint32_t last_clock_tick = 0;
    uint32_t last_frame_tick = 0;
    int needs_redraw = 1;

    while (1) {
        int mx  = mouse_x, my = mouse_y;
        int btn = (int)(uint32_t)mouse_btn;

        if (mx != prev_mx || my != prev_my || btn != prev_btn) {
            int in_taskbar = (my >= (int)fb_height - TASKBAR_H_PX);
            int handled = wm_handle_mouse(mx, my, btn, prev_btn);
            if (!handled) {
                if (!in_taskbar) {
                    desktop_handle_mouse(mx, my, btn, prev_btn);
                } else if (!btn && prev_btn) {
                    taskbar_handle_click(mx, my);
                }
            }
            
            if (btn != prev_btn || handled) {
                needs_redraw = 1;
            } else {
                // Pure mouse move: no full redraw needed
                restore_cursor_bg();
                draw_mouse_cursor(mx, my);
                wait_for_vsync();
                swap_buffers();
            }
            prev_btn = btn; prev_mx = mx; prev_my = my;
        }

        uint8_t sc = k_get_scancode();
        if (sc != 0 && (sc < 0x80 || sc == 0xE0)) {
            char c = scancode_to_char(sc);
            wm_handle_key(c, sc);
            needs_redraw = 1;
        }

        extern void net_poll();
        net_poll();

        uint32_t now = get_ticks();
        
        // 1000 Hz timer => 1000 ticks = 1 second
        if (now - last_clock_tick >= 1000) {
            last_clock_tick = now;
            wm_tick_all();
            needs_redraw = 1;
        }

        // Disabled forced 60Hz redraw to fix FPS drops
        // Redraw will only happen on events (mouse/kb/timer)

        if (needs_redraw) {
            needs_redraw = 0;
            last_frame_tick = now;
            fps_frames++;
            
            // Update FPS counter every second (1000 ticks)
            if (now - fps_last_tick >= 1000) {
                fps_val = fps_frames;
                fps_frames = 0;
                fps_last_tick = now;
            }
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
