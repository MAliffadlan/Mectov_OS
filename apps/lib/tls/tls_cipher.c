// apps/lib/tls/tls_cipher.c — the AEADs and the key exchange.
//
//   * ChaCha20 (RFC 8439) + Poly1305, and AEAD_CHACHA20_POLY1305.
//   * AES-128 (FIPS-197) + GHASH / AES-128-GCM (NIST SP 800-38D).
//   * X25519 (RFC 7748), 16-bit-limb field arithmetic: no __int128, and no
//     64-bit division anywhere (the 32-bit link has no libgcc).
//
// All of it is vector-tested by apps/tlsselftest.c against the RFCs.

#include "tls.h"

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void le32_store(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// =========================================================================
// ChaCha20
// =========================================================================
#define CHACHA_QR(a, b, c, d)          \
    a += b; d ^= a; d = ROTL32(d, 16); \
    c += d; b ^= c; b = ROTL32(b, 12); \
    a += b; d ^= a; d = ROTL32(d, 8);  \
    c += d; b ^= c; b = ROTL32(b, 7)

void tls_chacha20_block(const uint8_t key[32], uint32_t counter,
                        const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    // ChaCha20 loads its key/nonce/counter LITTLE-endian (that is the whole
    // reason tls_load32 — big-endian, as TLS records are — is the wrong
    // helper here; le32 is the right one).
    for (int i = 0; i < 8; i++) s[4 + i] = le32(key + i * 4);
    s[12] = counter;
    s[13] = le32(nonce + 0);
    s[14] = le32(nonce + 4);
    s[15] = le32(nonce + 8);

    uint32_t w[16];
    for (int i = 0; i < 16; i++) w[i] = s[i];
    for (int i = 0; i < 10; i++) {          // 20 rounds = 10 double rounds
        CHACHA_QR(w[0], w[4], w[8],  w[12]);
        CHACHA_QR(w[1], w[5], w[9],  w[13]);
        CHACHA_QR(w[2], w[6], w[10], w[14]);
        CHACHA_QR(w[3], w[7], w[11], w[15]);
        CHACHA_QR(w[0], w[5], w[10], w[15]);
        CHACHA_QR(w[1], w[6], w[11], w[12]);
        CHACHA_QR(w[2], w[7], w[8],  w[13]);
        CHACHA_QR(w[3], w[4], w[9],  w[14]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t v = w[i] + s[i];
        out[i * 4 + 0] = (uint8_t)v;
        out[i * 4 + 1] = (uint8_t)(v >> 8);
        out[i * 4 + 2] = (uint8_t)(v >> 16);
        out[i * 4 + 3] = (uint8_t)(v >> 24);
    }
}

void tls_chacha20_xor(const uint8_t key[32], uint32_t counter,
                      const uint8_t nonce[12], const uint8_t* in,
                      uint8_t* out, uint32_t len) {
    uint8_t ks[64];
    while (len) {
        tls_chacha20_block(key, counter, nonce, ks);
        uint32_t take = len < 64 ? len : 64;
        for (uint32_t i = 0; i < take; i++) out[i] = (uint8_t)(in[i] ^ ks[i]);
        in += take; out += take; len -= take; counter++;
    }
}

// =========================================================================
// Poly1305 (RFC 8439 §2.5), 26-bit limbs
// =========================================================================
typedef struct {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
    uint8_t  buf[16];
    uint32_t buf_len;
} poly1305_ctx;

static void poly1305_init(poly1305_ctx* c, const uint8_t key[32]) {
    c->r[0] = le32(key + 0) & 0x3ffffff;
    c->r[1] = (le32(key + 3) >> 2) & 0x3ffff03;
    c->r[2] = (le32(key + 6) >> 4) & 0x3ffc0ff;
    c->r[3] = (le32(key + 9) >> 6) & 0x3f03fff;
    c->r[4] = (le32(key + 12) >> 8) & 0x00fffff;
    c->h[0] = c->h[1] = c->h[2] = c->h[3] = c->h[4] = 0;
    c->pad[0] = le32(key + 16);
    c->pad[1] = le32(key + 20);
    c->pad[2] = le32(key + 24);
    c->pad[3] = le32(key + 28);
    c->buf_len = 0;
}

static void poly1305_blocks(poly1305_ctx* c, const uint8_t* m, uint32_t bytes,
                            uint32_t hibit) {
    uint32_t r0 = c->r[0], r1 = c->r[1], r2 = c->r[2], r3 = c->r[3], r4 = c->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = c->h[0], h1 = c->h[1], h2 = c->h[2], h3 = c->h[3], h4 = c->h[4];

    while (bytes >= 16) {
        h0 += le32(m + 0) & 0x3ffffff;
        h1 += (le32(m + 3) >> 2) & 0x3ffffff;
        h2 += (le32(m + 6) >> 4) & 0x3ffffff;
        h3 += (le32(m + 9) >> 6) & 0x3ffffff;
        h4 += (le32(m + 12) >> 8) | hibit;

        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 +
                      (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 +
                      (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 +
                      (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 +
                      (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 +
                      (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

        uint32_t cc = (uint32_t)(d0 >> 26);
        h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += cc; cc = (uint32_t)(d1 >> 26);
        h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += cc; cc = (uint32_t)(d2 >> 26);
        h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += cc; cc = (uint32_t)(d3 >> 26);
        h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += cc; cc = (uint32_t)(d4 >> 26);
        h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += cc * 5; cc = h0 >> 26;
        h0 &= 0x3ffffff;
        h1 += cc;

        m += 16;
        bytes -= 16;
    }
    c->h[0] = h0; c->h[1] = h1; c->h[2] = h2; c->h[3] = h3; c->h[4] = h4;
}

static void poly1305_update(poly1305_ctx* c, const uint8_t* m, uint32_t len) {
    if (c->buf_len) {
        uint32_t need = 16 - c->buf_len;
        uint32_t take = len < need ? len : need;
        tls_memcpy(c->buf + c->buf_len, m, take);
        c->buf_len += take;
        m += take;
        len -= take;
        if (c->buf_len == 16) {
            poly1305_blocks(c, c->buf, 16, 1u << 24);
            c->buf_len = 0;
        }
    }
    if (len >= 16) {
        uint32_t whole = len & ~(uint32_t)15;
        poly1305_blocks(c, m, whole, 1u << 24);
        m += whole;
        len -= whole;
    }
    if (len) {
        tls_memcpy(c->buf, m, len);
        c->buf_len = len;
    }
}

static void poly1305_final(poly1305_ctx* c, uint8_t tag[16]) {
    uint32_t h0, h1, h2, h3, h4, cc;

    if (c->buf_len) {
        c->buf[c->buf_len++] = 1;
        while (c->buf_len < 16) c->buf[c->buf_len++] = 0;
        poly1305_blocks(c, c->buf, 16, 0);
    }

    h0 = c->h[0]; h1 = c->h[1]; h2 = c->h[2]; h3 = c->h[3]; h4 = c->h[4];
    cc = h1 >> 26; h1 &= 0x3ffffff;
    h2 += cc; cc = h2 >> 26; h2 &= 0x3ffffff;
    h3 += cc; cc = h3 >> 26; h3 &= 0x3ffffff;
    h4 += cc; cc = h4 >> 26; h4 &= 0x3ffffff;
    h0 += cc * 5; cc = h0 >> 26; h0 &= 0x3ffffff;
    h1 += cc;

    // h + -p, then keep the smaller of the two (constant time)
    uint32_t g0 = h0 + 5; cc = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + cc; cc = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + cc; cc = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + cc; cc = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + cc - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;      // 0xffffffff when h >= p
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    // h mod 2^128 (the high bits are meant to fall off)
    uint32_t w0 = (h0) | (h1 << 26);
    uint32_t w1 = (h1 >> 6) | (h2 << 20);
    uint32_t w2 = (h2 >> 12) | (h3 << 14);
    uint32_t w3 = (h3 >> 18) | (h4 << 8);

    uint64_t f = (uint64_t)w0 + c->pad[0];      w0 = (uint32_t)f;
    f = (uint64_t)w1 + c->pad[1] + (f >> 32);   w1 = (uint32_t)f;
    f = (uint64_t)w2 + c->pad[2] + (f >> 32);   w2 = (uint32_t)f;
    f = (uint64_t)w3 + c->pad[3] + (f >> 32);   w3 = (uint32_t)f;

    le32_store(tag + 0, w0);
    le32_store(tag + 4, w1);
    le32_store(tag + 8, w2);
    le32_store(tag + 12, w3);
}

void tls_poly1305(const uint8_t key[32], const uint8_t* msg, uint32_t len,
                  uint8_t tag[16]) {
    poly1305_ctx c;
    poly1305_init(&c, key);
    poly1305_update(&c, msg, len);
    poly1305_final(&c, tag);
}

// -- AEAD_CHACHA20_POLY1305 (RFC 8439 §2.8) --------------------------------
static void aead_pad16(poly1305_ctx* p, uint32_t len) {
    static const uint8_t zeros[16] = {0};
    uint32_t rem = len & 15;
    if (rem) poly1305_update(p, zeros, 16 - rem);
}

static void aead_mac(poly1305_ctx* p, const uint8_t* aad, uint32_t aadlen,
                     const uint8_t* ct, uint32_t ctlen) {
    poly1305_update(p, aad, aadlen);
    aead_pad16(p, aadlen);
    poly1305_update(p, ct, ctlen);
    aead_pad16(p, ctlen);
    // RFC 8439 §2.8: the trailer holds the AAD and ciphertext lengths in
    // BYTES, little-endian. (AES-GCM's length block is in bits — mixing the
    // two up is the classic porting bug, and it produced a wrong tag on
    // every record while the Poly1305 unit vector still passed.)
    uint8_t lens[16];
    for (int i = 0; i < 8; i++) {
        lens[i] = (uint8_t)((uint64_t)aadlen >> (8 * i));
        lens[8 + i] = (uint8_t)((uint64_t)ctlen >> (8 * i));
    }
    poly1305_update(p, lens, 16);
}

int tls_chacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t* aad, uint32_t aadlen,
                               const uint8_t* pt, uint32_t ptlen,
                               uint8_t* ct, uint8_t tag[16]) {
    uint8_t otk[64];
    tls_chacha20_block(key, 0, nonce, otk);
    tls_chacha20_xor(key, 1, nonce, pt, ct, ptlen);
    poly1305_ctx p;
    poly1305_init(&p, otk);
    aead_mac(&p, aad, aadlen, ct, ptlen);
    poly1305_final(&p, tag);
    return 0;
}

int tls_chacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t* aad, uint32_t aadlen,
                               const uint8_t* ct, uint32_t ctlen,
                               const uint8_t tag[16], uint8_t* pt) {
    uint8_t otk[64];
    uint8_t expect[16];
    tls_chacha20_block(key, 0, nonce, otk);
    poly1305_ctx p;
    poly1305_init(&p, otk);
    aead_mac(&p, aad, aadlen, ct, ctlen);
    poly1305_final(&p, expect);
    if (!tls_const_eq(expect, tag, 16)) return -1;
    tls_chacha20_xor(key, 1, nonce, ct, pt, ctlen);
    return 0;
}

// =========================================================================
// AES-128 (FIPS-197)
// =========================================================================
static const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
}

void tls_aes128_key_expand(const uint8_t key[16], uint8_t rk[176]) {
    static const uint8_t rcon[11] = {0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};
    tls_memcpy(rk, key, 16);
    for (int i = 4; i < 44; i++) {
        uint8_t t0 = rk[(i - 1) * 4 + 0];
        uint8_t t1 = rk[(i - 1) * 4 + 1];
        uint8_t t2 = rk[(i - 1) * 4 + 2];
        uint8_t t3 = rk[(i - 1) * 4 + 3];
        if ((i & 3) == 0) {
            uint8_t tmp = t0;
            t0 = (uint8_t)(sbox[t1] ^ rcon[i / 4]);
            t1 = sbox[t2];
            t2 = sbox[t3];
            t3 = sbox[tmp];
        }
        rk[i * 4 + 0] = (uint8_t)(rk[(i - 4) * 4 + 0] ^ t0);
        rk[i * 4 + 1] = (uint8_t)(rk[(i - 4) * 4 + 1] ^ t1);
        rk[i * 4 + 2] = (uint8_t)(rk[(i - 4) * 4 + 2] ^ t2);
        rk[i * 4 + 3] = (uint8_t)(rk[(i - 4) * 4 + 3] ^ t3);
    }
}

void tls_aes128_encrypt_block(const uint8_t rk[176], const uint8_t in[16],
                              uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[i]);
    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];
        // state is column-major: s[4*col + row]
        uint8_t t;
        t = s[1];  s[1]  = s[5];  s[5]  = s[9];  s[9]  = s[13]; s[13] = t;
        t = s[2];  s[2]  = s[10]; s[10] = t;
        t = s[6];  s[6]  = s[14]; s[14] = t;
        t = s[15]; s[15] = s[11]; s[11] = s[7];  s[7]  = s[3];  s[3]  = t;
        if (round != 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t* p = s + 4 * c;
                uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                uint8_t x = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                p[0] = (uint8_t)(a0 ^ x ^ xtime((uint8_t)(a0 ^ a1)));
                p[1] = (uint8_t)(a1 ^ x ^ xtime((uint8_t)(a1 ^ a2)));
                p[2] = (uint8_t)(a2 ^ x ^ xtime((uint8_t)(a2 ^ a3)));
                p[3] = (uint8_t)(a3 ^ x ^ xtime((uint8_t)(a3 ^ a0)));
            }
        }
        for (int i = 0; i < 16; i++) s[i] ^= rk[round * 16 + i];
    }
    tls_memcpy(out, s, 16);
}

