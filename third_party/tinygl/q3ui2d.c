/* q3ui2d.c — the 2D software rasterizer the official Quake III UI draws into.
 * See q3ui2d.h for why it exists and why 640x480. v38.150.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>   /* the q3 stub's memset/memcpy -> q3_memset/q3_memcpy */

#include "q3ui2d.h"
#include "q3jpeg.h"   /* the pak's .jpg pictures (levelshots, gfx/2d tiles) */
#include "q3tga.h"    /* and its .tga ones (menu/art, gfx/2d/bigchars)        */

extern void write_serial_string(const char *s);
extern void *kmalloc(uint32_t size);
extern void  kfree(void *p);

/* id's FS over the game volume, so a UI picture is read exactly the way the
 * world textures and the .bsp are: qpath-relative ("menu/art/main.tga"), from
 * /ext2/baseq3. Declared here rather than pulled in through a header because
 * q3bsp.h belongs to the map loader; these two functions are all the UI needs.
 * (v38.150: the Mectov VFS — vfs_read_file, node table and all — is a
 * DIFFERENT filesystem and cannot see /ext2 at all. A UI picture read through
 * it always came back empty, which is why the Mectov-styled menu's levelshots
 * never appeared.) */
extern int  q3bsp_read_file(const char *qpath, unsigned char **data, int *len);
extern void q3bsp_free_file(unsigned char *data);

/* ------------------------------------------------------------------ */
/* The surface.                                                        */
/* ------------------------------------------------------------------ */
static uint32_t *ui2d_fb;
static int ui2d_ready;

/* ------------------------------------------------------------------ */
/* Shader registry.                                                    */
/* ------------------------------------------------------------------ */
#define Q3UI_MAX_SHADERS 128
#define Q3UI_NAME_MAX    64
/* v38.150: handle 0 means NULL in id's own vocabulary — ui_syscalls' callers
 * and every UI screen do `if (!hShader) return;`, and UI_DrawHandlePic's
 * "nothing to draw" path IS the NULL path. So 0 is never handed out, a name
 * that resolves to no image returns 0 (draw nothing, exactly what retail does
 * with a failed shader), and the built-in "white" (id answers that name
 * without a file, and the UI paints every window frame with it) sits in a
 * reserved slot at the end. */
#define Q3UI_NULL_SH     0
#define Q3UI_WHITE_SH    (Q3UI_MAX_SHADERS - 1)
#define Q3UI_FIRST_FREE  1
#define Q3UI_LAST_FREE   (Q3UI_MAX_SHADERS - 2)
/* Cache ceiling. The menu's working set measured against the pak: about 30
 * pictures, the largest 256x256, so ~1.5 MB live; the budget leaves room for
 * the setup/controls screens (more button art) without letting a UI that
 * walks a huge list starve the kernel heap the game module lives in. */
#define Q3UI_BYTE_BUDGET (6 * 1024 * 1024)

typedef struct {
    char     name[Q3UI_NAME_MAX];
    int      w, h;
    uint8_t *rgba;          /* w*h*4, top-down, NULL when the name found nothing */
    unsigned stamp;         /* LRU clock */
} ui2d_shader;

static ui2d_shader ui2d_sh[Q3UI_MAX_SHADERS];
static int ui2d_clock;
static int ui2d_cached_bytes;
static int ui2d_missing;         /* requests that resolved to nothing */
static int ui2d_draws, ui2d_pixels, ui2d_rotated;

/* Built-in 1x1 white: id's renderer answers "white" without a file, and the UI
 * draws every menu frame and every scrollbar bar with it. It lives in the
 * reserved slot Q3UI_WHITE_SH, NOT in 0 — see the slot-layout comment above. */

