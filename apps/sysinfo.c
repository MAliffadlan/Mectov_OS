#include "apps/lib/libc.h"

void** __mct_lib_ptr;

static void draw_bar(int wid, int x, int y, int w, int h, uint32_t filled, uint32_t total, uint32_t on_col) {
    sys_draw_rect(wid, x, y, w, h, 0x00313244);
    sys_draw_rect(wid, x, y, w, 1, 0x0045475A); // Border
    sys_draw_rect(wid, x, y+h-1, w, 1, 0x0045475A);
    sys_draw_rect(wid, x, y, 1, h, 0x0045475A);
    sys_draw_rect(wid, x+w-1, y, 1, h, 0x0045475A);
    
    if (total > 0) {
        uint32_t pct = (filled / (total / 100 + 1));
        int fill = (int)(w * pct / 100);
        if (fill > w) fill = w;
        if (fill > 2) {
            sys_draw_rect(wid, x+1, y+1, fill-2, h-2, on_col);
        }
    }
}

static void draw_sysinfo(int wid) {
    sys_draw_rect(wid, 0, 0, 360, 240, 0x001E1E2E);
    
    sysinfo_t info;
    sys_get_sysinfo(&info);
    
    int lx = 12, ly = 10, gap = 28;

    sys_draw_text(wid, lx, ly, "System Information", 0x00CDD6F4);
    sys_draw_text(wid, 300, ly, "Ring 3", 0x00F9E2AF);
    sys_draw_rect(wid, lx, ly + 18, 360 - 24, 1, 0x00313244);
    ly += gap;

    // CPU
    sys_draw_text(wid, lx, ly, "CPU:", 0x006C7086);
    sys_draw_text(wid, lx + 40, ly, info.cpu_brand, 0x00CDD6F4);
    ly += gap;

    // RAM usage
    sys_draw_text(wid, lx, ly, "RAM:", 0x006C7086);
    char rbuf[64];
    sprintf(rbuf, "%d MB / %d MB", info.used_ram_kb / 1024, info.total_ram_kb / 1024);
    
    sys_draw_text(wid, lx + 40, ly, rbuf, 0x00CDD6F4);
    draw_bar(wid, lx + 40, ly + 14, 280, 8, info.used_ram_kb, info.total_ram_kb, 0x0089B4FA);
    ly += gap;

    // Uptime
    sys_draw_text(wid, lx, ly, "Up:", 0x006C7086);
    uint32_t secs = info.uptime_ms / 1000;
    uint32_t mins = secs / 60;
    uint32_t hrs  = mins / 60;
    char ubuf[32];
    sprintf(ubuf, "%02d:%02d:%02d", hrs, mins % 60, secs % 60);
    sys_draw_text(wid, lx + 40, ly, ubuf, 0x00A6E3A1);
    ly += gap;

    // Display
    sys_draw_text(wid, lx, ly, "VGA:", 0x006C7086);
    char sbuf[32];
    sprintf(sbuf, "%dx%d@%d", info.fb_width, info.fb_height, info.fb_bpp);
    sys_draw_text(wid, lx + 40, ly, sbuf, 0x00CDD6F4);
    ly += gap;

    // MAC Address
    sys_draw_text(wid, lx, ly, "MAC:", 0x006C7086);
    char mbuf[32];
    sprintf(mbuf, "%02X:%02X:%02X:%02X:%02X:%02X", 
            info.mac_addr[0], info.mac_addr[1], info.mac_addr[2],
            info.mac_addr[3], info.mac_addr[4], info.mac_addr[5]);
    sys_draw_text(wid, lx + 40, ly, mbuf, 0x00CDD6F4);
    ly += gap;

    // Cores — live per-CPU load bars (Fase 3 SMP: every core ticks and runs
    // its own runqueue, so each bar shows real per-core utilization).
    sys_draw_rect(wid, lx, ly - 6, 360 - 24, 1, 0x00313244);
    sys_draw_text(wid, lx, ly, "Cores:", 0x006C7086);
    if (info.cpu_count > 4) info.cpu_count = 4;
    static const uint32_t core_colors[4] = { 0x0089B4FA, 0x00A6E3A1, 0x00F9E2AF, 0x00F38BA8 };
    for (uint32_t i = 0; i < info.cpu_count; i++) {
        int cy = ly + 14 + (int)i * 24;
        char cbuf[16];
        sprintf(cbuf, "CPU %d", i);
        sys_draw_text(wid, lx, cy, cbuf, 0x00CDD6F4);
        draw_bar(wid, lx + 52, cy, 230, 12, info.cpu_load[i], 100, core_colors[i]);
        char pb[8];
        sprintf(pb, "%d%%", info.cpu_load[i]);
        sys_draw_text(wid, lx + 288, cy + 1, pb, core_colors[i]);
    }

    sys_update_window(wid);
}

typedef struct {
    int type; // 1 = paint, 2 = key, 3 = mouse
    int x, y;
    int key;
} gui_event_t;

void _start() {
    __mct_lib_ptr = mct_load_library("apps/libc.mct");
    
    int wid = sys_create_window(200, 150, 360, 300, "SysInfo (Ring 3)");
    if (wid < 0) sys_exit();
    
    draw_sysinfo(wid);
    
    gui_event_t ev;
    
    while (1) {
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 1) {
                draw_sysinfo(wid);
            } else if (ev.type == 2) {
                if (ev.key == 27) sys_exit(); // ESC
            }
        }
        
        // Live redraw at ~10 fps. sys_sleep parks the task (hlt) instead of
        // spinning on sys_yield, so SysInfo does not peg a whole core and the
        // CPU bars it renders stay honest.
        draw_sysinfo(wid);
        sys_sleep(100);
    }
}
