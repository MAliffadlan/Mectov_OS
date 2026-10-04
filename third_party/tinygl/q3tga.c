/* q3tga.c — Targa decoder for the Quake III asset paths (v38.150).
 * See q3tga.h for scope. The decode is the one q3world_render.c grew inline
 * for world textures, generalized over the output layout so the 2D UI path
 * can keep the alpha channel; q3world_render.c now calls through here.
 */
#include <stdint.h>
#include "q3tga.h"

extern void *kmalloc(uint32_t size);
extern void  kfree(void *p);

/* Shared header parse. Returns 0 when the image is one this file decodes. */
static int tga_header(const unsigned char *raw, int len, int *id_len, int *type,
                      int *w, int *h, int *bytes, int *top_down) {
    if (len < 18) return 1;
    if (raw[1] != 0) return 2;                 /* colour-mapped: rejected */
    *type   = raw[2];
    *w      = raw[12] | (raw[13] << 8);
    *h      = raw[14] | (raw[15] << 8);
    *bytes  = raw[16] >> 3;
    *id_len = raw[0];
    *top_down = (raw[17] & 0x20) ? 1 : 0;
    if ((*type != 2 && *type != 10) || *w <= 0 || *h <= 0 ||
        *w > Q3TGA_MAX_SIDE || *h > Q3TGA_MAX_SIDE ||
        (*bytes != 3 && *bytes != 4)) {
        return 3;
    }
    return 0;
}

int q3tga_dims(const unsigned char *raw, int len, int *w, int *h, int *bpp) {
    int id_len, type, ww, hh, bytes, top;
    if (tga_header(raw, len, &id_len, &type, &ww, &hh, &bytes, &top) != 0)
        return 1;
    if (w) *w = ww;
    if (h) *h = hh;
    if (bpp) *bpp = bytes * 8;
    return 0;
}

int q3tga_decode(const unsigned char *raw, int len, unsigned char **out,
                 int *w, int *h, int stride) {
    int id_len, type, ww, hh, bytes, top;
    const unsigned char *p, *src;
    unsigned char *rgb;
    int x, y, total;

    if (stride != 3 && stride != 4) return 1;
    if (tga_header(raw, len, &id_len, &type, &ww, &hh, &bytes, &top) != 0)
        return 2;
    if ((long)ww * hh * stride > 0x7FFFFFFFL) return 3;

    src = raw + 18 + id_len;
    if (src - raw > len) return 4;

    rgb = (unsigned char *)kmalloc((uint32_t)((long)ww * hh * stride));
    if (!rgb) return 5;

    /* Pixel writer: Targa stores BGR(A); both output layouts above are
     * R,G,B(,A). `dst_row` is already flipped for the file's origin. */
#define TGA_PUT(px, row, col) do {                                  \
        const unsigned char *s = (px);                              \
        unsigned char *d = rgb + ((long)(row) * ww + (col)) * stride;\
        d[0] = s[2]; d[1] = s[1]; d[2] = s[0];                      \
        if (stride == 4) d[3] = s[3];                               \
    } while (0)

    if (type == 2) {                                   /* uncompressed */
        if (src - raw + (long)ww * hh * bytes > len) { kfree(rgb); return 6; }
        for (y = 0; y < hh; y++) {
            int row = top ? y : (hh - 1 - y);
            for (x = 0; x < ww; x++)
                TGA_PUT(src + ((long)y * ww + x) * bytes, row, x);
        }
    } else {                                           /* RLE */
        total = ww * hh;
        p = src;
        for (x = 0; x < total; ) {
            int count, rep, i;
            unsigned char px[4];
            if (p - raw >= len) { kfree(rgb); return 7; }
            count = *p++;
            rep = count & 0x80;
            count = (count & 0x7F) + 1;
            if (x + count > total) { kfree(rgb); return 8; }
            if (rep) {                               /* one pixel, repeated */
                if (p - raw + bytes > len) { kfree(rgb); return 7; }
                px[0] = p[0]; px[1] = p[1]; px[2] = p[2]; px[3] = p[3];
                p += bytes;
                for (i = 0; i < count; i++, x++)
                    TGA_PUT(px, top ? (x / ww) : (hh - 1 - x / ww), x % ww);
            } else {                                 /* count literal pixels */
                for (i = 0; i < count; i++, x++) {
                    if (p - raw + bytes > len) { kfree(rgb); return 7; }
                    TGA_PUT(p, top ? (x / ww) : (hh - 1 - x / ww), x % ww);
                    p += bytes;
                }
            }
        }
    }
#undef TGA_PUT

    *out = rgb;
    *w = ww;
    *h = hh;
    return 0;
}