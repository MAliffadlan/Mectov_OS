#include "src/include/syscall.h"

static void itoa(int val, char* buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char temp[12]; int len = 0;
    while (val > 0) { temp[len++] = '0' + (val % 10); val /= 10; }
    int pos = 0;
    for (int i = len - 1; i >= 0; i--) buf[pos++] = temp[i];
    buf[pos] = '\0';
}

static void itoa_pad(int val, char* buf, int pad) {
    char temp[12]; int len = 0;
    if (val == 0) temp[len++] = '0';
    while (val > 0) { temp[len++] = '0' + (val % 10); val /= 10; }
    int pos = 0;
    while (pos < pad - len) buf[pos++] = '0';
    for (int i = len - 1; i >= 0; i--) buf[pos++] = temp[i];
    buf[pos] = '\0';
}

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
    int rpos = 0;
    
    char tmp[16];
    itoa(info.used_ram_kb / 1024, tmp);
    for(int i=0; tmp[i]; i++) rbuf[rpos++] = tmp[i];
    rbuf[rpos++] = ' '; rbuf[rpos++] = 'M'; rbuf[rpos++] = 'B'; rbuf[rpos++] = ' ';
    rbuf[rpos++] = '/'; rbuf[rpos++] = ' ';
    
    itoa(info.total_ram_kb / 1024, tmp);
    for(int i=0; tmp[i]; i++) rbuf[rpos++] = tmp[i];
    rbuf[rpos++] = ' '; rbuf[rpos++] = 'M'; rbuf[rpos++] = 'B'; rbuf[rpos] = '\0';
    
    sys_draw_text(wid, lx + 40, ly, rbuf, 0x00CDD6F4);
    draw_bar(wid, lx + 40, ly + 14, 280, 8, info.used_ram_kb, info.total_ram_kb, 0x0089B4FA);
    ly += gap;

    // Uptime
    sys_draw_text(wid, lx, ly, "Up:", 0x006C7086);
    uint32_t secs = info.uptime_ms / 1000;
    uint32_t mins = secs / 60;
    uint32_t hrs  = mins / 60;
    char ubuf[32];
    itoa_pad(hrs, ubuf, 2);
    ubuf[2] = ':';
    itoa_pad(mins % 60, ubuf+3, 2);
    ubuf[5] = ':';
    itoa_pad(secs % 60, ubuf+6, 2);
    ubuf[8] = '\0';
    sys_draw_text(wid, lx + 40, ly, ubuf, 0x00A6E3A1);
    ly += gap;

    // Display
    sys_draw_text(wid, lx, ly, "VGA:", 0x006C7086);
    char sbuf[32];
    int spos = 0;
    itoa(info.fb_width, tmp);
    for(int i=0; tmp[i]; i++) sbuf[spos++] = tmp[i];
    sbuf[spos++] = 'x';
    itoa(info.fb_height, tmp);
    for(int i=0; tmp[i]; i++) sbuf[spos++] = tmp[i];
    sbuf[spos++] = '@';
    itoa(info.fb_bpp, tmp);
    for(int i=0; tmp[i]; i++) sbuf[spos++] = tmp[i];
    sbuf[spos] = '\0';
    sys_draw_text(wid, lx + 40, ly, sbuf, 0x00CDD6F4);
    ly += gap;

    // MAC Address
    sys_draw_text(wid, lx, ly, "MAC:", 0x006C7086);
    char mbuf[32];
    int mpos = 0;
    const char hex[] = "0123456789ABCDEF";
    for(int i=0; i<6; i++) {
        mbuf[mpos++] = hex[(info.mac_addr[i] >> 4) & 0xF];
        mbuf[mpos++] = hex[info.mac_addr[i] & 0xF];
        if (i < 5) mbuf[mpos++] = ':';
    }
    mbuf[mpos] = '\0';
    sys_draw_text(wid, lx + 40, ly, mbuf, 0x00CDD6F4);

    sys_update_window(wid);
}

typedef struct {
    int type; // 1 = paint, 2 = key, 3 = mouse
    int x, y;
    int key;
} gui_event_t;

void _start() {
    int wid = sys_create_window(200, 200, 360, 240, "SysInfo (Ring 3)");
    if (wid < 0) sys_exit();
    
    draw_sysinfo(wid);
    
    gui_event_t ev;
    int tick = 0;
    
    while (1) {
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 1) {
                draw_sysinfo(wid);
            } else if (ev.type == 2) {
                if (ev.key == 27) sys_exit(); // ESC
            }
        }
        
        tick++;
        if (tick % 10000 == 0) {
            draw_sysinfo(wid);
        }
        
        sys_yield();
    }
}
