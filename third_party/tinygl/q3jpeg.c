/* q3jpeg.c — baseline JPEG decoder (v38.111). See q3jpeg.h for the contract.
 *
 * Layout of this file:
 *   1. fixed-point tables (cosine in Q15, C(u) folded in Q16)
 *   2. the separable 8x8 IDCT — NOT the clever AAN form. The straightforward
 *      separable transform is 2x1024 multiply-accumulates per block, which at
 *      64x64 textures (64 blocks) is noise even under TCG, and it can be
 *      verified numerically against the reference formula rather than trusted.
 *      Accumulators are long long: 32x32->64 multiply is a single i686
 *      instruction, and the kernel only ever misses libgcc for 64-bit DIVIDE.
 *   3. Huffman table setup and the bit reader (FF00 stuffing, RSTn handling)
 *   4. marker parsing: SOI/APPn/COM/DQT/SOF0/DHT/DRI/SOS
 *   5. the MCU loop with 4:4:4 / 4:2:2 / 4:2:0 chroma placement
 *   6. YCbCr -> RGB in fixed point; grayscale passes through
 *
 * State: everything lives in one context struct; the only heap use is the
 * component-plane scratch (one kmalloc, one kfree per decode), owned by the
 * caller's budget. The output is written top-down; JPEG's MCU grid is already
 * top-down, so there is no flip (TGA's bottom-up convention does not apply).
 */
#include <stdint.h>
#include <stddef.h>
#include "q3jpeg.h"

extern void *kmalloc(unsigned int size);
extern void  kfree(void *p);

#define ZIGZAG_N 64

/* ------------------------------------------------------------------ */
/* fixed-point tables                                                 */
/* ------------------------------------------------------------------ */

/* COSQ[u][x] = round(32768 * cos((2x+1) * u * pi / 16)), u = row (frequency),
 * x = column (sample). Row 0 is the DC basis. Values generated with:
 *   python3 -c "import math; [print(round(32768*math.cos((2*x+1)*u*math.pi/16))) for u in range(8) for x in range(8)]"
 */
static const int COSQ[8][8] = {
    { 32768,  32768,  32768,  32768,  32768,  32768,  32768,  32768 },
    { 32138,  27246,  18205,   6393,  -6393, -18205, -27246, -32138 },
    { 30274,  12540, -12540, -30274, -30274, -12540,  12540,  30274 },
    { 27246,  -6393, -32138, -18205,  18205,  32138,   6393, -27246 },
    { 23170, -23170, -23170,  23170,  23170, -23170, -23170,  23170 },
    { 18205, -32138,   6393,  27246, -27246,  -6393,  32138, -18205 },
    { 12540, -30274,  30274, -12540, -12540,  30274, -30274,  12540 },
    {  6393, -18205,  27246, -32138,  32138, -27246,  18205,  -6393 }
};

/* C(u) in Q16: C(0) = 1/sqrt(2), C(u>0) = 1.0 (= 65536 in Q16). */
#define C0Q16 46341
#define CKQ16 65536

static const int zigzag[ZIGZAG_N] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* ------------------------------------------------------------------ */
/* decoder context                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    int16_t qt[4][ZIGZAG_N];        /* quant tables, zig-zag order */

    struct {
        unsigned char counts[17];   /* counts[1..16] */
        unsigned char values[256];
        int mincode[17], maxcode[17], valptr[17];
        int nvals;
    } huff[2][4];                   /* [class 0=DC 1=AC][slot] */

    int ncomp;
    struct {
        int id, h, v, tq, td, ta;
        int dc_pred;
    } comp[3];

    int width, height;
    int mcus_w, mcus_h, mcu_w, mcu_h;
    int restart_interval;

    const unsigned char *data;
    int len, pos;
    unsigned int bitbuf;
    int bitcnt;
    int eoi;                        /* a terminating marker was hit */
} jdec;

/* ------------------------------------------------------------------ */
/* bit reader                                                         */
/* ------------------------------------------------------------------ */

