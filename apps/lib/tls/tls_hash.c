// apps/lib/tls/tls_hash.c — helpers, SHA-256, HMAC-SHA256 and the TLS 1.3
// HKDF-Expand-Label schedule (RFC 8446 §7.1).
//
// No libc: every routine here is self-contained and allocation-free.

#include "tls.h"

// ------------------------------------------------------------------ helpers
void tls_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dp = (uint8_t*)d;
    const uint8_t* sp = (const uint8_t*)s;
    while (n--) *dp++ = *sp++;
}

void tls_memset(void* d, int v, uint32_t n) {
    uint8_t* dp = (uint8_t*)d;
    while (n--) *dp++ = (uint8_t)v;
}

int tls_memcmp(const void* a, const void* b, uint32_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

// Constant-time equality for MAC/tag comparisons: a tag compare that returns
// early leaks how many bytes matched.
int tls_const_eq(const void* a, const void* b, uint32_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    uint8_t diff = 0;
    while (n--) diff |= (uint8_t)(*x++ ^ *y++);
    return diff == 0;
}

uint32_t tls_load24(const uint8_t* p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

uint32_t tls_load32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

void tls_store16(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

void tls_store24(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}

// ------------------------------------------------------------------ SHA-256
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t ror32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_compress(tls_sha256_t* c, const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void tls_sha256_init(tls_sha256_t* c) {
    c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85;
    c->h[2] = 0x3c6ef372; c->h[3] = 0xa54ff53a;
    c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c;
    c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
    c->blen = 0;
    c->total = 0;
}

void tls_sha256_update(tls_sha256_t* c, const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    c->total += len;
    while (len) {
        uint32_t take = 64 - c->blen;
        if (take > len) take = len;
        tls_memcpy(c->blk + c->blen, p, take);
        c->blen += take;
        p += take;
        len -= take;
        if (c->blen == 64) {
            sha256_compress(c, c->blk);
            c->blen = 0;
        }
    }
}

void tls_sha256_final(tls_sha256_t* c, uint8_t out[32]) {
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    tls_sha256_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->blen != 56) tls_sha256_update(c, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    tls_sha256_update(c, lenb, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4 + 0] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->h[i]);
    }
}

void tls_sha256(const void* data, uint32_t len, uint8_t out[32]) {
    tls_sha256_t c;
    tls_sha256_init(&c);
    tls_sha256_update(&c, data, len);
    tls_sha256_final(&c, out);
}

// ------------------------------------------------------------------ HMAC
// Streaming HMAC, so HKDF-Expand can hash T(i-1) || info || i without
// assembling that buffer (info alone can be a few hundred bytes here).
typedef struct {
    tls_sha256_t inner;
    uint8_t opad[64];
} hmac_ctx;

static void hmac_init(hmac_ctx* h, const uint8_t* key, uint32_t klen) {
    uint8_t k[64], ipad[64];
    tls_memset(k, 0, sizeof(k));
    if (klen > 64) {
        tls_sha256(key, klen, k);       // long keys are hashed first
    } else {
        tls_memcpy(k, key, klen);
    }
    for (int i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        h->opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }
    tls_sha256_init(&h->inner);
    tls_sha256_update(&h->inner, ipad, 64);
}

static void hmac_update(hmac_ctx* h, const uint8_t* data, uint32_t len) {
    if (len) tls_sha256_update(&h->inner, data, len);
}

static void hmac_final(hmac_ctx* h, uint8_t out[32]) {
    uint8_t inner[32];
    tls_sha256_final(&h->inner, inner);
    tls_sha256_t o;
    tls_sha256_init(&o);
    tls_sha256_update(&o, h->opad, 64);
    tls_sha256_update(&o, inner, 32);
    tls_sha256_final(&o, out);
}

void tls_hmac_sha256(const uint8_t* key, uint32_t klen,
                     const uint8_t* data, uint32_t dlen, uint8_t out[32]) {
    hmac_ctx h;
    hmac_init(&h, key, klen);
    hmac_update(&h, data, dlen);
    hmac_final(&h, out);
}

// ------------------------------------------------------------------ HKDF (RFC 5869)
void tls_hkdf_extract(const uint8_t* salt, uint32_t slen,
                      const uint8_t* ikm, uint32_t ilen, uint8_t out[32]) {
    static const uint8_t zeros[32] = {0};
    if (!salt || slen == 0) { salt = zeros; slen = 32; }
    tls_hmac_sha256(salt, slen, ikm, ilen, out);
}

void tls_hkdf_expand(const uint8_t* prk, const uint8_t* info, uint32_t ilen,
                     uint8_t* out, uint32_t outlen) {
    uint8_t t[32];
    uint32_t tlen = 0;
    uint8_t counter = 1;
    while (outlen > 0) {
        // T(i) = HMAC(PRK, T(i-1) | info | i)   (RFC 5869 §2.3)
        hmac_ctx h;
        hmac_init(&h, prk, 32);
        hmac_update(&h, t, tlen);
        hmac_update(&h, info, ilen);
        hmac_update(&h, &counter, 1);
        hmac_final(&h, t);
        tlen = 32;
        uint32_t take = outlen < 32 ? outlen : 32;
        tls_memcpy(out, t, take);
        out += take;
        outlen -= take;
        counter++;
    }
}

// HKDF-Expand-Label (RFC 8446 §7.1):
//   struct { uint16 length; opaque label<7..255> = "tls13 " + label;
//            opaque context<0..255>; } HkdfLabel;
void tls_hkdf_expand_label(const uint8_t* secret, const char* label,
                           const uint8_t* ctx, uint32_t ctxlen,
                           uint8_t* out, uint32_t outlen) {
    uint8_t info[2 + 1 + 255 + 1 + 255];
    uint32_t lab_len = 0;
    while (label[lab_len]) lab_len++;
    uint32_t i = 0;
    info[i++] = (uint8_t)(outlen >> 8);
    info[i++] = (uint8_t)outlen;
    info[i++] = (uint8_t)(6 + lab_len);
    const char* pre = "tls13 ";
    for (int k = 0; k < 6; k++) info[i++] = (uint8_t)pre[k];
    for (uint32_t k = 0; k < lab_len; k++) info[i++] = (uint8_t)label[k];
    info[i++] = (uint8_t)ctxlen;
    for (uint32_t k = 0; k < ctxlen; k++) info[i++] = ctx[k];
    tls_hkdf_expand(secret, info, i, out, outlen);
}

// =========================================================================
// SHA-512 / SHA-384 (FIPS 180-4)
// =========================================================================
// Needed for P-384 certificate chains and RSA-PSS with SHA-384/512. 64-bit
// words are fine on i386 (adds/shifts/rotates are inlined); only division
// would need libgcc, and there is none.
static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};

static uint64_t ror64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static void sha512_compress(uint64_t h[8], const uint8_t* p) {
    uint64_t w[80];
    for (int i = 0; i < 16; i++) {
        uint64_t v = 0;
        for (int k = 0; k < 8; k++) v = (v << 8) | p[i * 8 + k];
        w[i] = v;
    }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint64_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 80; i++) {
        uint64_t S1 = ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41);
        uint64_t ch = (e & f) ^ ((~e) & g);
        uint64_t t1 = hh + S1 + ch + K512[i] + w[i];
        uint64_t S0 = ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39);
        uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

