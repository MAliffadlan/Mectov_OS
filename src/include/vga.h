#ifndef VGA_H
#define VGA_H

#include "types.h"

// ---- Framebuffer state ----
extern uint32_t* fb_addr;       // Physical framebuffer (front buffer)
extern uint32_t  fb_pitch;
extern uint32_t  fb_width;
extern uint32_t  fb_height;
extern uint8_t   fb_bpp;
extern int       is_vbe;

// ---- Double buffer ----
extern uint32_t* back_buffer;   // Off-screen rendering target
extern uint32_t  bb_pitch;      // Always width*4 (32-bit internal)

// ---- GUI Theme Colors (Linux Mint XFCE) ----
#define C_BLACK      0x00000000
#define C_WHITE      0x00FFFFFF
#define C_GREEN      0x0000FF00
#define C_CYAN       0x0000FFFF
#define C_BLUE       0x000000FF
#define C_NAVY       0x00000080
#define C_GRAY       0x00808080
#define C_DARK_GRAY  0x00404040
#define C_YELLOW     0x00FFFF00

#define GUI_BG       0x00E5E5E5
#define GUI_DESKTOP  0x002C2C2C
#define GUI_BORDER   0x00888888
#define GUI_BORDER2  0x00AAAAAA
#define GUI_TASKBAR  0x001F1F1F
#define GUI_TITLE_A  0x00D9D9D9
#define GUI_TITLE_B  0x00F0F0F0
#define GUI_BTN      0x00DDDDDD
#define GUI_BTN_HOV  0x00EEEEEE
#define GUI_CLOSE    0x00CC3333
#define GUI_ICON_BG  0x00333333
#define GUI_WHITE    0x00FFFFFF
#define GUI_CYAN     0x0088CC88
#define GUI_DIM      0x00444444
#define GUI_TEXT     0x00222222
#define GUI_TEXT_INV 0x00EEEEEE

// ---- Text console constants ----
#define CW 128
#define CH 48
#define CX 10
#define CY 5
#define WIN_X 15
#define WIN_Y 8
#define WIN_W 90
#define WIN_H 30

extern int cx, cy;
extern unsigned char cur_col;

// ---- Core framebuffer ----
void init_vbe(uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp);
void init_double_buffer(void);  // Call after init_mem + paging_init
void swap_buffers(void);        // Copy back_buffer → front buffer
void wait_vsync(void);          // Wait for vertical retrace (optional)

// ---- Drawing primitives (all write to back_buffer) ----
void put_pixel(int x, int y, uint32_t color);
void draw_rect(int x, int y, int w, int h, uint32_t color);
void draw_rect_border(int x, int y, int w, int h, uint32_t col);
void draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void draw_circle(int xc, int yc, int r, uint32_t color);
void fill_circle(int xc, int yc, int r, uint32_t color);

// ---- Text rendering ----
void d_char(int x, int y, char c, unsigned char col);
void draw_char_px(int px, int py, char c, uint32_t fg, uint32_t bg);
void draw_string_px(int px, int py, const char* s, uint32_t fg, uint32_t bg);
uint32_t vga_to_rgb(unsigned char col);

// ---- Legacy text-mode ----
void clear_screen(void);
void p_char(char c, unsigned char col);
void print(const char* s, unsigned char col);
void s_work(void);
void c_work(void);
void update_hw_cursor(int x, int y);
void update_marquee(void);
void update_hud(void);

// ---- GUI screens ----
void draw_startup_logo(void);
void d_desktop(void);
void d_win(int x, int y, int w, int h, const char* t);

// ---- Mouse cursor ----
void draw_mouse_cursor(int x, int y);
extern uint32_t cursor_save_buf[12*20];
extern int cursor_saved_x, cursor_saved_y;

// ---- Background save/restore (legacy, kept for login screen) ----
void save_bg(int x, int y, int w, int h, uint32_t* buf);
void restore_bg(int x, int y, int w, int h, uint32_t* buf);

#endif
