#include "../include/vga.h"
#include "../include/io.h"
#include "../include/utils.h"

volatile char* video_m = (volatile char*) 0xb8000;
unsigned char cur_col = 0x0F; 
int cx = 0, cy = 0;

void d_char(int x, int y, char c, unsigned char col) { if (x >= 0 && x < 80 && y >= 0 && y < 25) { int i = (y * 80 + x) * 2; video_m[i] = c; video_m[i + 1] = col; } }
void d_desktop() { for (int y = 0; y < 24; y++) for (int x = 0; x < 80; x++) d_char(x, y, 176, 0x09); for (int x = 0; x < 80; x++) d_char(x, 24, ' ', 0x70); }
void d_win(int x, int y, int w, int h, const char* t) { for (int i = x; i < x + w; i++) for (int j = y; j < y + h; j++) { if (j == y) d_char(i, j, ' ', 0x1F); else if (i == x || i == x + w - 1 || j == y + h - 1) d_char(i, j, 177, 0x1F); else d_char(i, j, ' ', 0x0F); } int tl = 0; while(t[tl]) tl++; int tx = x + (w - tl) / 2; for(int i = 0; i < tl; i++) d_char(tx + i, y, t[i], 0x1F); }
void c_work() { for (int y = CY; y < CY + CH - 1; y++) for (int x = CX; x < CX + CW; x++) d_char(x, y, ' ', 0x0F); cx = 0; cy = 0; update_hw_cursor(CX, CY); }
void s_work() { for (int y = CY; y < CY + CH - 2; y++) for (int x = CX; x < CX + CW; x++) { int s = ((y + 1) * 80 + x) * 2, d = (y * 80 + x) * 2; video_m[d] = video_m[s]; video_m[d + 1] = video_m[s + 1]; } int ly = CY + CH - 2; for (int x = CX; x < CX + CW; x++) d_char(x, ly, ' ', 0x0F); cy--; }
void print(const char* s, unsigned char col) { int i = 0; while (s[i] != '\0') { if (s[i] == '\n') { cx = 0; cy++; } else { d_char(CX + cx, CY + cy, s[i], col); cx++; if (cx >= CW) { cx = 0; cy++; } } if (cy >= CH - 1) s_work(); i++; } update_hw_cursor(CX + cx, CY + cy); }
void p_char(char c, unsigned char col) { if (c == '\n') { cx = 0; cy++; } else { d_char(CX + cx, CY + cy, c, col); cx++; if (cx >= CW) { cx = 0; cy++; } } if (cy >= CH - 1) s_work(); update_hw_cursor(CX + cx, CY + cy); }

void update_hw_cursor(int x, int y) {
    unsigned short pos = y * 80 + x;
    outb(0x3D4, 0x0F); outb(0x3D5, (unsigned char) (pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (unsigned char) ((pos >> 8) & 0xFF));
}

const char* marquee_text = ">>> Mectov OS v10.5 - Pro Dashboard Enabled - TUI System Stable - Indonesia Raya <<<         ";
int marquee_pos = 0; int marquee_counter = 0;
void wait_retrace() { while (inb(0x3DA) & 0x08); while (!(inb(0x3DA) & 0x08)); }
void update_marquee() {
    marquee_counter++; if (marquee_counter < 350000) return; marquee_counter = 0;
    int text_len = 0; while(marquee_text[text_len]) text_len++;
    wait_retrace();
    for (int i = 0; i < 80; i++) { int c_idx = (marquee_pos + i) % text_len; int v_idx = (24 * 80 + i) * 2; video_m[v_idx] = marquee_text[c_idx]; video_m[v_idx + 1] = 0x70; }
    marquee_pos = (marquee_pos + 1) % text_len;
}

int hud_counter = 0;
void update_hud() {
    hud_counter++; if (hud_counter < 600000) return; hud_counter = 0;
    unsigned char ch = bcd_to_bin(read_cmos(0x04)), cm = bcd_to_bin(read_cmos(0x02)), cs = bcd_to_bin(read_cmos(0x00));
    int wj = (ch + 7) % 24;
    int tb = boot_hour * 60 + boot_min, tc = ch * 60 + cm, diff = tc - tb; if (diff < 0) diff += 1440;
    int up_h = diff / 60; int up_m = diff % 60;
    
    char h_str[40]; int i = 0;
    const char* t1 = " WIB "; while(*t1) h_str[i++] = *t1++;
    h_str[i++] = (wj/10)+'0'; h_str[i++] = (wj%10)+'0'; h_str[i++] = ':';
    h_str[i++] = (cm/10)+'0'; h_str[i++] = (cm%10)+'0'; h_str[i++] = ':';
    h_str[i++] = (cs/10)+'0'; h_str[i++] = (cs%10)+'0';
    const char* t2 = " | UP: "; while(*t2) h_str[i++] = *t2++;
    if(up_h>9) { h_str[i++] = (up_h/10)+'0'; } h_str[i++] = (up_h%10)+'0'; h_str[i++] = 'h'; h_str[i++] = ' ';
    if(up_m>9) { h_str[i++] = (up_m/10)+'0'; } h_str[i++] = (up_m%10)+'0'; h_str[i++] = 'm'; h_str[i++] = ' ';
    h_str[i] = '\0';
    
    int start_x = 80 - i;
    wait_retrace();
    for (int j = 0; j < i; j++) { d_char(start_x + j, 0, h_str[j], 0x1E); } 
}
