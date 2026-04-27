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

static void full_redraw() {
    desktop_draw();
    wm_draw_all();
    taskbar_draw();

    // FPS counter at top-right
    char fps_buf[12];
    int fi = 0;
    int tmp = fps_val;
    if (tmp == 0) { fps_buf[fi++] = '0'; }
    else {
        char rev[8]; int rl = 0;
        while (tmp > 0) { rev[rl++] = '0' + tmp % 10; tmp /= 10; }
        while (rl > 0) fps_buf[fi++] = rev[--rl];
    }
    fps_buf[fi++] = ' ';
    fps_buf[fi++] = 'F';
    fps_buf[fi++] = 'P';
    fps_buf[fi++] = 'S';
    fps_buf[fi] = '\0';

    int fx = (int)fb_width - (fi * 8) - 8;
    draw_rect(fx - 4, 22, fi * 8 + 8, 18, 0x00000000);
    draw_string_px(fx, 23, fps_buf, 0x0000FF00, 0x00000000);

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
    extern void init_syscalls(void);
    init_syscalls();
    init_timer(60);
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
    vfs_load();

    // User Mode (Ring 3) infrastructure is READY:
    //   - GDT has User Code (0x18) and User Data (0x20) segments
    //   - TSS is loaded for kernel stack switching
    //   - Syscall handler registered at int 0x80
    // Currently disabled: apps use direct I/O (in/out) which needs Ring 0.
    // To activate: refactor apps to use syscalls, then uncomment below:
    // extern void switch_to_user_mode(void);
    // switch_to_user_mode();

    // Allocate double buffer now that memory is ready
    init_double_buffer();
    
    init_tasking();

    __asm__ __volatile__ ("sti");
    
    // Create a Ring 3 user task for testing
    extern int create_user_task(void (*entry)());
    extern void user_task_entry();
    create_user_task(user_task_entry);

    init_mouse();

    draw_startup_logo();
    nada(440, 150); nada(523, 150); nada(659, 300);
    // Removed 10-second delay(600) so it boots instantly without hanging on black screen

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
            if (!wm_handle_mouse(mx, my, btn, prev_btn)) {
                if (!in_taskbar) {
                    desktop_handle_mouse(mx, my, btn, prev_btn);
                } else if (!btn && prev_btn) {
                    taskbar_handle_click(mx, my);
                }
            }
            prev_btn = btn; prev_mx = mx; prev_my = my;
            needs_redraw = 1;
        }

        uint8_t sc = k_get_scancode();
        if (sc != 0 && sc < 0x80) {
            char c = scancode_to_char(sc);
            wm_handle_key(c, sc);
            needs_redraw = 1;
        }

        // Poll network for incoming packets
        extern void net_poll();
        net_poll();

        uint32_t now = get_ticks();
        if (now - last_clock_tick >= 60) {
            last_clock_tick = now;
            wm_tick_all();
            needs_redraw = 1;
        }

        // Force continuous redraw for marquee animation
        needs_redraw = 1;

        // Throttle rendering to ~30 FPS (every 2 ticks at 60Hz)
        if (needs_redraw && (now - last_frame_tick >= 2)) {
            needs_redraw = 0;
            last_frame_tick = now;
            fps_frames++;
            // Update FPS counter every second (60 ticks)
            if (now - fps_last_tick >= 60) {
                fps_val = fps_frames;
                fps_frames = 0;
                fps_last_tick = now;
            }
            full_redraw();
        }

        __asm__ __volatile__ ("hlt");
    }
}

#include "src/include/syscall.h"

// Ring 3 user task — runs entirely in user mode.
// Only communicates with kernel via int 0x80 syscalls.
void user_task_entry() {
    // Simple blinking indicator: draw a small colored rect via syscall
    int tick = 0;
    while (1) {
        uint32_t color = (tick & 1) ? 0x00FF00 : 0x00FFFF;
        // Syscall 7: Draw Rect. Args packed: ebx=(x<<16|y), ecx=(w<<16|h), edx=color
        syscall(7, (770 << 16) | 5, (16 << 16) | 16, color);
        tick++;
        // Busy-wait delay (no privileged instructions!)
        for (volatile int i = 0; i < 2000000; i++);
    }
}
