/* q3jpeg.h — baseline JPEG decoder for the Q3 world texture path (v38.111).
 *
 * Retail Quake III's textures are mostly JPEG (the pak's TGA files are the
 * exception, not the rule). This decodes baseline sequential DCT, Huffman
 * coded JPEGs — exactly what id's texture pipeline shipped — into 24-bit RGB,
 * so the world renderer can upload them the same way it uploads its TGAs.
 *
 * Constraints this port places on the decoder, and how they are met:
 *   - freestanding kernel: no libc, no malloc during decode (the caller
 *     provides the output buffer), no floating point (integer IDCT, fixed
 *     point YCbCr -> RGB), no libgcc (no 64/64 division anywhere);
 *   - 32-bit build: all per-sample arithmetic fits in int;
 *   - small stack: the largest frame is one 8x8 block of ints (256 B).
 *
 * Scope, stated plainly: baseline (SOF0) only, Huffman only, no progressive
 * (SOF2), no arithmetic coding, no hierarchical. Retail textures never use
 * those; a file this cannot decode reports the failure and the texture path
 * falls back to the placeholder — visible, logged, never a crash.
 */
#ifndef Q3JPEG_H
#define Q3JPEG_H

#include <stdint.h>

/* Decode a baseline JPEG into RGB888 rows, top-down.
 *   data/len: the raw JPEG file bytes
 *   out:      caller-provided buffer, at least *out_w * *out_h * 3 bytes
 *   out_w/out_h: in/out. If they arrive > 0 they cap which size is accepted
 *               (the texture loader passes its expectations; 0/0 accepts any
 *               size up to Q3JPEG_MAX_SIDE and reports what was decoded).
 * Returns 0 on success with out filled and out_w/out_h set to the image
 * size; non-zero (a q3jpeg_err_* code) with nothing written on failure. */
int q3jpeg_decode(const unsigned char *data, int len,
                  unsigned char *out, int *out_w, int *out_h);

/* Dimensions-only: parse the marker chain (SOI .. SOF0) and report the
 * encoded size without entering the entropy-coded scan. The texture loader
 * uses this to size its output buffer, so the decode itself allocates nothing.
 * Returns 0 with *out_w/*out_h set; non-zero on failure (values untouched). */
int q3jpeg_dims(const unsigned char *data, int len, int *out_w, int *out_h);

#define Q3JPEG_MAX_SIDE 2048

/* Error codes (negative return from q3jpeg_decode). */
#define Q3JPEG_ERR_NO_SOI      1   /* no FFD8 marker */
#define Q3JPEG_ERR_TRUNCATED   2   /* data ended mid-stream */
#define Q3JPEG_ERR_NOT_BASELINE 3  /* SOF1/SOF2/... — only SOF0 is supported */
#define Q3JPEG_ERR_UNSUPPORTED 4   /* arithmetic coding, odd sampling, 16-bit
                                      precision — structurally not decodable */
#define Q3JPEG_ERR_BAD_MARKER  5   /* marker out of place */
#define Q3JPEG_ERR_DIM         6   /* size 0, over Q3JPEG_MAX_SIDE, or outside
                                      the caller's expected w/h */
#define Q3JPEG_ERR_NCOMP       7   /* component count not 1..3 */
#define Q3JPEG_ERR_HUFF        8   /* malformed Huffman table */
#define Q3JPEG_ERR_DATA        9   /* entropy-coded data ran out / bad scan */

#endif /* Q3JPEG_H */
