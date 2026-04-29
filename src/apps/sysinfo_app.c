#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/mem.h"
#include "../include/wm.h"
#include "../include/timer.h"
#include "../include/rtl8139.h"

static int si_win_id = -1;
static int si_open   = 0;

// Draw a labeled progress bar
static void draw_bar(int x, int y, int w, int h,
                     uint32_t filled, uint32_t total, uint32_t on_col) {
    draw_rect(x, y, w, h, GUI_BORDER2);
    draw_rect_border(x, y, w, h, GUI_BORDER);
    if (total > 0) {
        uint32_t pct = (filled / (total / 100 + 1));
        int fill = (int)(w * pct / 100);
        if (fill > w) fill = w;
        draw_rect(x, y, fill, h, on_col);
    }
}

static void si_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id; (void)ch;
    // App background
    draw_rect(cx, cy, cw, ch, GUI_BG);

    int lx = cx + 12, ly = cy + 10, gap = 28;

    // Title
    draw_string_px(lx, ly, "System Information", GUI_TEXT, 0xFFFFFFFF);
    draw_rect(lx, ly + 18, cw - 24, 1, GUI_BORDER2);
    ly += gap;

    // CPU
    draw_string_px(lx, ly, "CPU:", GUI_DIM, 0xFFFFFFFF);
    char short_cpu[32];
    int k2 = 0;
    while (cpu_brand[k2] == ' ') k2++;
    int j2 = 0;
    while (cpu_brand[k2] && j2 < 31) short_cpu[j2++] = cpu_brand[k2++];
    short_cpu[j2] = '\0';
    draw_string_px(lx + 40, ly, short_cpu, GUI_TEXT, 0xFFFFFFFF);
    ly += gap;

    // RAM usage
    unsigned int used_kb = (unsigned int)(get_used_memory() / 1024);
    unsigned int tot_kb  = (unsigned int)(get_total_memory() / 1024);
    draw_string_px(lx, ly, "RAM:", GUI_DIM, 0xFFFFFFFF);
    char rbuf[32];
    int rpos = 0;
    unsigned int tmp = used_kb;
    char tmp2[12]; int tl = 0;
    if (tmp == 0) { tmp2[tl++] = '0'; }
    while (tmp > 0) { tmp2[tl++] = '0' + tmp % 10; tmp /= 10; }
    for (int ri = tl-1; ri >= 0; ri--) rbuf[rpos++] = tmp2[ri];
    rbuf[rpos++] = '/'; rbuf[rpos++] = ' ';
    tmp = tot_kb; tl = 0;
    if (tmp == 0) { tmp2[tl++] = '0'; }
    while (tmp > 0) { tmp2[tl++] = '0' + tmp % 10; tmp /= 10; }
    for (int ri = tl-1; ri >= 0; ri--) rbuf[rpos++] = tmp2[ri];
    rbuf[rpos++] = ' '; rbuf[rpos++] = 'K'; rbuf[rpos++] = 'B'; rbuf[rpos] = '\0';
    draw_string_px(lx + 40, ly, rbuf, GUI_TEXT, 0xFFFFFFFF);
    ly += 16;
    draw_bar(lx, ly, cw - 24, 10, used_kb, tot_kb, GUI_TEAL);
    ly += gap;

    // Uptime
    uint32_t ticks = get_ticks();
    uint32_t secs  = ticks / 50;
    uint32_t mins2 = secs / 60; secs %= 60;
    uint32_t hrs2  = mins2 / 60; mins2 %= 60;
    draw_string_px(lx, ly, "Uptime:", GUI_DIM, 0xFFFFFFFF);
    char ubuf[32]; int upos = 0;
    if (hrs2 > 0) {
        tmp = hrs2; tl = 0;
        while (tmp > 0) { tmp2[tl++] = '0'+tmp%10; tmp/=10; }
        for (int ri=tl-1;ri>=0;ri--) ubuf[upos++]=tmp2[ri];
        ubuf[upos++]='h'; ubuf[upos++]=' ';
    }
    ubuf[upos++] = '0'+mins2/10; ubuf[upos++] = '0'+mins2%10;
    ubuf[upos++] = 'm'; ubuf[upos++] = ' ';
    ubuf[upos++] = '0'+secs/10; ubuf[upos++] = '0'+secs%10;
    ubuf[upos++] = 's'; ubuf[upos] = '\0';
    draw_string_px(lx + 60, ly, ubuf, GUI_TEXT, 0xFFFFFFFF);
    ly += gap;

    // OS info
    draw_string_px(lx, ly, "OS:", GUI_DIM, 0xFFFFFFFF);
    draw_string_px(lx + 40, ly, "Mectov OS v15.0 [VBE GUI]", GUI_TEXT, 0xFFFFFFFF);
    ly += gap;
    draw_string_px(lx, ly, "Res:", GUI_DIM, 0xFFFFFFFF);
    char res[20]; int rp=0;
    tmp=fb_width; tl=0;
    while(tmp>0){tmp2[tl++]='0'+tmp%10;tmp/=10;}
    for(int ri=tl-1;ri>=0;ri--)res[rp++]=tmp2[ri];
    res[rp++]='x';
    tmp=fb_height;tl=0;
    while(tmp>0){tmp2[tl++]='0'+tmp%10;tmp/=10;}
    for(int ri=tl-1;ri>=0;ri--)res[rp++]=tmp2[ri];
    res[rp]='\0';
    draw_string_px(lx + 40, ly, res, GUI_TEXT, 0xFFFFFFFF);
    ly += gap;

    // Network Info
    draw_string_px(lx, ly, "Net:", GUI_DIM, 0xFFFFFFFF);
    if (rtl_present) {
        char mac_str[24];
        int mi = 0;
        for (int i = 0; i < 6; i++) {
            uint8_t b = rtl_mac[i];
            uint8_t hi = (b >> 4) & 0x0F;
            uint8_t lo = b & 0x0F;
            mac_str[mi++] = (hi > 9) ? ('A' + hi - 10) : ('0' + hi);
            mac_str[mi++] = (lo > 9) ? ('A' + lo - 10) : ('0' + lo);
            if (i < 5) mac_str[mi++] = ':';
        }
        mac_str[mi] = '\0';
        draw_string_px(lx + 40, ly, "RTL8139 MAC ", GUI_TEXT, 0xFFFFFFFF);
        draw_string_px(lx + 140, ly, mac_str, GUI_TEAL, 0xFFFFFFFF);
    } else {
        draw_string_px(lx + 40, ly, "No Network Card Detected", 0x00FF0000, 0xFFFFFFFF);
    }
}

static void si_key(int id, char c, uint8_t sc) { (void)id;(void)c;(void)sc; }
static void si_tick(int id) { (void)id; }

void open_sysinfo_app() {
    if (si_open && wm_is_open(si_win_id)) { wm_raise(si_win_id); return; }
    si_win_id = wm_open(120, 80, 460, 270, "System Info", si_draw, si_key, si_tick, 0);
    si_open = (si_win_id >= 0);
}