static void bits_fill(jdec *j) {
    while (j->bitcnt <= 24) {
        unsigned int b;
        if (j->pos >= j->len) {
            /* the spec's fill convention: pad with 1-bits past the end */
            j->bitbuf = (j->bitbuf << 8) | 0xFFu;
            j->bitcnt += 8;
            continue;
        }
        b = j->data[j->pos++];
        if (b == 0xFF) {
            unsigned char n = (j->pos < j->len) ? j->data[j->pos] : 0xD9;
            if (n == 0x00) {
                j->pos++;                    /* FF00 stuffing: literal FF */
            } else if (n >= 0xD0 && n <= 0xD7) {
                j->pos++;                    /* RSTn: consumed here */
                j->bitbuf = (j->bitbuf << 8) | n;
                j->bitcnt += 8;
                continue;                    /* value bits never read from it */
            } else {
                j->eoi = 1;                  /* EOI/anything else: stop scan */
                j->pos++;                    /* consume the marker byte */
                j->bitbuf = (j->bitbuf << 8) | 0xFFu;
                j->bitcnt += 8;
                continue;
            }
        }
        j->bitbuf = (j->bitbuf << 8) | b;
        j->bitcnt += 8;
    }
}

static int bits_get(jdec *j, int n) {
    int v;
    if (n == 0) return 0;
    if (j->bitcnt < n) bits_fill(j);
    j->bitcnt -= n;
    v = (int)(j->bitbuf >> j->bitcnt) & ((1 << n) - 1);
    return v;
}

static int bits_bit(jdec *j) {
    if (j->bitcnt < 1) bits_fill(j);
    j->bitcnt--;
    return (int)(j->bitbuf >> j->bitcnt) & 1;
}

/* ------------------------------------------------------------------ */
/* huffman                                                            */
/* ------------------------------------------------------------------ */

static int huff_build(jdec *j, int cls, int slot) {
    int code = 0, k = 0, i, total = 0;
    for (i = 1; i <= 16; i++) {
        total += j->huff[cls][slot].counts[i];
        j->huff[cls][slot].valptr[i] = k;
        j->huff[cls][slot].mincode[i] = code;
        code += j->huff[cls][slot].counts[i];
        k += j->huff[cls][slot].counts[i];
        j->huff[cls][slot].maxcode[i] = code - 1;
        code <<= 1;
    }
    j->huff[cls][slot].nvals = total;
    return (total <= 256) ? 0 : Q3JPEG_ERR_HUFF;
}

static int huff_decode_sym(jdec *j, int cls, int slot) {
    int code = 0, l;
    if (j->huff[cls][slot].nvals <= 0) return -1;
    for (l = 1; l <= 16; l++) {
        code = (code << 1) | bits_bit(j);
        /* NOTE: no eoi check here. bits_fill sets eoi while PREFETCHING past
         * the scan end, which can happen several bits before the current
         * symbol finishes; aborting mid-symbol would cut off a valid final
         * EOB. The MCU loop's !j.eoi check stops the scan at block
         * granularity instead, which is the correct place. */
        /* Both bounds matter: maxcode alone also matches an IN-PROGRESS code
         * (code below mincode of this length), which sends valptr+code-mincode
         * negative and corrupts the block. The tiny8 fixture failed exactly
         * this way before the mincode guard was added. */
        if (code >= j->huff[cls][slot].mincode[l] &&
            code <= j->huff[cls][slot].maxcode[l]) {
            int idx = j->huff[cls][slot].valptr[l] + code
                    - j->huff[cls][slot].mincode[l];
            if (idx < 0 || idx >= j->huff[cls][slot].nvals) return -1;
            return j->huff[cls][slot].values[idx];
        }
    }
    return -1;
}

static int huff_extend(int v, int n) {
    if (n == 0) return 0;
    if (v < (1 << (n - 1))) return v - (1 << n) + 1;
    return v;
}

/* ------------------------------------------------------------------ */
/* the separable IDCT                                                 */
/* ------------------------------------------------------------------ */

/* One 1-D pass over 8 samples. `in` holds frequency-domain samples (already
 * dequantized, raw scale), `out` gets spatial samples at the same effective
 * scale divided by 4 per dimension — the caller applies the (1/4)(1/4) of the
 * JPEG formula as >>2 in each pass. Accumulate in long long. */