static unsigned ui2d_hash(const char *s) {
    unsigned h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

/* id's extension order for a bare shader name (files.c: the image search tries
 * each in turn). .tga first because every UI picture the menu shows is a .tga;
 * the .jpg fallback is for the levelshots and the few gfx/2d tiles that ship as
 * JPEG. The bare name comes LAST, and that is not a style choice: the demo pak
 * has no .jpg for these, so probing the bare name first made every single
 * shader registration emit a "Can't find <name>" line from id's own
 * FS_FOpenFileRead before succeeding on the next extension — 20 lines of noise
 * per menu. */
static const char *const ui2d_exts[] = { ".tga", ".jpg", ".jpeg", "" };

static int ui2d_strip_ext(const char *name, char *out, int out_size) {
    static const char *const known[] = { ".tga", ".jpg", ".jpeg", ".png" };
    int n = 0, i;
    for (i = 0; name[i] && i < out_size - 5; i++) out[n++] = name[i];
    out[n] = '\0';
    for (i = 0; i < 4; i++) {
        int ln = n, el = 0;
        const char *e = known[i];
        while (e[el]) el++;
        if (ln > el) {
            int k, match = 1;
            for (k = 0; k < el; k++) {
                char a = out[ln - el + k], b = e[k];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                if (a != b) { match = 0; break; }
            }
            if (match) { out[ln - el] = '\0'; n = ln - el; break; }
        }
    }
    return n;
}

/* Decode a picture into the shader's cache slot. Returns 0 on success. */
static int ui2d_load_image(ui2d_shader *s, const char *base) {
    char path[128];
    unsigned char *raw;
    int len, i, rc = -1;

    for (i = 0; i < 4; i++) {
        int n = 0, k;
        for (k = 0; base[k] && n < (int)sizeof(path) - 8; k++) path[n++] = base[k];
        for (k = 0; ui2d_exts[i][k] && n < (int)sizeof(path) - 1; k++)
            path[n++] = ui2d_exts[i][k];
        path[n] = '\0';
        if (q3bsp_read_file(path, &raw, &len) == 0 && raw && len > 0) {
            int w = 0, h = 0;
            /* The pak's TGA art is 32bpp true-color (measured: 258 files, types
             * 2 and 10 only), so the 4-byte output keeps the alpha that the
             * button frames are cut with. */
            if (q3tga_decode(raw, len, (unsigned char **)&s->rgba, &w, &h, 4) == 0) {
                s->w = w; s->h = h;
                rc = 0;
            } else if (q3jpeg_dims(raw, len, &w, &h) == 0 && w > 0 && h > 0 &&
                       w <= 1024 && h <= 1024) {
                unsigned char *rgb = (unsigned char *)kmalloc((uint32_t)(w * h * 3));
                if (rgb) {
                    int ow = w, oh = h;
                    if (q3jpeg_decode(raw, len, rgb, &ow, &oh) == 0) {
                        int x, y;
                        s->rgba = (uint8_t *)kmalloc((uint32_t)(ow * oh * 4));
                        if (s->rgba) {
                            for (y = 0; y < oh; y++)
                                for (x = 0; x < ow; x++) {
                                    const unsigned char *p = rgb + (y * ow + x) * 3;
                                    uint8_t *d = s->rgba + (y * ow + x) * 4;
                                    d[0] = p[0]; d[1] = p[1]; d[2] = p[2];
                                    d[3] = 255;
                                }
                            s->w = ow; s->h = oh;
                            kfree(rgb);
                            rc = 0;
                        } else {
                            kfree(rgb);
                        }
                    } else {
                        kfree(rgb);
                    }
                }
            }
            q3bsp_free_file(raw);
            if (rc == 0) {
                ui2d_cached_bytes += s->w * s->h * 4;
                return 0;
            }
            if (s->rgba) { kfree(s->rgba); s->rgba = NULL; }
            s->w = s->h = 0;
        }
    }
    return -1;
}

/* Evict least-recently-used until the budget is met. The menu never churns
 * this hard; it exists so a pathological name list cannot walk into the game
 * module's heap. */
static void ui2d_evict_to_budget(void) {
    int i;
    while (ui2d_cached_bytes > Q3UI_BYTE_BUDGET) {
        int victim = -1;
        unsigned oldest = 0xFFFFFFFFu;
        for (i = Q3UI_FIRST_FREE; i <= Q3UI_LAST_FREE; i++) {
            if (!ui2d_sh[i].rgba) continue;
            if (ui2d_sh[i].stamp < oldest) { oldest = ui2d_sh[i].stamp; victim = i; }
        }
        if (victim < 0) break;
        ui2d_cached_bytes -= ui2d_sh[victim].w * ui2d_sh[victim].h * 4;
        kfree(ui2d_sh[victim].rgba);
        ui2d_sh[victim].rgba = NULL;
        ui2d_sh[victim].w = ui2d_sh[victim].h = 0;
        ui2d_sh[victim].name[0] = '\0';
    }
}

/* Case-insensitive compare, local rather than id's Q_stricmp: this file is
 * compiled with TinyGL's flags, which do not carry the Q3 include path (the
 * renderer TU works the same way — see q3w_ncaseeq in q3world_render.c). */
static int ui2d_stricmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
        if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
}