// =========================================================================
// GHASH / AES-128-GCM
// =========================================================================
typedef struct {
    uint32_t y[4];       // current GHASH state, big-endian words
    uint32_t h[4];       // hash subkey H
    uint8_t  buf[16];
    uint32_t bl;
} ghash_ctx;

// z = x * H in GF(2^128), MSB-first bit order (GCM convention).
static void gf_mul(uint32_t x[4], const uint32_t h[4]) {
    uint32_t z[4] = {0, 0, 0, 0};
    uint32_t v[4] = {h[0], h[1], h[2], h[3]};
    for (int i = 0; i < 128; i++) {
        uint32_t bit = (x[i >> 5] >> (31 - (i & 31))) & 1u;
        if (bit) { z[0] ^= v[0]; z[1] ^= v[1]; z[2] ^= v[2]; z[3] ^= v[3]; }
        uint32_t lsb = v[3] & 1u;
        v[3] = (v[3] >> 1) | (v[2] << 31);
        v[2] = (v[2] >> 1) | (v[1] << 31);
        v[1] = (v[1] >> 1) | (v[0] << 31);
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xE1000000u;
    }
    x[0] = z[0]; x[1] = z[1]; x[2] = z[2]; x[3] = z[3];
}

static void ghash_init(ghash_ctx* g, const uint8_t H[16]) {
    g->y[0] = g->y[1] = g->y[2] = g->y[3] = 0;
    g->h[0] = tls_load32(H + 0);
    g->h[1] = tls_load32(H + 4);
    g->h[2] = tls_load32(H + 8);
    g->h[3] = tls_load32(H + 12);
    g->bl = 0;
}