static void idct_1d(const int *in, int *out) {
    int x, u;
    for (x = 0; x < 8; x++) {
        long long acc = 0;
        for (u = 0; u < 8; u++) {
            long long t = (long long)in[u] * COSQ[u][x];   /* s * cos (Q15) */
            acc += ((t + 16384) >> 15) * ((u == 0) ? C0Q16 : CKQ16); /* *C(u) Q16, rounded */
        }
        out[x] = (int)((acc + 32768) >> 16);               /* drop C(u) Q16, rounded */
    }
    /* NOTE: the caller folds the formula's overall 1/4 per dimension by
     * right-shifting the SECOND pass output by 2; the first pass output is
     * kept unscaled so the second pass sees correct magnitudes. */
}

static void idct_block(const int16_t *qtab, const int coef[ZIGZAG_N],
                       unsigned char out[64]) {
    int x[ZIGZAG_N];
    int col[8], row[8], tmp[8][8];
    int i, j;

    for (i = 0; i < ZIGZAG_N; i++) {
        int zz = zigzag[i];
        x[zz] = coef[i] * qtab[i];          /* raw dequantized coefficients */
    }

    for (i = 0; i < 8; i++) {               /* rows: frequency u -> spatial x */
        idct_1d(&x[i * 8], row);
        for (j = 0; j < 8; j++) tmp[i][j] = row[j];
    }
    for (i = 0; i < 8; i++) {               /* cols: frequency v -> spatial y */
        for (j = 0; j < 8; j++) col[j] = tmp[j][i];
        idct_1d(col, row);
        for (j = 0; j < 8; j++) {
            int v = ((row[j] + 2) >> 2) + 128;    /* the formula's overall 1/4, rounded */
            if (v < 0) v = 0;
            else if (v > 255) v = 255;
            out[j * 8 + i] = (unsigned char)v;
        }
    }
}

/* ------------------------------------------------------------------ */
/* markers                                                            */
/* ------------------------------------------------------------------ */

static int next_marker(jdec *j) {
    while (j->pos + 1 < j->len) {
        if (j->data[j->pos] != 0xFF) { j->pos++; continue; }
        {
            unsigned char n = j->data[j->pos + 1];
            if (n == 0xFF) { j->pos++; continue; }
            j->pos += 2;
            return n;
        }
    }
    return -1;
}

static int read_length(jdec *j) {
    int l;
    if (j->pos + 2 > j->len) return -1;
    l = (j->data[j->pos] << 8) | j->data[j->pos + 1];
    j->pos += 2;
    return l;
}

static int parse_dqt(jdec *j) {
    int seg = read_length(j);
    if (seg < 2 || j->pos + seg - 2 > j->len) return Q3JPEG_ERR_TRUNCATED;
    while (seg >= 65) {
        int slot = j->data[j->pos] & 15;
        int i;
        if (j->data[j->pos] >> 4) return Q3JPEG_ERR_UNSUPPORTED;  /* 16-bit Pq */
        if (slot > 3) return Q3JPEG_ERR_BAD_MARKER;
        j->pos++;
        for (i = 0; i < ZIGZAG_N; i++) j->qt[slot][i] = j->data[j->pos + i];
        j->pos += 64;
        seg -= 65;
    }
    return 0;
}

static int parse_dht(jdec *j) {
    int seg = read_length(j);
    if (seg < 2 || j->pos + seg - 2 > j->len) return Q3JPEG_ERR_TRUNCATED;
    while (seg > 17) {
        int tc = j->data[j->pos] >> 4;
        int th = j->data[j->pos] & 15;
        int total = 0, i, rc;
        j->pos++;
        if (tc > 1 || th > 3) return Q3JPEG_ERR_UNSUPPORTED;
        for (i = 1; i <= 16; i++) {
            j->huff[tc][th].counts[i] = j->data[j->pos + i - 1];
            total += j->huff[tc][th].counts[i];
        }
        j->pos += 16;
        if (total > 256 || j->pos + total > j->len) return Q3JPEG_ERR_HUFF;
        for (i = 0; i < total; i++)
            j->huff[tc][th].values[i] = j->data[j->pos + i];
        j->pos += total;
        seg -= 1 + 16 + total;
        rc = huff_build(j, tc, th);
        if (rc) return rc;
    }
    return 0;
}