static int ui2d_find(const char *name) {
    int i;
    for (i = 0; i < Q3UI_MAX_SHADERS; i++)
        if (ui2d_sh[i].name[0] && ui2d_sh[i].stamp &&
            !ui2d_stricmp(ui2d_sh[i].name, name))
            return i;
    return -1;
}

int q3ui2d_register_shader(const char *name) {
    char base[Q3UI_NAME_MAX];
    int slot = -1, i;

    if (!ui2d_ready || !name || !name[0]) return 0;
    ui2d_strip_ext(name, base, Q3UI_NAME_MAX);

    i = ui2d_find(base);
    if (i >= 0) { ui2d_sh[i].stamp = ++ui2d_clock; return i; }

    /* Free slot, else evict the oldest non-built-in. */
    for (i = Q3UI_FIRST_FREE; i <= Q3UI_LAST_FREE; i++)
        if (!ui2d_sh[i].name[0]) { slot = i; break; }
    if (slot < 0) {
        int oldest = Q3UI_FIRST_FREE;
        unsigned ostamp = 0xFFFFFFFFu;
        for (i = Q3UI_FIRST_FREE; i <= Q3UI_LAST_FREE; i++)
            if (ui2d_sh[i].stamp && ui2d_sh[i].stamp < ostamp) {
                ostamp = ui2d_sh[i].stamp; oldest = i;
            }
        ui2d_cached_bytes -= ui2d_sh[oldest].w * ui2d_sh[oldest].h * 4;
        if (ui2d_sh[oldest].rgba) kfree(ui2d_sh[oldest].rgba);
        memset(&ui2d_sh[oldest], 0, sizeof(ui2d_shader));
        slot = oldest;
    }

    memset(&ui2d_sh[slot], 0, sizeof(ui2d_shader));
    for (i = 0; base[i] && i < Q3UI_NAME_MAX - 1; i++) ui2d_sh[slot].name[i] = base[i];
    ui2d_sh[slot].stamp = ++ui2d_clock;
    if (ui2d_load_image(&ui2d_sh[slot], base) != 0) {
        /* No such picture. Keep the NAME cached so the menu does not re-probe
         * the filesystem every frame, and answer NULL — which is what retail
         * hands back for a shader that did not resolve, and what every id UI
         * screen already tests for. The demo pak has no menuback /
         * menubacknologo (the big Quake III logo backgrounds live in the retail
         * paks), so the main menu paints its buttons, frames and text over an
         * empty background: id's own behaviour for a missing picture, not a
         * hole of our own making. */
        ui2d_missing++;
        ui2d_evict_to_budget();
        return Q3UI_NULL_SH;
    }
    ui2d_evict_to_budget();
    /* The budget pass may have evicted the slot we just filled. */
    return ui2d_sh[slot].rgba ? slot : Q3UI_NULL_SH;
}