static void ghash_absorb(ghash_ctx* g, const uint8_t block[16]) {
    uint32_t t[4];
    t[0] = g->y[0] ^ tls_load32(block + 0);
    t[1] = g->y[1] ^ tls_load32(block + 4);
    t[2] = g->y[2] ^ tls_load32(block + 8);
    t[3] = g->y[3] ^ tls_load32(block + 12);
    gf_mul(t, g->h);
    g->y[0] = t[0]; g->y[1] = t[1]; g->y[2] = t[2]; g->y[3] = t[3];
}

static void ghash_update(ghash_ctx* g, const uint8_t* p, uint32_t len) {
    if (g->bl) {
        uint32_t need = 16 - g->bl;
        uint32_t take = len < need ? len : need;
        tls_memcpy(g->buf + g->bl, p, take);
        g->bl += take;
        p += take;
        len -= take;
        if (g->bl == 16) { ghash_absorb(g, g->buf); g->bl = 0; }
    }
    while (len >= 16) { ghash_absorb(g, p); p += 16; len -= 16; }
    if (len) { tls_memcpy(g->buf, p, len); g->bl = len; }
}

static void ghash_pad(ghash_ctx* g) {
    if (g->bl) {
        while (g->bl < 16) g->buf[g->bl++] = 0;
        ghash_absorb(g, g->buf);
        g->bl = 0;
    }
}