static int parse_sof0(jdec *j) {
    int seg = read_length(j), i;
    if (seg < 6 || j->pos + seg - 2 > j->len) return Q3JPEG_ERR_TRUNCATED;
    if (j->data[j->pos] != 8) return Q3JPEG_ERR_UNSUPPORTED;
    j->height = (j->data[j->pos + 1] << 8) | j->data[j->pos + 2];
    j->width  = (j->data[j->pos + 3] << 8) | j->data[j->pos + 4];
    j->ncomp  = j->data[j->pos + 5];
    j->pos += 6;
    if (j->ncomp != 1 && j->ncomp != 3) return Q3JPEG_ERR_NCOMP;
    if (seg != 8 + j->ncomp * 3) return Q3JPEG_ERR_BAD_MARKER;  /* len counts itself */
    for (i = 0; i < j->ncomp; i++) {
        j->comp[i].id = j->data[j->pos];
        j->comp[i].h  = j->data[j->pos + 1] >> 4;
        j->comp[i].v  = j->data[j->pos + 1] & 15;
        j->comp[i].tq = j->data[j->pos + 2];
        j->pos += 3;
        if (j->comp[i].h < 1 || j->comp[i].h > 2 ||
            j->comp[i].v < 1 || j->comp[i].v > 2)
            return Q3JPEG_ERR_UNSUPPORTED;
        if (j->comp[i].tq > 3) return Q3JPEG_ERR_BAD_MARKER;
    }
    if (j->width <= 0 || j->height <= 0 ||
        j->width > Q3JPEG_MAX_SIDE || j->height > Q3JPEG_MAX_SIDE)
        return Q3JPEG_ERR_DIM;
    return 0;
}

static int parse_sos(jdec *j) {
    int seg = read_length(j), ns, i;
    if (seg < 4 || j->pos + seg - 2 > j->len) return Q3JPEG_ERR_TRUNCATED;
    ns = j->data[j->pos];
    if (ns != j->ncomp) return Q3JPEG_ERR_UNSUPPORTED;
    j->pos++;
    for (i = 0; i < ns; i++) {
        int cs = j->data[j->pos];
        int td = j->data[j->pos + 1] >> 4;
        int ta = j->data[j->pos + 1] & 15;
        j->pos += 2;
        if (cs != j->comp[i].id) return Q3JPEG_ERR_BAD_MARKER;
        if (td > 3 || ta > 3) return Q3JPEG_ERR_BAD_MARKER;
        j->comp[i].td = td;
        j->comp[i].ta = ta;
    }
    j->pos += 3;    /* Ss=0, Se=63, Ah/Al=0 in baseline */
    return 0;
}

/* ------------------------------------------------------------------ */
/* MCU decoding                                                       */
/* ------------------------------------------------------------------ */

static int decode_block(jdec *j, int c, int coef[ZIGZAG_N]) {
    int s, k;
    for (k = 0; k < ZIGZAG_N; k++) coef[k] = 0;

    s = huff_decode_sym(j, 0, j->comp[c].td);
    if (s < 0) return j->eoi ? -1 : Q3JPEG_ERR_DATA;
    if (s > 15) return Q3JPEG_ERR_DATA;
    j->comp[c].dc_pred += huff_extend(bits_get(j, s), s);
    coef[0] = j->comp[c].dc_pred;

    k = 1;
    while (k < ZIGZAG_N) {
        int rs = huff_decode_sym(j, 1, j->comp[c].ta);
        int r, v;
        if (rs < 0) return j->eoi ? -1 : Q3JPEG_ERR_DATA;
        r = rs >> 4;
        s = rs & 15;
        if (s == 0) {
            if (r == 15) { k += 16; continue; }
            break;                          /* EOB */
        }
        k += r;
        if (k >= ZIGZAG_N) return Q3JPEG_ERR_DATA;
        v = huff_extend(bits_get(j, s), s);
        coef[k] = v;
        k++;
    }
    return 0;
}

