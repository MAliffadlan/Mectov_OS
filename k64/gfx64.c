/* M9 GUI-1 drawing core: one place that knows the framebuffer's pixel format.
 *
 * Before this file every consumer wrote pixels itself (M8's console had its
 * own put_px/row_copy). Now the format rules — 16/24/32bpp packing, pitch
 * strides, clipping — live here, and the console, the desktop chrome and the
 * mouse cursor all draw through these calls. The framebuffer is UC (mem64
 * pass 5), so fills use wide stores where the format allows it: a 1024x768
 * wallpaper is ~786K pixels and per-pixel byte stores would crawl.
 *
 * Clipping is unconditional on purpose: a desktop draws chrome near the
 * screen edges, and one bad coordinate must not scribble outside the panel.
 * Primitives that only touch the inside of a rect (gfx_shift_up, fills) skip
 * the per-pixel checks and clip the rect once.
 *
 * Glyphs come from the shared 8x16 table (src/drivers/font8x16.c), same as
 * the M8 console and the 32-bit desktop, so there is one font in the tree.
 */
#include "cpu64.h"

extern unsigned char font8x16_data[256][16];

static volatile u8 *fb; /* framebuffer base (identity-mapped, UC) */
static u32 fb_pitch;
static int fb_w, fb_h;
static int fb_bpp, fb_px;
static int fb_on;

/* Build a 32bpp pixel word for the current format (single source of truth:
 * every primitive below funnels through store_u32/pack). */
static u32 pack(u32 rgb) {
    if (fb_bpp == 16) /* RGB565 */
        return (u32)(((rgb >> 19) & 0x1F) << 11 | ((rgb >> 10) & 0x3F) << 5 |
                     ((rgb >> 3) & 0x1F));
    return rgb & 0xFFFFFFu;
}

static void store_u32(int x, int y, u32 rgb) {
    volatile u8 *p = fb + (u64)y * fb_pitch + (u64)x * (u64)fb_px;
    u32 v = pack(rgb);
    if (fb_bpp == 32) {
        *(volatile u32 *)p = v;
    } else if (fb_bpp == 24) {
        p[0] = (u8)v;
        p[1] = (u8)(v >> 8);
        p[2] = (u8)(v >> 16);
    } else {
        *(volatile u16 *)p = (u16)v;
    }
}

u64 gfx_read(int x, int y) {
    if (!fb_on || x < 0 || y < 0 || x >= fb_w || y >= fb_h) return 0;
    volatile u8 *p = fb + (u64)y * fb_pitch + (u64)x * (u64)fb_px;
    if (fb_bpp == 32) return *(volatile u32 *)p & 0xFFFFFFu;
    if (fb_bpp == 24) return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16);
    u32 v = *(volatile u16 *)p;
    return (((v >> 11) & 0x1F) << 19) | (((v >> 5) & 0x3F) << 10) |
           ((v & 0x1F) << 3);
}

int gfx_ready(void) { return fb_on; }
int gfx_width(void) { return fb_w; }
int gfx_height(void) { return fb_h; }

/* Returns 0 for modes we cannot pack (callers fall back to serial-only). */
int gfx_init(u64 addr, u32 pitch, u32 w, u32 h, u32 bpp) {
    fb_on = 0;
    if (!addr || !pitch || !w || !h) return 0;
    if (bpp != 16 && bpp != 24 && bpp != 32) return 0;
    if (pitch < w * (bpp / 8)) return 0;
    fb = (volatile u8 *)addr;
    fb_pitch = pitch;
    fb_w = (int)w;
    fb_h = (int)h;
    fb_bpp = (int)bpp;
    fb_px = (int)(bpp / 8);
    fb_on = 1;
    return 1;
}

static inline void clip(int *x, int *y, int *w, int *h) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*w < 0) *w = 0;
    if (*h < 0) *h = 0;
    if (*x + *w > fb_w) *w = fb_w - *x;
    if (*y + *h > fb_h) *h = fb_h - *y;
}

void gfx_px(int x, int y, u32 rgb) {
    if (!fb_on || x < 0 || y < 0 || x >= fb_w || y >= fb_h) return;
    store_u32(x, y, rgb);
}

/* Row fill on the fast path: 32bpp writes two pixels per store, 24bpp one
 * pixel per 3-byte group, 16bpp one u16 (the console's scroll pays this). */
static void fill_row(int x, int y, int w, u32 rgb) {
    volatile u8 *p = fb + (u64)y * fb_pitch + (u64)x * (u64)fb_px;
    if (fb_bpp == 32) {
        u32 v = pack(rgb);
        u64 pair = ((u64)v << 32) | v;
        int i = 0;
        for (; i + 2 <= w; i += 2) *(volatile u64 *)(p + i * 4) = pair;
        if (i < w) *(volatile u32 *)(p + i * 4) = v;
    } else if (fb_bpp == 24) {
        for (int i = 0; i < w; i++) store_u32(x + i, y, rgb);
    } else {
        u16 v = (u16)pack(rgb);
        for (int i = 0; i < w; i++) *(volatile u16 *)(p + i * 2) = v;
    }
}

