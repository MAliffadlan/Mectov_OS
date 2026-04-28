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

#include "theme.h"

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
void wait_for_vsync(void);      // Wait for vertical retrace
void mark_dirty(int x, int y, int w, int h); // Mark dirty rect

// ---- Drawing primitives (all write to back_buffer) ----
void put_pixel(int x, int y, uint32_t color);
void draw_rect(int x, int y, int w, int h, uint32_t color);
void draw_rect_alpha(int x, int y, int w, int h, uint32_t color);
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
void restore_cursor_bg(void);
extern uint32_t cursor_save_buf[12*20];
extern int cursor_saved_x, cursor_saved_y;

// ---- Background save/restore (legacy, kept for login screen) ----
void save_bg(int x, int y, int w, int h, uint32_t* buf);
void restore_bg(int x, int y, int w, int h, uint32_t* buf);

#endif
