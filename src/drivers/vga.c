// ============================================================
// vga.c — Mectov OS Framebuffer Driver with Double Buffering
// ============================================================
#include "../include/vga.h"
#include "../include/io.h"
#include "../include/utils.h"
#include "../include/font8x16.h"
#include "../include/mem.h"

// ---- Framebuffer state ----
uint32_t* fb_addr   = (uint32_t*)0;   // Physical front buffer
uint32_t  fb_pitch  = 0;
uint32_t  fb_width  = 0;
uint32_t  fb_height = 0;
uint8_t   fb_bpp    = 32;
int       is_vbe    = 0;

// ---- Double buffer ----
uint32_t* back_buffer = NULL;         // All drawing goes here
uint32_t  bb_pitch    = 0;            // Always width*4

// ---- Text console state ----
volatile char* video_m = (volatile char*) 0xb8000;
int cx = 0, cy = 0;
unsigned char cur_col = 0x0F;

// ============================================================
// VGA palette conversion
// ============================================================
uint32_t vga_to_rgb(unsigned char col) {
    switch (col & 0x0F) {
        case 0x00: return 0x00000000; case 0x01: return 0x000000AA;
        case 0x02: return 0x0000AA00; case 0x03: return 0x0000AAAA;
        case 0x04: return 0x00AA0000; case 0x05: return 0x00AA00AA;
        case 0x06: return 0x00AA5500; case 0x07: return 0x00AAAAAA;
        case 0x08: return 0x00555555; case 0x09: return 0x005555FF;
        case 0x0A: return 0x0055FF55; case 0x0B: return 0x0055FFFF;
        case 0x0C: return 0x00FF5555; case 0x0D: return 0x00FF55FF;
        case 0x0E: return 0x00FFFF55; case 0x0F: return 0x00FFFFFF;
        default: return 0x00FFFFFF;
    }
}

// ============================================================
// Core drawing — all write to back_buffer
// ============================================================
void put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || x >= (int)fb_width || y < 0 || y >= (int)fb_height) return;
    if (back_buffer)
        back_buffer[y * (bb_pitch / 4) + x] = color;
    else {
        // Fallback: direct write (before double buffer init)
        if (fb_bpp == 32)
            fb_addr[y * (fb_pitch / 4) + x] = color;
        else if (fb_bpp == 24) {
            uint8_t* row = (uint8_t*)fb_addr + (y * fb_pitch) + (x * 3);
            row[0] = color & 0xFF;
            row[1] = (color >> 8) & 0xFF;
            row[2] = (color >> 16) & 0xFF;
        }
    }
}

void draw_rect(int x, int y, int w, int h, uint32_t color) {
    // Clip to screen
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w > (int)fb_width)  ? (int)fb_width  : (x + w);
    int y1 = (y + h > (int)fb_height) ? (int)fb_height : (y + h);
    if (x0 >= x1 || y0 >= y1) return;

    if (back_buffer) {
        uint32_t stride = bb_pitch / 4;
        int count = x1 - x0;
        for (int row = y0; row < y1; row++) {
            uint32_t* dst = back_buffer + row * stride + x0;
            uint32_t cnt = (uint32_t)count;
            // rep stosl: fill cnt dwords at dst with color
            __asm__ __volatile__(
                "rep stosl"
                : "+D"(dst), "+c"(cnt)
                : "a"(color)
                : "memory"
            );
        }
    } else {
        for (int i = y0; i < y1; i++)
            for (int j = x0; j < x1; j++)
                put_pixel(j, i, color);
    }
}

void draw_rect_border(int x, int y, int w, int h, uint32_t col) {
    draw_rect(x, y, w, 1, col);          // top
    draw_rect(x, y + h - 1, w, 1, col);  // bottom
    draw_rect(x, y, 1, h, col);          // left
    draw_rect(x + w - 1, y, 1, h, col);  // right
}