// One-shot only: `outlen` is 48 (SHA-384) or 64 (SHA-512).
static void sha512_family(const void* data, uint32_t len, uint8_t* out,
                         uint32_t outlen, const uint64_t iv[8]) {
    uint64_t h[8];
    for (int i = 0; i < 8; i++) h[i] = iv[i];
    const uint8_t* p = (const uint8_t*)data;
    uint32_t full = len / 128;
    for (uint32_t i = 0; i < full; i++) sha512_compress(h, p + i * 128);

    uint8_t tail[256];
    uint32_t rem = len - full * 128;
    tls_memcpy(tail, p + full * 128, rem);
    tail[rem++] = 0x80;
    uint32_t blocks = (rem > 112) ? 2 : 1;
    while (rem < blocks * 128) tail[rem++] = 0;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) tail[blocks * 128 - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (uint32_t b = 0; b < blocks; b++) sha512_compress(h, tail + b * 128);

    uint8_t full_out[64];
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 8; k++)
            full_out[i * 8 + k] = (uint8_t)(h[i] >> (56 - 8 * k));
    tls_memcpy(out, full_out, outlen);
}

void tls_sha512(const void* data, uint32_t len, uint8_t out[64]) {
    static const uint64_t iv[8] = {
        0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL };
    sha512_family(data, len, out, 64, iv);
}

void tls_sha384(const void* data, uint32_t len, uint8_t out[48]) {
    static const uint64_t iv[8] = {
        0xcbbb9d5dc1059ed8ULL,0x629a292a367cd507ULL,0x9159015a3070dd17ULL,0x152fecd8f70e5939ULL,
        0x67332667ffc00b31ULL,0x8eb44a8768581511ULL,0xdb0c2e0d64f98fa7ULL,0x47b5481dbefa4fa4ULL };
    sha512_family(data, len, out, 48, iv);
}