static void ghash_final(const ghash_ctx* g, uint8_t out[16]) {
    for (int i = 0; i < 4; i++) {
        out[i * 4 + 0] = (uint8_t)(g->y[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(g->y[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(g->y[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(g->y[i]);
    }
}

// GCTR: counter starts at inc32(icb).
static void gcm_ctr(const uint8_t rk[176], const uint8_t icb[16],
                    const uint8_t* in, uint8_t* out, uint32_t len) {
    uint8_t ctr[16], ks[16];
    tls_memcpy(ctr, icb, 16);
    while (len) {
        for (int i = 15; i >= 12; i--) {
            if (++ctr[i] != 0) break;
        }
        tls_aes128_encrypt_block(rk, ctr, ks);
        uint32_t take = len < 16 ? len : 16;
        for (uint32_t i = 0; i < take; i++) out[i] = (uint8_t)(in[i] ^ ks[i]);
        in += take; out += take; len -= take;
    }
}

static void gcm_j0(const uint8_t nonce[12], uint8_t j0[16]) {
    tls_memcpy(j0, nonce, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
}

static void gcm_tag(const uint8_t rk[176], const uint8_t j0[16],
                    const uint8_t* aad, uint32_t aadlen,
                    const uint8_t* ct, uint32_t ctlen, uint8_t tag[16]) {
    uint8_t H[16], zero[16], S[16];
    for (int i = 0; i < 16; i++) zero[i] = 0;
    tls_aes128_encrypt_block(rk, zero, H);

    ghash_ctx g;
    ghash_init(&g, H);
    ghash_update(&g, aad, aadlen);
    ghash_pad(&g);
    ghash_update(&g, ct, ctlen);
    ghash_pad(&g);
    uint8_t lens[16];
    uint64_t ab = (uint64_t)aadlen * 8;
    uint64_t cb = (uint64_t)ctlen * 8;
    for (int i = 0; i < 8; i++) {
        lens[i] = (uint8_t)(ab >> (56 - 8 * i));
        lens[8 + i] = (uint8_t)(cb >> (56 - 8 * i));
    }
    ghash_update(&g, lens, 16);
    ghash_final(&g, S);

    uint8_t mask[16];
    tls_aes128_encrypt_block(rk, j0, mask);
    for (int i = 0; i < 16; i++) tag[i] = (uint8_t)(S[i] ^ mask[i]);
}

int tls_aes128_gcm_seal(const uint8_t key[16], const uint8_t nonce[12],
                        const uint8_t* aad, uint32_t aadlen,
                        const uint8_t* pt, uint32_t ptlen,
                        uint8_t* ct, uint8_t tag[16]) {
    uint8_t rk[176], j0[16];
    tls_aes128_key_expand(key, rk);
    gcm_j0(nonce, j0);
    gcm_ctr(rk, j0, pt, ct, ptlen);
    gcm_tag(rk, j0, aad, aadlen, ct, ptlen, tag);
    return 0;
}

int tls_aes128_gcm_open(const uint8_t key[16], const uint8_t nonce[12],
                        const uint8_t* aad, uint32_t aadlen,
                        const uint8_t* ct, uint32_t ctlen,
                        const uint8_t tag[16], uint8_t* pt) {
    uint8_t rk[176], j0[16], expect[16];
    tls_aes128_key_expand(key, rk);
    gcm_j0(nonce, j0);
    gcm_tag(rk, j0, aad, aadlen, ct, ctlen, expect);
    if (!tls_const_eq(expect, tag, 16)) return -1;
    gcm_ctr(rk, j0, ct, pt, ctlen);
    return 0;
}

// =========================================================================
// X25519 (RFC 7748) — 16-bit limbs, int64 intermediates
// =========================================================================
typedef int64_t fe64[16];

static const fe64 X25519_121665 = {0xDB41, 1};

static void fe_carry(fe64 o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void fe_sel(fe64 p, fe64 q, int b) {
    int64_t c = ~(int64_t)(b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void fe_pack(uint8_t* o, const fe64 n) {
    fe64 m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    fe_carry(t);
    fe_carry(t);
    fe_carry(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        fe_sel(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i]     = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)((t[i] >> 8) & 0xff);
    }
}

static void fe_unpack(fe64 o, const uint8_t* n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void fe_add(fe64 o, const fe64 a, const fe64 b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void fe_sub(fe64 o, const fe64 a, const fe64 b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void fe_mul(fe64 o, const fe64 a, const fe64 b) {
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    }
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    fe_carry(o);
    fe_carry(o);
}

static void fe_sq(fe64 o, const fe64 a) { fe_mul(o, a, a); }

static void fe_inv(fe64 o, const fe64 i) {
    fe64 c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) {
        fe_sq(c, c);
        if (a != 2 && a != 4) fe_mul(c, c, i);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

void tls_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t z[32];
    int64_t x[80];
    fe64 a, b, c, d, e, f;

    for (int i = 0; i < 31; i++) z[i] = scalar[i];
    z[31] = (uint8_t)((scalar[31] & 127) | 64);
    z[0] &= 248;

    fe_unpack(x, point);
    for (int i = 0; i < 16; i++) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;

    for (int i = 254; i >= 0; i--) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        fe_sel(a, b, r);
        fe_sel(c, d, r);
        fe_add(e, a, c);
        fe_sub(a, a, c);
        fe_add(c, b, d);
        fe_sub(b, b, d);
        fe_sq(d, e);
        fe_sq(f, a);
        fe_mul(a, c, a);
        fe_mul(c, b, e);
        fe_add(e, a, c);
        fe_sub(a, a, c);
        fe_sq(b, a);
        fe_sub(c, d, f);
        fe_mul(a, c, X25519_121665);
        fe_add(a, a, d);
        fe_mul(c, c, a);
        fe_mul(a, d, f);
        fe_mul(d, b, x);
        fe_sq(b, e);
        fe_sel(a, b, r);
        fe_sel(c, d, r);
    }
    fe_inv(c, c);
    fe_mul(a, a, c);
    fe_pack(out, a);

    // Wipe the ladder scratch: it holds the shared secret.
    for (int i = 0; i < 80; i++) x[i] = 0;
    for (int i = 0; i < 16; i++) { a[i] = b[i] = c[i] = d[i] = e[i] = f[i] = 0; }
}

void tls_x25519_base(uint8_t out[32], const uint8_t scalar[32]) {
    uint8_t base[32];
    for (int i = 0; i < 32; i++) base[i] = 0;
    base[0] = 9;
    tls_x25519(out, scalar, base);
}