/* ------------------------------------------------------------------ */
/* State + drawing.                                                    */
/* ------------------------------------------------------------------ */
static float ui2d_col[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
/* Q3's own colour coding runs 0..1 with some elements pushing above 1 for the
 * additive-ish highlights; clamp like the framebuffer does. */
static int ui2d_c(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

void q3ui2d_set_color(const float *rgba) {
    if (!rgba) {
        ui2d_col[0] = ui2d_col[1] = ui2d_col[2] = 1.0f;
        ui2d_col[3] = 1.0f;
        return;
    }
    ui2d_col[0] = rgba[0];
    ui2d_col[1] = rgba[1];
    ui2d_col[2] = rgba[2];
    ui2d_col[3] = rgba[3];
}

static uint32_t ui2d_src_px(void) {
    int r = ui2d_c((int)(ui2d_col[0] * 255.0f + 0.5f));
    int g = ui2d_c((int)(ui2d_col[1] * 255.0f + 0.5f));
    int b = ui2d_c((int)(ui2d_col[2] * 255.0f + 0.5f));
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* src-alpha blend of one pixel; `a` is 0..255. */
static void ui2d_blend(uint32_t *d, uint32_t src, int a) {
    if (a <= 0) return;
    if (a >= 255) { *d = src; return; }
    {
        uint32_t dr = (*d >> 16) & 0xFF, dg = (*d >> 8) & 0xFF, db = *d & 0xFF;
        uint32_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
        int r = (int)(sr * a + dr * (255 - a)) / 255;
        int g = (int)(sg * a + dg * (255 - a)) / 255;
        int b = (int)(sb * a + db * (255 - a)) / 255;
        *d = ((uint32_t)ui2d_c(r) << 16) | ((uint32_t)ui2d_c(g) << 8) |
             (uint32_t)ui2d_c(b);
    }
}

void q3ui2d_fill_rect(float x, float y, float w, float h) {
    int x0, y0, x1, y1, px, py;
    uint32_t src;
    int alpha;

    if (!ui2d_ready || w <= 0.0f || h <= 0.0f) return;
    x0 = (int)x; y0 = (int)y;
    x1 = (int)(x + w); y1 = (int)(y + h);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > Q3UI_W) x1 = Q3UI_W; if (y1 > Q3UI_H) y1 = Q3UI_H;
    if (x0 >= x1 || y0 >= y1) return;

    src = ui2d_src_px();
    alpha = ui2d_c((int)(ui2d_col[3] * 255.0f + 0.5f));
    for (py = y0; py < y1; py++) {
        uint32_t *row = ui2d_fb + (size_t)py * Q3UI_W;
        for (px = x0; px < x1; px++) {
            if (alpha >= 255) row[px] = src;
            else ui2d_blend(&row[px], src, alpha);
        }
    }
    ui2d_draws++;
    ui2d_pixels += (x1 - x0) * (y1 - y0);
}

void q3ui2d_draw_stretch_pic(float x, float y, float w, float h,
                             float s0, float t0, float s1, float t1,
                             int shader, float rotate) {
    ui2d_shader *sh;
    int x0, y0, x1, y1, px, py;
    uint32_t src;
    int alpha;

    if (rotate != 0.0f) ui2d_rotated++;
    if (!ui2d_ready || w <= 0.0f || h <= 0.0f) return;
    if (shader <= Q3UI_NULL_SH || shader >= Q3UI_MAX_SHADERS) return;
    sh = &ui2d_sh[shader];
    /* v38.150: THE crash this fixed. A name that resolves to no image still had
     * a slot (the menu caches the name so it does not re-probe every frame), so
     * w/h could be 0, and the bilinear neighbour index (x + 1, clamped to
     * w - 1) went to -1: the fetch then read at rgba - 4 and the loop walked
     * off the buffer. The demo pak has no menuback (the logo backgrounds are in
     * the retail paks), so the main menu's first frame hit this every time.
     * Zero-sized means nothing to sample — which is also exactly what id does
     * with a shader that did not load. */
    if (!sh->rgba || sh->w <= 0 || sh->h <= 0) return;

    x0 = (int)x; y0 = (int)y;
    x1 = (int)(x + w); y1 = (int)(y + h);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > Q3UI_W) x1 = Q3UI_W; if (y1 > Q3UI_H) y1 = Q3UI_H;
    if (x0 >= x1 || y0 >= y1) return;

    src = ui2d_src_px();
    alpha = ui2d_c((int)(ui2d_col[3] * 255.0f + 0.5f));

    {
        int dw = x1 - x0, dh = y1 - y0;
        int tw = sh->w, th = sh->h;
        /* Bilinear, because id samples UI pictures with GL_LINEAR (no mipmaps
         * are ever generated for them — that is what UI_R_REGISTERSHADERNOMIP
         * asks for) and a nearest-neighbour 2D upscale of button art is visibly
         * worse. */
        for (py = y0; py < y1; py++) {
            uint32_t *row = ui2d_fb + (size_t)py * Q3UI_W;
            float v = t0 + (t1 - t0) * (((float)(py - y0) + 0.5f) / (float)dh);
            float sy = v * (float)th - 0.5f;
            int iy = (int)sy;
            float wy = sy - (float)iy;
            int y0i = iy < 0 ? 0 : (iy >= th ? th - 1 : iy);
            int y1i = y0i + 1; if (y1i >= th) y1i = th - 1;
            if (y0i < 0) y0i = 0; if (y1i < 0) y1i = 0;
            for (px = x0; px < x1; px++) {
                float u = s0 + (s1 - s0) * (((float)(px - x0) + 0.5f) / (float)dw);
                float sx = u * (float)tw - 0.5f;
                int ix = (int)sx;
                float wx = sx - (float)ix;
                int x0i = ix < 0 ? 0 : (ix >= tw ? tw - 1 : ix);
                int x1i = x0i + 1; if (x1i >= tw) x1i = tw - 1;
                if (x0i < 0) x0i = 0; if (x1i < 0) x1i = 0;
                const uint8_t *p00 = sh->rgba + ((size_t)y0i * tw + x0i) * 4;
                const uint8_t *p10 = sh->rgba + ((size_t)y0i * tw + x1i) * 4;
                const uint8_t *p01 = sh->rgba + ((size_t)y1i * tw + x0i) * 4;
                const uint8_t *p11 = sh->rgba + ((size_t)y1i * tw + x1i) * 4;
                uint32_t rgb;
                int a;
                {
                    /* Weighted per channel: the UI art is alpha-cut art, so
                     * filtering RGB and A separately (then multiplying) is
                     * what keeps a scaled button frame from darkening at its
                     * edges the way a filtered RGBA quadruple would. */
                    int r = (int)(0.5f + ((p00[0] * (1 - wx) + p10[0] * wx) * (1 - wy) +
                                          (p01[0] * (1 - wx) + p11[0] * wx) * wy));
                    int g = (int)(0.5f + ((p00[1] * (1 - wx) + p10[1] * wx) * (1 - wy) +
                                          (p01[1] * (1 - wx) + p11[1] * wx) * wy));
                    int b = (int)(0.5f + ((p00[2] * (1 - wx) + p10[2] * wx) * (1 - wy) +
                                          (p01[2] * (1 - wx) + p11[2] * wx) * wy));
                    rgb = ((uint32_t)ui2d_c(r) << 16) | ((uint32_t)ui2d_c(g) << 8) |
                          (uint32_t)ui2d_c(b);
                    a = (int)(0.5f + ((p00[3] * (1 - wx) + p10[3] * wx) * (1 - wy) +
                                      (p01[3] * (1 - wx) + p11[3] * wx) * wy));
                    a = a * alpha / 255;
                }
                if (a >= 255) row[px] = rgb;
                else ui2d_blend(&row[px], rgb, a);
            }
        }
        ui2d_pixels += dw * dh;
    }
    ui2d_draws++;
    ui2d_sh[shader].stamp = ++ui2d_clock;
}

void q3ui2d_present(uint32_t *dst, int dw, int dh, int pitch) {
    int x, y;
    if (!ui2d_ready || !dst || dw <= 0 || dh <= 0) return;
    if (pitch < dw) pitch = dw;
    for (y = 0; y < dh; y++) {
        /* Source span for this destination row, area-average over it. At the
         * 2:1 this port presents, that is exactly a 2x2 box filter. */
        int sy0 = (int)((long)y * Q3UI_H / dh);
        int sy1 = (int)((long)(y + 1) * Q3UI_H / dh);
        uint32_t *d = dst + (size_t)y * pitch;
        int rows = sy1 - sy0;
        if (sy1 > Q3UI_H) sy1 = Q3UI_H;
        if (sy0 >= Q3UI_H) sy0 = Q3UI_H - 1;
        if (rows <= 0) rows = 1;
        for (x = 0; x < dw; x++) {
            int sx0 = (int)((long)x * Q3UI_W / dw);
            int sx1 = (int)((long)(x + 1) * Q3UI_W / dw);
            int cols = sx1 - sx0;
            long acc[3];
            int ry, rx;
            if (sx1 > Q3UI_W) sx1 = Q3UI_W;
            if (sx0 >= Q3UI_W) sx0 = Q3UI_W - 1;
            if (cols <= 0) cols = 1;
            acc[0] = acc[1] = acc[2] = 0;
            for (ry = sy0; ry < sy0 + rows; ry++) {
                const uint32_t *s = ui2d_fb + (size_t)ry * Q3UI_W;
                for (rx = sx0; rx < sx0 + cols; rx++) {
                    uint32_t p = s[rx];
                    acc[0] += (long)((p >> 16) & 0xFF);
                    acc[1] += (long)((p >> 8) & 0xFF);
                    acc[2] += (long)(p & 0xFF);
                }
            }
            {
                long n = (long)rows * cols;
                d[x] = ((uint32_t)ui2d_c((int)(acc[0] / n)) << 16) |
                       ((uint32_t)ui2d_c((int)(acc[1] / n)) << 8) |
                       (uint32_t)ui2d_c((int)(acc[2] / n));
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle + stats.                                                  */
/* ------------------------------------------------------------------ */

/* Per-frame accounting reset. The host calls this before UI_REFRESH so
 * q3ui2d_stats() describes the frame just drawn and not the whole session —
 * a missing picture or a rotation request has to be attributable to a moment,
 * and the draw/pixel totals are what the frame-cost budget is judged on. */
void q3ui2d_frame_reset(void) {
    ui2d_draws = 0;
    ui2d_pixels = 0;
    ui2d_rotated = 0;
}

int q3ui2d_init(void) {
    int i;
    if (ui2d_ready) return 0;
    ui2d_fb = (uint32_t *)kmalloc((uint32_t)((uint32_t)Q3UI_W * Q3UI_H * 4));
    if (!ui2d_fb) return -1;
    for (i = 0; i < (int)(sizeof(ui2d_fb[0]) * Q3UI_W * Q3UI_H); i++) ui2d_fb[i] = 0;
    memset(ui2d_sh, 0, sizeof(ui2d_sh));
    ui2d_clock = 0;
    ui2d_cached_bytes = 0;
    ui2d_missing = 0;
    ui2d_draws = ui2d_pixels = ui2d_rotated = 0;

    /* "white": id's renderer answers this name from a built-in, and the UI
     * paints every window frame with it. 1x1 opaque, 4 bytes. */
    ui2d_sh[Q3UI_WHITE_SH].rgba = (uint8_t *)kmalloc(4);
    if (!ui2d_sh[Q3UI_WHITE_SH].rgba) { kfree(ui2d_fb); ui2d_fb = NULL; return -1; }
    ui2d_sh[Q3UI_WHITE_SH].rgba[0] = 255;
    ui2d_sh[Q3UI_WHITE_SH].rgba[1] = 255;
    ui2d_sh[Q3UI_WHITE_SH].rgba[2] = 255;
    ui2d_sh[Q3UI_WHITE_SH].rgba[3] = 255;
    ui2d_sh[Q3UI_WHITE_SH].w = 1;
    ui2d_sh[Q3UI_WHITE_SH].h = 1;
    ui2d_sh[Q3UI_WHITE_SH].stamp = ++ui2d_clock;
    ui2d_sh[Q3UI_WHITE_SH].name[0] = 'w'; ui2d_sh[Q3UI_WHITE_SH].name[1] = 'h';
    ui2d_sh[Q3UI_WHITE_SH].name[2] = 'i'; ui2d_sh[Q3UI_WHITE_SH].name[3] = 't';
    ui2d_sh[Q3UI_WHITE_SH].name[4] = 'e'; ui2d_sh[Q3UI_WHITE_SH].name[5] = '\0';
    /* Opened with a plain black, which is what id's SCR_StartScreen leaves
     * before the first UI frame clears it. */
    ui2d_col[0] = ui2d_col[1] = ui2d_col[2] = ui2d_col[3] = 1.0f;
    ui2d_ready = 1;
    return 0;
}

void q3ui2d_shutdown(void) {
    int i;
    if (!ui2d_ready) return;
    for (i = 0; i < Q3UI_MAX_SHADERS; i++)
        if (ui2d_sh[i].rgba) kfree(ui2d_sh[i].rgba);
    memset(ui2d_sh, 0, sizeof(ui2d_sh));
    if (ui2d_fb) kfree(ui2d_fb);
    ui2d_fb = NULL;
    ui2d_ready = 0;
}

int  q3ui2d_ready(void) { return ui2d_ready; }
uint32_t *q3ui2d_surface(void) { return ui2d_fb; }

void q3ui2d_stats(int *shaders, int *images, int *cachedBytes, int *draws,
                  int *pixels, int *missing, int *rotated) {
    int i, live = 0;
    for (i = 0; i < Q3UI_MAX_SHADERS; i++)
        if (ui2d_sh[i].rgba) live++;
    if (shaders) *shaders = live;
    if (images) *images = live;
    if (cachedBytes) *cachedBytes = ui2d_cached_bytes;
    if (draws) *draws = ui2d_draws;
    if (pixels) *pixels = ui2d_pixels;
    if (missing) *missing = ui2d_missing;
    if (rotated) *rotated = ui2d_rotated;
}