// ============================================================
// Bresenham line
// ============================================================
void draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    while (1) {
        put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

// ============================================================
// Midpoint circle (outline)
// ============================================================
void draw_circle(int xc, int yc, int r, uint32_t color) {
    int x = 0, y = r, d = 1 - r;
    while (x <= y) {
        put_pixel(xc + x, yc + y, color);
        put_pixel(xc - x, yc + y, color);
        put_pixel(xc + x, yc - y, color);
        put_pixel(xc - x, yc - y, color);
        put_pixel(xc + y, yc + x, color);
        put_pixel(xc - y, yc + x, color);
        put_pixel(xc + y, yc - x, color);
        put_pixel(xc - y, yc - x, color);
        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}

// ============================================================
// Filled circle
// ============================================================
void fill_circle(int xc, int yc, int r, uint32_t color) {
    int x = 0, y = r, d = 1 - r;
    while (x <= y) {
        draw_rect(xc - x, yc + y, 2 * x + 1, 1, color);
        draw_rect(xc - x, yc - y, 2 * x + 1, 1, color);
        draw_rect(xc - y, yc + x, 2 * y + 1, 1, color);
        draw_rect(xc - y, yc - x, 2 * y + 1, 1, color);
        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}

// ============================================================
// Init VBE + allocate double buffer
// ============================================================
void init_vbe(uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp) {
    fb_addr   = (uint32_t*)addr;
    fb_width  = width;
    fb_height = height;
    fb_pitch  = pitch;
    fb_bpp    = bpp;
    bb_pitch  = width * 4;  // Internal back buffer always 32-bit
    is_vbe    = 1;
    // Back buffer will be allocated after kmalloc is ready (from kernel_main)
    back_buffer = NULL;
}

// Called from kernel_main AFTER init_mem + paging_init
void init_double_buffer(void) {
    if (!is_vbe || fb_width == 0 || fb_height == 0) return;
    uint32_t buf_size = fb_width * fb_height * 4;
    back_buffer = (uint32_t*)kmalloc(buf_size);
    if (back_buffer) {
        memset(back_buffer, 0, buf_size);
        bb_pitch = fb_width * 4;
    }
}

// ============================================================
// swap_buffers — copy back_buffer to front framebuffer
// ============================================================
void swap_buffers(void) {
    if (!back_buffer || !fb_addr) return;

    if (fb_bpp == 32 && fb_pitch == bb_pitch) {
        // Ideal case: same pitch, bulk memcpy
        memcpy(fb_addr, back_buffer, fb_width * fb_height * 4);
    } else if (fb_bpp == 32) {
        // Different pitch: copy row by row
        for (uint32_t y = 0; y < fb_height; y++) {
            uint32_t* src = back_buffer + y * fb_width;
            uint32_t* dst = (uint32_t*)((uint8_t*)fb_addr + y * fb_pitch);
            memcpy(dst, src, fb_width * 4);
        }
    } else if (fb_bpp == 24) {
        // 24-bit: convert each pixel
        for (uint32_t y = 0; y < fb_height; y++) {
            uint32_t* src = back_buffer + y * fb_width;
            uint8_t*  dst = (uint8_t*)fb_addr + y * fb_pitch;
            for (uint32_t x = 0; x < fb_width; x++) {
                uint32_t c = src[x];
                dst[x * 3 + 0] = c & 0xFF;
                dst[x * 3 + 1] = (c >> 8) & 0xFF;
                dst[x * 3 + 2] = (c >> 16) & 0xFF;
            }
        }
    }
}

// ============================================================
// VSync — wait for vertical retrace via VGA status port
// ============================================================
void wait_vsync(void) {
    // Wait until not in retrace
    while (inb(0x3DA) & 0x08);
    // Wait until retrace begins
    while (!(inb(0x3DA) & 0x08));
}

// ============================================================
// Text rendering
// ============================================================
void d_char(int x, int y, char c, unsigned char col) {
    if (is_vbe) {
        uint32_t fg = vga_to_rgb(col);
        uint32_t bg = vga_to_rgb(col >> 4);
        int px = x * 8, py = y * 16;
        for (int j = 0; j < 16; j++) {
            unsigned char row = font8x16_data[(unsigned char)c][j];
            for (int i = 0; i < 8; i++) {
                if (row & (0x80 >> i)) put_pixel(px + i, py + j, fg);
                else put_pixel(px + i, py + j, bg);
            }
        }
    } else {
        int i = (y * 80 + x) * 2;
        video_m[i] = c; video_m[i + 1] = col;
    }
}

void draw_char_px(int px, int py, char c, uint32_t fg, uint32_t bg) {
    if (!is_vbe) return;
    for (int j = 0; j < 16; j++) {
        unsigned char row = font8x16_data[(unsigned char)c][j];
        for (int i = 0; i < 8; i++) {
            uint32_t col = (row & (0x80 >> i)) ? fg : bg;
            if (col != 0xFFFFFFFF) put_pixel(px + i, py + j, col);
        }
    }
}

void draw_string_px(int px, int py, const char* s, uint32_t fg, uint32_t bg) {
    while (*s) { draw_char_px(px, py, *s++, fg, bg); px += 8; }
}

// ============================================================
// Clear screen
// ============================================================
void clear_screen() {
    if (is_vbe) draw_rect(0, 0, fb_width, fb_height, GUI_BG);
    else for (int i = 0; i < 80 * 25; i++) { video_m[i * 2] = ' '; video_m[i * 2 + 1] = 0x1F; }
    cx = 0; cy = 0;
}

// ============================================================
// Text console scroll
// ============================================================
void s_work() {
    if (is_vbe) {
        int sx = (WIN_X + 1) * 8, sy = (WIN_Y + 1) * 16;
        int sw = (WIN_W - 2) * 8, sh = (WIN_H - 2) * 16;
        uint32_t* buf = back_buffer ? back_buffer : fb_addr;
        uint32_t stride = back_buffer ? (bb_pitch / 4) : (fb_pitch / 4);
        for (int y = sy; y < sy + sh - 16; y++)
            for (int x = sx; x < sx + sw; x++)
                buf[y * stride + x] = buf[(y + 16) * stride + x];
        draw_rect(sx, sy + sh - 16, sw, 16, 0);
    } else {
        for (int y = 1; y < 24; y++) for (int x = 0; x < 80; x++) {
            int s = ((y + 1) * 80 + x) * 2, d = (y * 80 + x) * 2;
            video_m[d] = video_m[s]; video_m[d + 1] = video_m[s + 1];
        }
        for (int x = 0; x < 80; x++) { video_m[(24 * 80 + x) * 2] = ' '; video_m[(24 * 80 + x) * 2 + 1] = 0x0F; }
    }
    cy--;
}

void p_char(char c, unsigned char col) {
    extern int get_use_term_buf();
    extern void p_char_gui(char c2, unsigned char col2);
    if (get_use_term_buf()) { p_char_gui(c, col); return; }
    if (c == '\n') { cx = 0; cy++; }
    else {
        d_char(WIN_X + 1 + cx, WIN_Y + 1 + cy, c, col);
        cx++; if (cx >= WIN_W - 2) { cx = 0; cy++; }
    }
    if (cy >= WIN_H - 2) s_work();
}

void print(const char* s, unsigned char col) { while (*s) p_char(*s++, col); }

// ============================================================
// GUI screens
// ============================================================
void draw_startup_logo() {
    if (!is_vbe || fb_width == 0 || fb_height == 0) return;

    // Mint dark gradient background
    for (uint32_t y = 0; y < fb_height; y++) {
        uint32_t t = (y * 255) / fb_height;
        uint32_t r = 30 + (t * 10) / 255;
        uint32_t g = 34 + (t * 20) / 255;
        uint32_t b = 36 + (t * 10) / 255;
        draw_rect(0, y, fb_width, 1, (r << 16) | (g << 8) | b);
    }

    // Center box
    int bw = 400, bh = 80;
    int bx = (int)(fb_width  - bw) / 2;
    int by = (int)(fb_height - bh) / 2;

    draw_rect(bx, by, bw, bh, GUI_BG);
    draw_rect_border(bx - 1, by - 1, bw + 2, bh + 2, GUI_BORDER);

    draw_string_px(bx + (bw - 21*8)/2, by + 14, "Mectov OS v13.5 [VBE]", GUI_TEXT, GUI_BG);
    draw_string_px(bx + (bw - 22*8)/2, by + 38, "PROFESSIONAL  GUI  EDITION", GUI_CYAN, GUI_BG);
    draw_string_px(bx + (bw - 20*8)/2, by + 56, "Loading, please wait...", GUI_DIM,  GUI_BG);

    // Flush splash to screen immediately
    swap_buffers();
}

void d_desktop() {
    if (is_vbe) {
        // Mint XFCE style background — clean dark gray gradient
        for (uint32_t y = 0; y < fb_height; y++) {
            uint32_t t = (y * 255) / (fb_height ? fb_height : 1);
            uint32_t r = 30 + (t * 10) / 255;
            uint32_t g = 34 + (t * 10) / 255;
            uint32_t b = 36 + (t * 10) / 255;
            draw_rect(0, y, fb_width, 1, (r << 16) | (g << 8) | b);
        }
        // Bottom taskbar strip
        draw_rect(0, fb_height - 28, fb_width, 28, GUI_TASKBAR);
        draw_rect(0, fb_height - 28, fb_width, 1, GUI_BORDER);
    }
}

void d_win(int x, int y, int w, int h, const char* t) {
    if (!is_vbe) return;
    int px = x * 8, py = y * 16, pw = w * 8, ph = h * 16;

    draw_rect(px + 4, py + 4, pw, ph, 0x00000000);
    draw_rect(px, py, pw, ph, GUI_BG);
    draw_rect_border(px - 1, py - 1, pw + 2, ph + 2, GUI_BORDER);
    draw_rect(px, py, pw, 22, GUI_TITLE_A);
    draw_rect(px, py + 22, pw, 1, GUI_BORDER);
    draw_rect(px + pw - 20, py + 4, 14, 14, GUI_CLOSE);
    draw_char_px(px + pw - 17, py + 3, 'x', GUI_WHITE, GUI_CLOSE);
    draw_string_px(px + 6, py + 3, t, GUI_TEXT, GUI_TITLE_A);
}

// ============================================================
// Legacy stubs
// ============================================================
void c_work() { cx = 0; cy = 0; }
void update_hw_cursor(int x, int y) { (void)x; (void)y; }
void update_marquee() {}
void update_hud() {}

// ============================================================
// Background save/restore (for login screen cursor)
// ============================================================
void save_bg(int x, int y, int w, int h, uint32_t* buf) {
    if (!is_vbe) return;
    uint32_t* src = back_buffer ? back_buffer : fb_addr;
    uint32_t stride = back_buffer ? (bb_pitch / 4) : (fb_pitch / 4);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int px2 = x + i, py2 = y + j;
            if (px2 >= 0 && px2 < (int)fb_width && py2 >= 0 && py2 < (int)fb_height)
                buf[j * w + i] = src[py2 * stride + px2];
            else buf[j * w + i] = 0;
        }
}

void restore_bg(int x, int y, int w, int h, uint32_t* buf) {
    if (!is_vbe) return;
    uint32_t* dst = back_buffer ? back_buffer : fb_addr;
    uint32_t stride = back_buffer ? (bb_pitch / 4) : (fb_pitch / 4);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int px2 = x + i, py2 = y + j;
            if (px2 >= 0 && px2 < (int)fb_width && py2 >= 0 && py2 < (int)fb_height)
                dst[py2 * stride + px2] = buf[j * w + i];
        }
}

// ============================================================
// Mouse cursor — always drawn last, directly to back_buffer
// ============================================================
static const unsigned char cursor_mask[20] = {
    0xC0,0xE0,0xF0,0xF8,0xFC,0xFE,0xFF,0xF8,
    0xD8,0x8C,0x0C,0x06,0x06,0x03,0x03,0x01,
    0x00,0x00,0x00,0x00
};
static const unsigned char cursor_inner[20] = {
    0x00,0x40,0x60,0x70,0x78,0x7C,0x78,0x70,
    0x50,0x08,0x04,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00
};

uint32_t cursor_save_buf[12*20];
int cursor_saved_x = -1, cursor_saved_y = -1;

void draw_mouse_cursor(int x, int y) {
    if (!is_vbe) return;
    // With double buffering, no save/restore needed — scene is redrawn each frame
    // Just draw the cursor on top of the back buffer
    cursor_saved_x = x; cursor_saved_y = y;
    for (int j = 0; j < 20; j++) {
        unsigned char mask = (j < 16) ? cursor_mask[j] : 0;
        unsigned char inner = (j < 16) ? cursor_inner[j] : 0;
        for (int i = 0; i < 8; i++) {
            if (mask & (0x80 >> i)) {
                uint32_t col = (inner & (0x80 >> i)) ? 0x00FFFFFF : 0x00000000;
                put_pixel(x + i, y + j, col);
            }
        }
    }
}
