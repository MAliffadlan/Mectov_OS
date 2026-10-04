/* q3tga.h — Targa decoder for the Quake III asset paths (v38.150).
 *
 * id's tools and id's shipped art are both Targa: every UI picture in the
 * pak (menu/art/*.tga, gfx/2d/bigchars.tga, the crosshairs) and a slice of
 * the world texture set. The world renderer grew its own decoder inline for
 * the textures (q3world_render.c); the 2D UI path needs the same code AND the
 * alpha channel the world path throws away — a button frame's transparent
 * border is what lets the menu background show through it — so the decoder
 * lives here, once, with the output layout as a parameter.
 *
 * Scope, and it is deliberately narrow because it is exactly what the pak
 * contains (measured: 258 TGAs in demo pak0, ALL of them type 2 uncompressed
 * or type 10 RLE, all of them 32bpp; the largest is 256x256):
 *   - types 2 (uncompressed true-color) and 10 (RLE true-color);
 *   - 24 and 32 bits per pixel;
 *   - either origin (image descriptor bit 5);
 *   - a colour-mapped image (types 1/9) is REJECTED, not mis-decoded.
 * Retail Q3 art contains none, and silently treating a palette index as a
 * colour would draw garbage rather than nothing.
 *
 * Constraints this port places on the decoder: freestanding kernel (no libc,
 * no floating point, no malloc during decode — kmalloc for the one output
 * buffer, which the caller frees), 32-bit build, no 64-bit division.
 */
#ifndef Q3TGA_H
#define Q3TGA_H

/* Header-only probe: image size and pixel depth straight out of the 18-byte
 * footer, no decode. 0 with *w/*h/*bpp set; non-zero on anything this
 * decoder will not handle (values untouched then). */
int q3tga_dims(const unsigned char *raw, int len, int *w, int *h, int *bpp);

/* Decode to a freshly kmalloc'd, tightly packed, top-down image.
 *   stride 3: RGB888  (what the world texture path uploads to GL_RGB)
 *   stride 4: RGBA8888 (what the 2D UI path blends with)
 * Returns 0 with *out/*w/*h set and a buffer the caller kfree()s; non-zero
 * with nothing allocated. */
int q3tga_decode(const unsigned char *raw, int len, unsigned char **out,
                 int *w, int *h, int stride);

/* Hard ceiling on either side. Retail art is 256x256; the cap exists so a
 * corrupt or hostile header cannot ask the kernel for a gigabyte. */
#define Q3TGA_MAX_SIDE 1024

#endif /* Q3TGA_H */