/* Dimensions-only query (see q3jpeg.h). Walks the same marker chain the
 * decoder walks — the parsing helpers above are shared — but returns the
 * moment SOF0 has been read. Encoders are free to place DQT/DHT before or
 * after SOF0, so both orders pass through here; a scan marker before any
 * frame header is a truncated stream. */
int q3jpeg_dims(const unsigned char *data, int len, int *out_w, int *out_h) {
    static jdec j;
    int rc, i;

    if (!data || len < 4 || !out_w || !out_h) return Q3JPEG_ERR_BAD_MARKER;
    if (data[0] != 0xFF || data[1] != 0xD8) return Q3JPEG_ERR_NO_SOI;
    {
        char *p = (char *)&j;
        for (i = 0; i < (int)sizeof(j); i++) p[i] = 0;
    }
    j.data = data;
    j.len = len;
    j.pos = 2;

    for (;;) {
        int m = next_marker(&j);
        if (m < 0) return Q3JPEG_ERR_TRUNCATED;
        if (m == 0xC0) {
            rc = parse_sof0(&j);
            if (rc) return rc;
            *out_w = j.width;
            *out_h = j.height;
            return 0;
        } else if ((m >= 0xC1 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC)) {
            return Q3JPEG_ERR_NOT_BASELINE;
        } else if (m == 0xC4) {
            rc = parse_dht(&j);
            if (rc) return rc;
        } else if (m == 0xDB) {
            rc = parse_dqt(&j);
            if (rc) return rc;
        } else if (m == 0xDD) {
            int seg = read_length(&j);
            if (seg != 4 || j.pos + 2 > j.len) return Q3JPEG_ERR_BAD_MARKER;
            j.pos += 2;
        } else if (m == 0xC8) {
            return Q3JPEG_ERR_UNSUPPORTED;
        } else if (m == 0xDA || m == 0xD9) {
            return Q3JPEG_ERR_TRUNCATED;
        } else if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            /* standalone */
        } else {
            int seg = read_length(&j);      /* APPn / COM: skip */
            if (seg < 2 || j.pos + seg - 2 > j.len) return Q3JPEG_ERR_TRUNCATED;
            j.pos += seg - 2;
        }
    }
}

/* fixed point YCbCr -> RGB (JPEG J.4 constants, Q16) */
static void ycbcr_rgb(int Y, int Cb, int Cr, unsigned char *px) {
    int cb = Cb - 128, cr = Cr - 128;
    int r = Y             + ((cr * 91881) >> 16);
    int g = Y - (((cb * 22554) + (cr * 46802)) >> 16);
    int b = Y + ((cb * 116130) >> 16);
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    px[0] = (unsigned char)r; px[1] = (unsigned char)g; px[2] = (unsigned char)b;
}

/* Half-pixel-centered separable bilinear chroma upsampling — the "fancy"
 * filter libjpeg/PIL use, in Q8 weights per axis (0..4 of 4). The plane is
 * MCU-padded (>= one full MCU beyond the image on every subsampled axis),
 * and every index is clamped anyway, so no read can go out of bounds.
 * Full-resolution chroma degenerates to a direct sample. */
