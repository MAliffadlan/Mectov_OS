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

static void full_redraw() {
    desktop_draw();
    wm_draw_all();
    taskbar_draw();
    draw_mouse_cursor(mouse_x, mouse_y);
    swap_buffers();
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
    init_timer(50);
    init_keyboard();
    detect_cpu();
    pci_scan();
    init_uptime();
    vfs_load();

    // Allocate double buffer now that memory is ready
    init_double_buffer();
    
    init_tasking();

    __asm__ __volatile__ ("sti");
    
    extern void dummy_task_entry();
    create_task(dummy_task_entry);

    init_mouse();

    draw_startup_logo();
    nada(440, 150); nada(523, 150); nada(659, 300);
    delay(600);

    wm_init();
    cursor_saved_x = -1;
    
    gui_login();

    nada(659, 80); nada(784, 80); nada(1047, 150);
    full_redraw();

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
            if (!btn && prev_btn) {
                if (!in_taskbar) desktop_handle_click(mx, my);
                else             taskbar_handle_click(mx, my);
            }
            wm_handle_mouse(mx, my, btn, prev_btn);
            prev_btn = btn; prev_mx = mx; prev_my = my;
            needs_redraw = 1;
        }

        uint8_t sc = k_get_scancode();
        if (sc != 0 && sc < 0x80) {
            char c = scancode_to_char(sc);
            wm_handle_key(c, sc);
            needs_redraw = 1;
        }

        uint32_t now = get_ticks();
        if (now - last_clock_tick >= 50) {
            last_clock_tick = now;
            wm_tick_all();
            needs_redraw = 1;
        }

        // Force continuous redraw for marquee animation
        needs_redraw = 1;

        // Throttle rendering to ~25 FPS (every 2 ticks at 50Hz)
        if (needs_redraw && (now - last_frame_tick >= 2)) {
            needs_redraw = 0;
            last_frame_tick = now;
            full_redraw();
        }

        __asm__ __volatile__ ("hlt");
    }
}

volatile int dummy_task_runs = 0;
void dummy_task_entry() {
    while (1) {
        dummy_task_runs++;
        if (is_vbe && fb_addr) {
            uint32_t color = (dummy_task_runs / 5000000) % 2 ? 0xFF0000 : 0x00FF00;
            // Draw a 20x20 blinking square at the top right (over the marquee)
            for (int y = 0; y < 20; y++) {
                for (int x = 0; x < 20; x++) {
                    fb_addr[(5 + y) * fb_width + (fb_width - 30 + x)] = color;
                }
            }
        }
    }
}