void gfx_fill(int x, int y, int w, int h, u32 rgb) {
    if (!fb_on) return;
    clip(&x, &y, &w, &h);
    for (int j = 0; j < h; j++) fill_row(x, y + j, w, rgb);
}

void gfx_hline(int x, int y, int w, u32 rgb) { gfx_fill(x, y, w, 1, rgb); }
void gfx_vline(int x, int y, int h, u32 rgb) { gfx_fill(x, y, 1, h, rgb); }

void gfx_frame(int x, int y, int w, int h, u32 rgb) {
    gfx_hline(x, y, w, rgb);
    gfx_hline(x, y + h - 1, w, rgb);
    gfx_vline(x, y, h, rgb);
    gfx_vline(x + w - 1, y, h, rgb);
}

/* Vertical gradient: one flat row per scanline, integer lerp (no FPU use —
 * the kernel keeps FPU state untouched outside the task layer). */
void gfx_vgrad(int x, int y, int w, int h, u32 top, u32 bot) {
    if (!fb_on || h <= 0) return;
    for (int j = 0; j < h; j++) {
        int t = (h > 1) ? (j * 256) / (h - 1) : 0;
        u32 r = (u32)((((top >> 16) & 0xFF) * (256 - t) + ((bot >> 16) & 0xFF) * t) >> 8);
        u32 g = (u32)((((top >> 8) & 0xFF) * (256 - t) + ((bot >> 8) & 0xFF) * t) >> 8);
        u32 b = (u32)(((top & 0xFF) * (256 - t) + (bot & 0xFF) * t) >> 8);
        int yy = y + j;
        if (yy < 0 || yy >= fb_h) continue;
        fill_row(x < 0 ? 0 : x,
                 yy,
                 (x + w > fb_w ? fb_w : x + w) - (x < 0 ? 0 : x),
                 (r << 16) | (g << 8) | b);
    }
}

/* Scroll a rect up by dy scanlines (console text scroll, M8 successor).
 * Copies with 8-byte stores; the source is always BELOW the destination, so
 * a forward loop is safe. */
void gfx_shift_up(int x, int y, int w, int h, int dy) {
    if (!fb_on || dy <= 0 || dy >= h) return;
    clip(&x, &y, &w, &h);
    u64 row_bytes = (u64)w * (u64)fb_px;
    for (int j = 0; j + dy < h; j++) {
        volatile u8 *d = fb + (u64)(y + j) * fb_pitch + (u64)x * (u64)fb_px;
        volatile u8 *s = d + (u64)dy * fb_pitch;
        u64 i = 0;
        for (; i + 8 <= row_bytes; i += 8)
            __asm__ __volatile__("mov (%0), %%rax\n\tmov %%rax, (%1)"
                                 : : "r"((const void *)(s + i)), "r"((void *)(d + i))
                                 : "rax", "memory");
        for (; i < row_bytes; i++) d[i] = s[i];
    }
}

/* One text cell. bg < 0 keeps the desktop behind the glyph (transparent). */
void gfx_cell(int x, int y, unsigned char ch, u32 fg, int bg) {
    if (!fb_on) return;
    for (int j = 0; j < 16; j++) {
        unsigned char bits = font8x16_data[ch][j];
        for (int i = 0; i < 8; i++) {
            if (bits & (0x80 >> i)) store_u32(x + i, y + j, fg);
            else if (bg >= 0) store_u32(x + i, y + j, (u32)bg);
        }
    }
}

/* Scaled cells draw each glyph pixel as a scale x scale block (title bars). */
void gfx_cell_scale(int x, int y, unsigned char ch, u32 fg, int bg, int scale) {
    if (!fb_on) return;
    if (scale <= 1) { gfx_cell(x, y, ch, fg, bg); return; }
    for (int j = 0; j < 16; j++) {
        unsigned char bits = font8x16_data[ch][j];
        for (int i = 0; i < 8; i++) {
            int ink = bits & (0x80 >> i);
            if (!ink && bg < 0) continue;
            gfx_fill(x + i * scale, y + j * scale, scale, scale,
                     ink ? fg : (u32)bg);
        }
    }
}

void gfx_text(int x, int y, const char *s, u32 fg, int bg) {
    for (; s && *s; s++, x += 8) gfx_cell(x, y, (unsigned char)*s, fg, bg);
}

void gfx_text_scale(int x, int y, const char *s, u32 fg, int bg, int scale) {
    for (; s && *s; s++, x += 8 * scale)
        gfx_cell_scale(x, y, (unsigned char)*s, fg, bg, scale);
}