static int chroma_at(const jdec *j, const unsigned char *plane, int pstride,
                     int x, int y, int hmax, int vmax) {
    int h = j->comp[1].h, v = j->comp[1].v;
    int cw = j->mcus_w * 8 * h;
    int ch = j->mcus_h * 8 * v;
    int cx0, cx1, cy0, cy1, wx0, wx1, wy0, wy1;
    int c00, c10, c01, c11;

    if (h == hmax) { cx0 = cx1 = x; wx0 = 4; wx1 = 0; }
    else {
        int sxq = 2 * x - 1;                 /* source pos in quarter-steps */
        int x0 = ((x + 1) >> 1) - 1;
        if (x0 < 0) x0 = 0; else if (x0 > cw - 1) x0 = cw - 1;
        cx0 = x0;
        cx1 = (x0 + 1 <= cw - 1) ? x0 + 1 : cw - 1;
        wx1 = sxq - 4 * x0;                  /* 0..4 */
        if (wx1 < 0) wx1 = 0; else if (wx1 > 4) wx1 = 4;
        wx0 = 4 - wx1;
    }
    if (v == vmax) { cy0 = cy1 = y; wy0 = 4; wy1 = 0; }
    else {
        int syq = 2 * y - 1;
        int y0 = ((y + 1) >> 1) - 1;
        if (y0 < 0) y0 = 0; else if (y0 > ch - 1) y0 = ch - 1;
        cy0 = y0;
        cy1 = (y0 + 1 <= ch - 1) ? y0 + 1 : ch - 1;
        wy1 = syq - 4 * y0;
        if (wy1 < 0) wy1 = 0; else if (wy1 > 4) wy1 = 4;
        wy0 = 4 - wy1;
    }
    if (wx1 == 0 && wy1 == 0) return plane[(size_t)cy0 * pstride + cx0];
    c00 = plane[(size_t)cy0 * pstride + cx0];
    c10 = plane[(size_t)cy0 * pstride + cx1];
    c01 = plane[(size_t)cy1 * pstride + cx0];
    c11 = plane[(size_t)cy1 * pstride + cx1];
    return (wx0 * wy0 * c00 + wx1 * wy0 * c10
          + wx0 * wy1 * c01 + wx1 * wy1 * c11 + 8) >> 4;
}

/* ------------------------------------------------------------------ */
/* top level                                                          */
/* ------------------------------------------------------------------ */

int q3jpeg_decode(const unsigned char *data, int len,
                  unsigned char *out, int *out_w, int *out_h) {
    static jdec j;                      /* ~3 KB of state, single-threaded */
    int rc, c, i;
    int hmax = 1, vmax = 1;
    int pw[3], ph[3], pstride[3];
    unsigned char *plane[3], *scratch;
    int mcu_w_blocks[3], my, mx;
    int restart_left;

    if (!data || len < 4 || !out_w || !out_h) return Q3JPEG_ERR_BAD_MARKER;
    if (data[0] != 0xFF || data[1] != 0xD8) return Q3JPEG_ERR_NO_SOI;

    {
        char *p = (char *)&j;
        for (i = 0; i < (int)sizeof(j); i++) p[i] = 0;
    }
    j.data = data;
    j.len = len;
    j.pos = 2;

    for (;;) {
        int m = next_marker(&j);
        if (m < 0) return Q3JPEG_ERR_TRUNCATED;
        if (m == 0xC0) {
            rc = parse_sof0(&j);
            if (rc) return rc;
        } else if ((m >= 0xC1 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC)) {
            return Q3JPEG_ERR_NOT_BASELINE;
        } else if (m == 0xC4) {
            rc = parse_dht(&j);
            if (rc) return rc;
        } else if (m == 0xDB) {
            rc = parse_dqt(&j);
            if (rc) return rc;
        } else if (m == 0xDD) {
            int seg = read_length(&j);
            if (seg != 4 || j.pos + 2 > j.len) return Q3JPEG_ERR_BAD_MARKER;
            j.restart_interval = (j.data[j.pos] << 8) | j.data[j.pos + 1];
            j.pos += 2;
        } else if (m == 0xC8) {
            return Q3JPEG_ERR_UNSUPPORTED;
        } else if (m == 0xDA) {
            rc = parse_sos(&j);
            if (rc) return rc;
            break;
        } else if (m == 0xD9) {
            return Q3JPEG_ERR_TRUNCATED;
        } else if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            /* standalone */
        } else {
            int seg = read_length(&j);      /* APPn / COM: skip */
            if (seg < 2 || j.pos + seg - 2 > j.len) return Q3JPEG_ERR_TRUNCATED;
            j.pos += seg - 2;
        }
    }

    if (*out_w > 0 && *out_w != j.width) return Q3JPEG_ERR_DIM;
    if (*out_h > 0 && *out_h != j.height) return Q3JPEG_ERR_DIM;
    *out_w = j.width;
    *out_h = j.height;
    /* Dimensions-only query: callers that must size the output buffer first
     * (the texture loader reads the SOF, allocates w*h*3, then decodes) pass
     * out == NULL and stop here — the marker chain is parsed, the scan is not
     * entered. */
    if (!out) return 0;

    for (c = 0; c < j.ncomp; c++) {
        if (j.comp[c].h > hmax) hmax = j.comp[c].h;
        if (j.comp[c].v > vmax) vmax = j.comp[c].v;
    }
    j.mcu_w = 8 * hmax;
    j.mcu_h = 8 * vmax;
    j.mcus_w = (j.width  + j.mcu_w - 1) / j.mcu_w;
    j.mcus_h = (j.height + j.mcu_h - 1) / j.mcu_h;

    /* one scratch holding every component's MCU-padded plane */
    {
        int total = 0;
        unsigned char *p;
        for (c = 0; c < j.ncomp; c++) {
            pw[c] = j.mcus_w * 8 * j.comp[c].h;
            ph[c] = j.mcus_h * 8 * j.comp[c].v;
            pstride[c] = pw[c];
            total += pstride[c] * ph[c];
        }
        scratch = (unsigned char *)kmalloc((unsigned int)total);
        if (!scratch) return Q3JPEG_ERR_DATA;
        p = scratch;
        for (c = 0; c < j.ncomp; c++) {
            plane[c] = p;
            p += (size_t)pstride[c] * ph[c];
        }
    }
    for (c = 0; c < j.ncomp; c++) mcu_w_blocks[c] = j.comp[c].h;

    /* ---- the entropy-coded scan ---- */
    for (i = 0; i < j.ncomp; i++) j.comp[i].dc_pred = 0;
    restart_left = j.restart_interval ? j.restart_interval
                                      : j.mcus_w * j.mcus_h;
    for (my = 0; my < j.mcus_h; my++) {
        for (mx = 0; mx < j.mcus_w; mx++) {
            if (restart_left == 0) {
                j.bitbuf = 0;
                j.bitcnt = 0;
                for (i = 0; i < j.ncomp; i++) j.comp[i].dc_pred = 0;
                restart_left = j.restart_interval;
            }
            for (c = 0; c < j.ncomp; c++) {
                int by, bx;
                for (by = 0; by < j.comp[c].v; by++) {
                    for (bx = 0; bx < j.comp[c].h; bx++) {
                        int coef[ZIGZAG_N];
                        unsigned char blk[64];
                        int ox, oy, yy;
                        rc = decode_block(&j, c, coef);
                        if (rc > 0) { kfree(scratch); return rc; }
                        /* rc < 0: out-of-data tail block (eoi hit); coef was
                         * zeroed by decode_block, so the IDCT stores 128 —
                         * mid-gray — which is the graceful end-of-scan fill. */
                        idct_block(j.qt[j.comp[c].tq], coef, blk);
                        ox = mx * 8 * j.comp[c].h + bx * 8;
                        oy = my * 8 * j.comp[c].v + by * 8;
                        for (yy = 0; yy < 8; yy++) {
                            unsigned char *d = plane[c]
                                + (size_t)(oy + yy) * pstride[c] + ox;
                            const unsigned char *s2 = blk + yy * 8;
                            for (i = 0; i < 8; i++) d[i] = s2[i];
                        }
                    }
                }
            }
            restart_left--;
        }
    }

    /* ---- sample + convert, top-down ----
     * Chroma is resampled with libjpeg's "fancy" (separable bilinear,
     * half-pixel-centered) filter so the output matches mainstream
     * decoders within a couple of LSBs. For full-res chroma (h==hmax)
     * this degenerates to a direct sample. */
    for (my = 0; my < j.height; my++) {
        unsigned char *row = out + (size_t)my * j.width * 3;
        for (mx = 0; mx < j.width; mx++) {
            int Y  = plane[0][(size_t)my * pstride[0] + mx];
            int Cb = 128, Cr = 128;
            if (j.ncomp == 3) {
                Cb = chroma_at(&j, plane[1], pstride[1], mx, my, hmax, vmax);
                Cr = chroma_at(&j, plane[2], pstride[2], mx, my, hmax, vmax);
            }
            ycbcr_rgb(Y, Cb, Cr, &row[mx * 3]);
        }
    }

    kfree(scratch);
    (void)mcu_w_blocks;
    return 0;
}
