// apps/lib/tls/tls_pubkey.c — big integers, RSA and ECDSA signature
// verification for the TLS client's certificate chain check.
//
// Only VERIFICATION is implemented: the kernel's TLS client never signs
// anything, so there is no private-key path here at all.
//
// Representation: fixed arrays of 32-bit limbs, least significant first, up to
// TLS_BN_LIMBS (4096 bits). Modular multiplication is Montgomery (CIOS), which
// is chosen here for a specific reason: this build has no libgcc, so 64-bit
// divide is unavailable — Montgomery multiplication needs none. Exponentiation
// is square-and-multiply over the exponent's bits, which matters for RSA
// (e = 65537, so 17 multiplications) and for the ECDSA inversions (Fermat).

#include "tls.h"

// ------------------------------------------------------------------ basics
void tls_bn_zero(tls_bn_t* a) {
    for (int i = 0; i < TLS_BN_LIMBS; i++) a->w[i] = 0;
    a->n = 0;
}

void tls_bn_set_u32(tls_bn_t* a, uint32_t v) {
    tls_bn_zero(a);
    if (v) { a->w[0] = v; a->n = 1; }
}

void tls_bn_from_bytes(tls_bn_t* a, const uint8_t* be, uint32_t len) {
    tls_bn_zero(a);
    uint32_t n = (len + 3) / 4;
    if (n > TLS_BN_LIMBS) n = TLS_BN_LIMBS;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t rev = len - 1 - i;
        a->w[rev / 4] |= (uint32_t)be[i] << (8 * (rev % 4));
    }
    a->n = n;
    while (a->n > 0 && a->w[a->n - 1] == 0) a->n--;
}

void tls_bn_to_bytes(const tls_bn_t* a, uint8_t* be, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        uint32_t rev = len - 1 - i;
        uint8_t v = 0;
        if (rev / 4 < (uint32_t)a->n) v = (uint8_t)(a->w[rev / 4] >> (8 * (rev % 4)));
        be[i] = v;
    }
}

int tls_bn_is_zero(const tls_bn_t* a) { return a->n == 0; }

int tls_bn_bitlen(const tls_bn_t* a) {
    if (a->n == 0) return 0;
    uint32_t top = a->w[a->n - 1];
    int bits = 0;
    while (top) { bits++; top >>= 1; }
    return (int)((a->n - 1) * 32 + (uint32_t)bits);
}

int tls_bn_cmp(const tls_bn_t* a, const tls_bn_t* b) {
    uint32_t n = a->n > b->n ? a->n : b->n;
    for (int i = (int)n - 1; i >= 0; i--) {
        uint32_t x = (i < a->n) ? a->w[i] : 0;
        uint32_t y = (i < b->n) ? b->w[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

// Bit k counted from the MOST significant bit of a big-endian buffer, so k = 0
// is the top bit of be[0]. Every square-and-multiply loop, scalar
// multiplication and Fermat inversion in this file reads bits through this one
// function: when each loop spelled out its own shifting, they disagreed about
// the order inside a byte, which quietly reduced the RSA exponent 0x010001 to 1.
static int be_bit(const uint8_t* be, uint32_t len, uint32_t k) {
    if (k >= len * 8) return 0;
    return (be[k >> 3] >> (7 - (k & 7))) & 1;
}

static int bn_ge_n(const uint32_t* a, const uint32_t* b, uint32_t n) {
    for (int i = (int)n - 1; i >= 0; i--) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return 1;
}

// a -= b (caller guarantees a >= b)
static void bn_sub(uint32_t* a, const uint32_t* b, uint32_t n) {
    uint64_t borrow = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t v = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)v;
        borrow = (v >> 32) ? 1 : 0;
    }
}

uint32_t tls_bn_mont_n0inv(const tls_bn_t* m) {
    // -m^-1 mod 2^32 by Newton: each step doubles the correct bit width, and
    // m[0] is odd (every modulus here is a prime or a product of odd primes).
    uint32_t inv = 1;
    for (int i = 0; i < 5; i++) inv *= 2u - m->w[0] * inv;
    return (uint32_t)(0u - inv);
}

// ------------------------------------------------------------------ Montgomery
typedef struct {
    tls_bn_t m;
    uint32_t n;
    uint32_t n0inv;
    tls_bn_t r2;      // R^2 mod m
} mont_ctx_t;

static void mont_init(mont_ctx_t* c, const tls_bn_t* m) {
    c->m = *m;
    c->n = m->n;
    c->n0inv = tls_bn_mont_n0inv(m);
    // R^2 mod m by doubling 1 exactly 2*32*n times (mod m each step). Slow but
    // one-time and division-free.
    tls_bn_t r;
    tls_bn_set_u32(&r, 1);
    uint32_t t[TLS_BN_LIMBS + 2];
    for (uint32_t step = 0; step < 64 * c->n; step++) {
        uint64_t carry = 0;
        for (uint32_t i = 0; i < c->n; i++) {
            uint64_t v = ((uint64_t)r.w[i] << 1) | carry;
            t[i] = (uint32_t)v;
            carry = v >> 32;
        }
        t[c->n] = (uint32_t)carry;
        if (carry || bn_ge_n(t, m->w, c->n)) bn_sub(t, m->w, c->n);
        for (uint32_t i = 0; i < c->n; i++) r.w[i] = t[i];
    }
    r.n = c->n;
    while (r.n > 0 && r.w[r.n - 1] == 0) r.n--;
    c->r2 = r;
}

// out = a * b * R^-1 mod m   (Koç CIOS, Algorithm 2)
static void mont_mul_ctx(const mont_ctx_t* c, tls_bn_t* out,
                         const tls_bn_t* a, const tls_bn_t* b) {
    uint32_t n = c->n;
    uint32_t t[TLS_BN_LIMBS + 2];
    for (uint32_t i = 0; i < n + 2; i++) t[i] = 0;
    const uint32_t* mb = c->m.w;

    for (uint32_t i = 0; i < n; i++) {
        uint64_t carry = 0;
        for (uint32_t j = 0; j < n; j++) {
            uint64_t v = (uint64_t)a->w[j] * b->w[i] + t[j] + carry;
            t[j] = (uint32_t)v;
            carry = v >> 32;
        }
        uint64_t v = (uint64_t)t[n] + carry;
        t[n] = (uint32_t)v;
        t[n + 1] = (uint32_t)(v >> 32);

        uint32_t mm = (uint32_t)((uint64_t)t[0] * c->n0inv);
        carry = 0;
        v = (uint64_t)mm * mb[0] + t[0];
        carry = v >> 32;
        for (uint32_t j = 1; j < n; j++) {
            v = (uint64_t)mm * mb[j] + t[j] + carry;
            t[j - 1] = (uint32_t)v;
            carry = v >> 32;
        }
        v = (uint64_t)t[n] + carry;
        t[n - 1] = (uint32_t)v;
        t[n] = t[n + 1] + (uint32_t)(v >> 32);
    }

    if (t[n] != 0 || bn_ge_n(t, mb, n)) {
        bn_sub(t, mb, n);
    }
    for (uint32_t i = 0; i < n; i++) out->w[i] = t[i];
    for (uint32_t i = n; i < TLS_BN_LIMBS; i++) out->w[i] = 0;
    out->n = n;
    while (out->n > 0 && out->w[out->n - 1] == 0) out->n--;
}



// out = base^exp mod m, with `exp` big-endian. Left-to-right square-and-
// multiply, most significant exponent bit first, across all explen*8 bits --
// leading zero bits are just wasted squarings of acc = 1, so there is no scan
// for the first set bit to get wrong.
void tls_bn_modexp(tls_bn_t* out, const tls_bn_t* base, const uint8_t* exp,
                   uint32_t explen, const tls_bn_t* m) {
    if (m->n == 0 || tls_bn_is_zero(m)) { tls_bn_zero(out); return; }
    mont_ctx_t c;
    mont_init(&c, m);

    tls_bn_t acc, bm, one;
    mont_mul_ctx(&c, &bm, base, &c.r2);          // base in Montgomery form
    tls_bn_set_u32(&one, 1);
    mont_mul_ctx(&c, &acc, &one, &c.r2);        // acc = 1 (Montgomery form)

    for (uint32_t k = 0; k < explen * 8; k++) {
        mont_mul_ctx(&c, &acc, &acc, &acc);
        if (be_bit(exp, explen, k)) mont_mul_ctx(&c, &acc, &acc, &bm);
    }
    tls_bn_set_u32(&one, 1);
    mont_mul_ctx(&c, out, &acc, &one);          // leave Montgomery form
}

// ------------------------------------------------------------------ MGF1
// RFC 8017 appendix B.2.1: MGF1(seed, len) = Hash(seed || C) for consecutive
// C, and C is a FOUR-octet big-endian counter -- not the single byte it looks
// like at a glance, since the first blocks happen to have a leading zero anyway.
// Feeding one byte instead of four hashes seed||0x00 rather than
// seed||0x00 0x00 0x00 0x00, so every mask differs and RSA-PSS verification
// fails on signatures that are perfectly good. Nothing caught this because the
// self-test drives the hashes and the RSA signature primitives with vectors but
// has never verified a PSS signature.
static void mgf1(const uint8_t* seed, uint32_t seedlen, uint8_t* out, uint32_t len) {
    uint32_t done = 0;
    uint32_t counter = 0;
    while (done < len) {
        uint8_t block[32];
        uint8_t c[4];
        c[0] = (uint8_t)(counter >> 24);
        c[1] = (uint8_t)(counter >> 16);
        c[2] = (uint8_t)(counter >> 8);
        c[3] = (uint8_t)counter;
        tls_sha256_t sh;
        tls_sha256_init(&sh);
        tls_sha256_update(&sh, seed, seedlen);
        tls_sha256_update(&sh, c, 4);
        tls_sha256_final(&sh, block);
        uint32_t take = len - done;
        if (take > 32) take = 32;
        tls_memcpy(out + done, block, take);
        done += take;
        counter++;
    }
}

// ------------------------------------------------------------------ RSA
#define RSA_MAX_BYTES (TLS_BN_LIMBS * 4)

static int rsa_public_op(const uint8_t* n, uint32_t nlen, const uint8_t* e,
                         uint32_t elen, const uint8_t* sig, uint8_t* em) {
    tls_bn_t nb, eb, sb, res;
    tls_bn_from_bytes(&nb, n, nlen);
    tls_bn_from_bytes(&eb, e, elen);
    tls_bn_from_bytes(&sb, sig, nlen);
    if (tls_bn_is_zero(&nb) || (nb.w[0] & 1) == 0) return -1;
    if (tls_bn_cmp(&sb, &nb) >= 0) return -1;      // signature >= modulus
    tls_bn_modexp(&res, &sb, e, elen, &nb);
    if (tls_bn_bitlen(&res) > (int)(nlen * 8)) return -1;
    tls_bn_to_bytes(&res, em, nlen);
    return 0;
}

// DER DigestInfo prefixes for the three hashes X.509 uses with RSA.
static int digest_info(uint32_t digest_len, const uint8_t** out, uint32_t* out_len) {
    static const uint8_t sha256[] = {
        0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,
        0x01,0x05,0x00,0x04,0x20 };
    static const uint8_t sha384[] = {
        0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,
        0x02,0x05,0x00,0x04,0x30 };
    static const uint8_t sha512[] = {
        0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,
        0x03,0x05,0x00,0x04,0x40 };
    if (digest_len == 32) { *out = sha256; *out_len = sizeof(sha256); }
    else if (digest_len == 48) { *out = sha384; *out_len = sizeof(sha384); }
    else if (digest_len == 64) { *out = sha512; *out_len = sizeof(sha512); }
    else return -1;
    return 0;
}

int tls_rsa_pkcs1_verify(const uint8_t* n, uint32_t nlen, const uint8_t* e,
                         uint32_t elen, const uint8_t* sig,
                         const uint8_t* digest, uint32_t digest_len) {
    if (nlen == 0 || nlen > RSA_MAX_BYTES) return -1;
    uint8_t em[RSA_MAX_BYTES];
    if (rsa_public_op(n, nlen, e, elen, sig, em) != 0) return -1;

    const uint8_t* di;
    uint32_t dilen;
    if (digest_info(digest_len, &di, &dilen) != 0) return -1;

    // EM = 0x00 || 0x01 || PS || 0x00 || DigestInfo
    if (em[0] != 0x00 || em[1] != 0x01) return -1;
    uint32_t need = 3 + dilen + digest_len;
    if (nlen < need) return -1;
    uint32_t i = 2;
    while (i < nlen && em[i] == 0xff) i++;
    if (i == 2 || i >= nlen || em[i] != 0x00) return -1;   // PS must be >= 8
    if (i < 10) return -1;
    i++;
    if (i + dilen + digest_len != nlen) return -1;
    if (tls_memcmp(em + i, di, dilen) != 0) return -1;
    if (tls_memcmp(em + i + dilen, digest, digest_len) != 0) return -1;
    return 0;
}

int tls_rsa_pss_verify(const uint8_t* n, uint32_t nlen, const uint8_t* e,
                       uint32_t elen, const uint8_t* sig,
                       const uint8_t* digest, uint32_t digest_len) {
    if (nlen == 0 || nlen > RSA_MAX_BYTES) return -1;
    if (digest_len != 32 && digest_len != 48 && digest_len != 64) return -1;
    uint8_t em[RSA_MAX_BYTES];
    if (rsa_public_op(n, nlen, e, elen, sig, em) != 0) return -1;

    int em_bits;
    tls_bn_t nb;
    tls_bn_from_bytes(&nb, n, nlen);
    em_bits = tls_bn_bitlen(&nb) - 1;
    uint32_t em_len = (uint32_t)((em_bits + 7) / 8);
    if (em_len != nlen) {
        // The top byte of the modulus must be >= 0x80 for em_len == nlen in
        // practice; if it is not, the encoding rules differ — reject rather
        // than guess.
        if (em_len + 1 != nlen) return -1;
    }
    uint32_t hlen = digest_len;
    if (em_len < hlen + 2) return -1;
    if (em[em_len - 1] != 0xbc) return -1;

    uint32_t db_len = em_len - hlen - 1;
    const uint8_t* masked_db = em;
    const uint8_t* H = em + db_len;

    uint8_t db[RSA_MAX_BYTES];
    mgf1(H, hlen, db, db_len);
    for (uint32_t i = 0; i < db_len; i++) db[i] = (uint8_t)(masked_db[i] ^ db[i]);
    // clear the leftmost (8*em_len - em_bits) bits of db[0]
    uint32_t leftmost = 8u * em_len - (uint32_t)em_bits;
    if (leftmost) db[0] &= (uint8_t)(0xff >> leftmost);

    uint32_t i = 0;
    while (i < db_len && db[i] == 0x00) i++;
    if (i == db_len || db[i] != 0x01) return -1;
    i++;
    uint32_t salt_len = db_len - i;
    const uint8_t* salt = db + i;

    // H' = Hash(0x00 * 8 || mHash || salt)
    uint8_t prime[8 + 128 + 64];
    tls_memset(prime, 0, 8);
    tls_memcpy(prime + 8, digest, digest_len);
    tls_memcpy(prime + 8 + digest_len, salt, salt_len);
    uint8_t hp[64];
    tls_sha256(prime, 8 + digest_len + salt_len, hp);
    return tls_const_eq(hp, H, hlen) ? 0 : -1;
}

// ------------------------------------------------------------------ curves
typedef struct {
    int      id;              // 1 = P-256, 2 = P-384
    uint32_t bytes;           // field byte length
    const char* name;
    const uint8_t* p;
    const uint8_t* b;
    const uint8_t* gx;
    const uint8_t* gy;
    const uint8_t* n;
} ec_curve_t;

static const uint8_t P256_P[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff, };
static const uint8_t P256_B[32] = {
    0x5a,0xc6,0x35,0xd8,0xaa,0x3a,0x93,0xe7,0xb3,0xeb,0xbd,0x55,
    0x76,0x98,0x86,0xbc,0x65,0x1d,0x06,0xb0,0xcc,0x53,0xb0,0xf6,
    0x3b,0xce,0x3c,0x3e,0x27,0xd2,0x60,0x4b, };
static const uint8_t P256_GX[32] = {
    0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,
    0x63,0xa4,0x40,0xf2,0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,
    0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96, };
static const uint8_t P256_GY[32] = {
    0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,
    0x7c,0x0f,0x9e,0x16,0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,
    0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5, };
static const uint8_t P256_N[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,
    0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51, };

// secp384r1 p = 2^384 - 2^128 - 2^96 + 2^32 - 1, verified against
// `openssl ecparam -name secp384r1 -param_enc explicit -text`. The 0xfe sits
// at byte 35 with four more 0xff after it and an eight-byte zero run before the
// trailing 0xffffffff; a single misplaced byte still fills all 48 slots, so a
// wrong modulus compiles silently and only shows up as a failed P-384 verify.
static const uint8_t P384_P[48] = {
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,0xff,0xff,0xff,0xff,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff };
static const uint8_t P384_B[48] = {
    0xb3,0x31,0x2f,0xa7,0xe2,0x3e,0xe7,0xe4,0x98,0x8e,0x05,0x6b,0xe3,0xf8,0x2d,0x19,
    0x18,0x1d,0x9c,0x6e,0xfe,0x81,0x41,0x12,0x03,0x14,0x08,0x8f,0x50,0x13,0x87,0x5a,
    0xc6,0x56,0x39,0x8d,0x8a,0x2e,0xd1,0x9d,0x2a,0x85,0xc8,0xed,0xd3,0xec,0x2a,0xef };
static const uint8_t P384_GX[48] = {
    0xaa,0x87,0xca,0x22,0xbe,0x8b,0x05,0x37,0x8e,0xb1,0xc7,0x1e,0xf3,0x20,0xad,0x74,
    0x6e,0x1d,0x3b,0x62,0x8b,0xa7,0x9b,0x98,0x59,0xf7,0x41,0xe0,0x82,0x54,0x2a,0x38,
    0x55,0x02,0xf2,0x5d,0xbf,0x55,0x29,0x6c,0x3a,0x54,0x5e,0x38,0x72,0x76,0x0a,0xb7 };
static const uint8_t P384_GY[48] = {
    0x36,0x17,0xde,0x4a,0x96,0x26,0x2c,0x6f,0x5d,0x9e,0x98,0xbf,0x92,0x92,0xdc,0x29,
    0xf8,0xf4,0x1d,0xbd,0x28,0x9a,0x14,0x7c,0xe9,0xda,0x31,0x13,0xb5,0xf0,0xb8,0xc0,
    0x0a,0x60,0xb1,0xce,0x1d,0x7e,0x81,0x9d,0x7a,0x43,0x1d,0x7c,0x90,0xea,0x0e,0x5f };
static const uint8_t P384_N[48] = {
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xc7,0x63,0x4d,0x81,0xf4,0x37,0x2d,0xdf,0x58,0x1a,0x0d,0xb2,
    0x48,0xb0,0xa7,0x7a,0xec,0xec,0x19,0x6a,0xcc,0xc5,0x29,0x73, };

static const ec_curve_t CURVES[2] = {
    { 1, 32, "prime256v1", P256_P, P256_B, P256_GX, P256_GY, P256_N },
    { 2, 48, "secp384r1",  P384_P, P384_B, P384_GX, P384_GY, P384_N },
};

typedef struct { tls_bn_t X, Y, Z; } jac_t;

typedef struct {
    const ec_curve_t* c;
    mont_ctx_t fp;                       // field modulus p
    mont_ctx_t fn;                       // group order n
    tls_bn_t one, two, three, eight;       // Montgomery-form constants
} ec_ctx_t;

static void jac_double(const ec_ctx_t* e, jac_t* p1);

static void ec_ctx_init(ec_ctx_t* e, const ec_curve_t* c) {
    e->c = c;
    tls_bn_t p, n;
    tls_bn_from_bytes(&p, c->p, c->bytes);
    tls_bn_from_bytes(&n, c->n, c->bytes);
    mont_init(&e->fp, &p);
    mont_init(&e->fn, &n);
    tls_bn_t k;
    tls_bn_set_u32(&k, 1);
    mont_mul_ctx(&e->fp, &e->one, &k, &e->fp.r2);
    tls_bn_set_u32(&k, 2);
    mont_mul_ctx(&e->fp, &e->two, &k, &e->fp.r2);
    tls_bn_set_u32(&k, 3);
    mont_mul_ctx(&e->fp, &e->three, &k, &e->fp.r2);
    tls_bn_set_u32(&k, 8);
    mont_mul_ctx(&e->fp, &e->eight, &k, &e->fp.r2);
    // Both curves have a = -3, so jac_double below uses the a = -3 formulas and
    // no curve coefficient needs to be carried around.
}

static void fmul(const ec_ctx_t* e, tls_bn_t* r, const tls_bn_t* a, const tls_bn_t* b) {
    mont_mul_ctx(&e->fp, r, a, b);
}

// Drop the limbs above n and strip leading zeros, so that tls_bn_is_zero() --
// which only looks at the used-limb count -- reports a field result of zero as
// zero. Without this, fsub_mont()'s "is U2 == U1?" test in jac_add() never
// fired and P + P silently fell through the general formula with H = 0.
static void bn_norm(tls_bn_t* a, uint32_t n) {
    for (uint32_t i = n; i < TLS_BN_LIMBS; i++) a->w[i] = 0;
    a->n = (int)n;
    while (a->n > 0 && a->w[a->n - 1] == 0) a->n--;
}

// Field add/sub in Montgomery form: values are < p, so add-then-reduce once.
static void fadd_mont(const ec_ctx_t* e, tls_bn_t* r, const tls_bn_t* a, const tls_bn_t* b) {
    uint32_t n = e->fp.n;
    uint64_t carry = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t v = (uint64_t)a->w[i] + b->w[i] + carry;
        r->w[i] = (uint32_t)v;
        carry = v >> 32;
    }
    bn_norm(r, n);
    if (carry || bn_ge_n(r->w, e->fp.m.w, n)) {
        bn_sub(r->w, e->fp.m.w, n);
        bn_norm(r, n);
    }
}

static void fsub_mont(const ec_ctx_t* e, tls_bn_t* r, const tls_bn_t* a, const tls_bn_t* b) {
    uint32_t n = e->fp.n;
    int64_t borrow = 0;
    for (uint32_t i = 0; i < n; i++) {
        int64_t v = (int64_t)a->w[i] - (int64_t)b->w[i] - borrow;
        if (v < 0) { v += ((int64_t)1 << 32); borrow = 1; } else borrow = 0;
        r->w[i] = (uint32_t)v;
    }
    bn_norm(r, n);
    if (borrow) {                       // r += p
        uint64_t carry = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t v = (uint64_t)r->w[i] + e->fp.m.w[i] + carry;
            r->w[i] = (uint32_t)v;
            carry = v >> 32;
        }
        bn_norm(r, n);
    }
}

static int fzero(const tls_bn_t* a) { return tls_bn_is_zero(a); }

static void jac_set_infinity(jac_t* j) {
    tls_bn_set_u32(&j->X, 1);
    tls_bn_set_u32(&j->Y, 1);
    tls_bn_set_u32(&j->Z, 0);
}

static int jac_is_infinity(const jac_t* j) {
    return fzero(&j->Z);
}

static void jac_from_affine(const ec_ctx_t* e, jac_t* j, const uint8_t* xb,
                            const uint8_t* yb) {
    tls_bn_t x, y;
    tls_bn_from_bytes(&x, xb, e->c->bytes);
    tls_bn_from_bytes(&y, yb, e->c->bytes);
    mont_mul_ctx(&e->fp, &j->X, &x, &e->fp.r2);
    mont_mul_ctx(&e->fp, &j->Y, &y, &e->fp.r2);
    tls_bn_t one;
    tls_bn_set_u32(&one, 1);
    mont_mul_ctx(&e->fp, &j->Z, &one, &e->fp.r2);   // Z = 1 in Montgomery form
}

// P1 = P1 + P2 (Jacobian, both general), a = -3 (add-2007-bl)
static void jac_add(const ec_ctx_t* e, jac_t* p1, const jac_t* p2) {
    if (jac_is_infinity(p1)) { *p1 = *p2; return; }
    if (jac_is_infinity(p2)) return;
    tls_bn_t z1z1, z2z2, u1, u2, s1, s2, h, i, j, r, v, t1, t2;
    fmul(e, &z1z1, &p1->Z, &p1->Z);
    fmul(e, &z2z2, &p2->Z, &p2->Z);
    fmul(e, &u1, &p1->X, &z2z2);
    fmul(e, &u2, &p2->X, &z1z1);
    tls_bn_t y1z2;
    fmul(e, &y1z2, &p1->Y, &p2->Z);
    fmul(e, &s1, &y1z2, &z2z2);
    tls_bn_t y2z1;
    fmul(e, &y2z1, &p2->Y, &p1->Z);
    fmul(e, &s2, &y2z1, &z1z1);
    fsub_mont(e, &h, &u2, &u1);
    fsub_mont(e, &r, &s2, &s1);
    if (fzero(&h)) {
        if (fzero(&r)) { jac_double(e, p1); return; }
        jac_set_infinity(p1);
        return;
    }
    fadd_mont(e, &t1, &h, &h);            // 2H
    fmul(e, &i, &t1, &t1);                // I = (2H)^2
    fmul(e, &j, &h, &i);                  // J = H*I
    // r is doubled ONCE, in place, because both X3 and Y3 below are defined
    // in terms of the doubled r. Squaring the doubled r into a scratch word
    // and then multiplying by the undoubled one in Y3 produces a point with a
    // plausible X and a Y that is not on the curve -- which then poisons every
    // later addition instead of failing loudly.
    fadd_mont(e, &r, &r, &r);             // r = 2*(S2-S1)
    fmul(e, &v, &u1, &i);
    // X3 = r^2 - J - 2V
    fmul(e, &t1, &r, &r);
    fsub_mont(e, &t1, &t1, &j);
    fadd_mont(e, &t2, &v, &v);
    fsub_mont(e, &p1->X, &t1, &t2);
    // Y3 = r*(V - X3) - 2*S1*J
    fsub_mont(e, &t2, &v, &p1->X);
    fmul(e, &t2, &r, &t2);
    fmul(e, &s1, &s1, &j);
    fadd_mont(e, &s1, &s1, &s1);
    fsub_mont(e, &p1->Y, &t2, &s1);
    // Z3 = ((Z1+Z2)^2 - Z1Z1 - Z2Z2)*H
    fadd_mont(e, &t1, &p1->Z, &p2->Z);
    fmul(e, &t1, &t1, &t1);
    fsub_mont(e, &t1, &t1, &z1z1);
    fsub_mont(e, &t1, &t1, &z2z2);
    fmul(e, &p1->Z, &t1, &h);
}

// P1 = 2*P1 (Jacobian, a = -3, dbl-2001-l)
static void jac_double(const ec_ctx_t* e, jac_t* p1) {
    if (jac_is_infinity(p1)) return;
    tls_bn_t xx, yy, yyyy, zz, zz2, s, m, t, t2, u;
    fmul(e, &xx, &p1->X, &p1->X);
    fmul(e, &yy, &p1->Y, &p1->Y);
    fmul(e, &yyyy, &yy, &yy);
    fmul(e, &zz, &p1->Z, &p1->Z);
    // S = 2*((X1+YY)^2 - XX - YYYY)
    fadd_mont(e, &u, &p1->X, &yy);
    fmul(e, &u, &u, &u);
    fsub_mont(e, &u, &u, &xx);
    fsub_mont(e, &u, &u, &yyyy);
    fmul(e, &s, &e->two, &u);
    // M = 3*XX - 3*ZZ^2   (a = -3)
    fmul(e, &m, &e->three, &xx);
    fmul(e, &zz2, &zz, &zz);
    fmul(e, &t, &e->three, &zz2);
    fsub_mont(e, &m, &m, &t);
    // T = M^2 - 2S
    fmul(e, &t, &m, &m);
    fadd_mont(e, &u, &s, &s);
    fsub_mont(e, &t, &t, &u);
    // Y3 = M*(S - T) - 8*YYYY. Keep Y1: Z3 below is built from the ORIGINAL
    // Y1, and p1->Y is about to be overwritten with Y3.
    fmul(e, &t2, &e->eight, &yyyy);
    tls_bn_t y1 = p1->Y;
    fsub_mont(e, &u, &s, &t);
    fmul(e, &u, &m, &u);
    fsub_mont(e, &p1->Y, &u, &t2);
    // X3 = T ; Z3 = (Y1+Z1)^2 - YY - ZZ
    p1->X = t;
    fadd_mont(e, &u, &y1, &p1->Z);
    fmul(e, &u, &u, &u);
    fsub_mont(e, &u, &u, &yy);
    fsub_mont(e, &p1->Z, &u, &zz);
}

// Convert out of Jacobian into affine X (and Y, when asked).
static void jac_to_affine_x(const ec_ctx_t* e, const jac_t* p, tls_bn_t* x) {
    // zinv = Z^(p-2) mod p (Fermat) — the field is prime.
    tls_bn_t pm, pm2;
    tls_bn_from_bytes(&pm, e->c->p, e->c->bytes);
    pm2 = pm;
    uint64_t borrow = 2;
    for (uint32_t i = 0; i < (uint32_t)pm2.n && borrow; i++) {
        uint64_t v = (uint64_t)pm2.w[i] - (borrow & 0xffffffffu);
        pm2.w[i] = (uint32_t)v;
        borrow = (v >> 32) ? 1 : 0;
    }
    uint8_t exp[48];
    tls_bn_to_bytes(&pm2, exp, e->c->bytes);

    tls_bn_t zmont, one, acc;
    // Z arrives already in Montgomery form from every jac_* routine, so it is
    // copied, NOT converted: running it through R^2 here would bolt on a
    // second R and quietly invert the wrong number.
    zmont = p->Z;
    tls_bn_set_u32(&one, 1);
    mont_mul_ctx(&e->fp, &one, &one, &e->fp.r2);      // one, Montgomery form
    acc = one;
    for (uint32_t k = 0; k < e->c->bytes * 8; k++) {
        mont_mul_ctx(&e->fp, &acc, &acc, &acc);
        if (be_bit(exp, e->c->bytes, k)) mont_mul_ctx(&e->fp, &acc, &acc, &zmont);
    }
    tls_bn_t zinv2;
    fmul(e, &zinv2, &acc, &acc);
    fmul(e, x, &p->X, &zinv2);
}

int tls_ecdsa_verify(int curve, const uint8_t* pub, uint32_t publen,
                     const uint8_t* digest, uint32_t digest_len,
                     const uint8_t* sig, uint32_t siglen) {
    const ec_curve_t* c = NULL;
    for (int i = 0; i < 2; i++) if (CURVES[i].id == curve) c = &CURVES[i];
    if (!c) return -1;
    if (publen != c->bytes * 2) return -1;
    if (siglen != c->bytes * 2) return -1;

    ec_ctx_t e;
    ec_ctx_init(&e, c);

    tls_bn_t r, s;
    tls_bn_from_bytes(&r, sig, c->bytes);
    tls_bn_from_bytes(&s, sig + c->bytes, c->bytes);
    tls_bn_t nb;
    tls_bn_from_bytes(&nb, c->n, c->bytes);
    if (tls_bn_is_zero(&r) || tls_bn_cmp(&r, &nb) >= 0) return -1;
    if (tls_bn_is_zero(&s) || tls_bn_cmp(&s, &nb) >= 0) return -1;

    // w = s^-1 mod n  (Fermat)
    uint8_t nminus2[48];
    {
        tls_bn_t nm2;
        nm2 = nb;
        uint64_t borrow = 2;
        for (uint32_t i = 0; i < (uint32_t)nm2.n && borrow; i++) {
            uint64_t v = (uint64_t)nm2.w[i] - (borrow & 0xffffffffu);
            nm2.w[i] = (uint32_t)v;
            borrow = (v >> 32) ? 1 : 0;
        }
        while (nm2.n > 0 && nm2.w[nm2.n - 1] == 0) nm2.n--;
        tls_bn_to_bytes(&nm2, nminus2, c->bytes);
    }
    tls_bn_t w;
    tls_bn_modexp(&w, &s, nminus2, c->bytes, &nb);

    // z = the leftmost `min(bitlen(n), bitlen(hash))` bits of the digest
    uint8_t zbytes[48];
    uint32_t zlim = c->bytes < digest_len ? c->bytes : digest_len;
    tls_memset(zbytes, 0, sizeof(zbytes));
    tls_memcpy(zbytes, digest + (digest_len - zlim), zlim);
    tls_bn_t z;
    tls_bn_from_bytes(&z, zbytes, c->bytes);

    // u1 = z*w mod n, u2 = r*w mod n — computed in Montgomery form and then
    // converted BACK to plain integers, because they are scalars below and
    // the scalar loops read them as plain bytes.
    tls_bn_t u1, u2;
    {
        tls_bn_t zm, wm, rm, plain_one;
        mont_mul_ctx(&e.fn, &zm, &z, &e.fn.r2);
        mont_mul_ctx(&e.fn, &wm, &w, &e.fn.r2);
        mont_mul_ctx(&e.fn, &rm, &r, &e.fn.r2);
        tls_bn_set_u32(&plain_one, 1);
        mont_mul_ctx(&e.fn, &u1, &zm, &wm);
        mont_mul_ctx(&e.fn, &u2, &rm, &wm);
        // Leaving Montgomery form is a multiply by the PLAIN 1, because every
        // Montgomery multiplication ends in an R^-1. Multiplying by the
        // Montgomery IMAGE of 1 (which is R itself) cancels that R^-1 and
        // hands back the Montgomery form, which then feeds the scalar loops as
        // the wrong integer entirely.
        mont_mul_ctx(&e.fn, &u1, &u1, &plain_one);
        mont_mul_ctx(&e.fn, &u2, &u2, &plain_one);
    }

    // R = u1*G + u2*Q  (double-and-add over both scalars)
    jac_t acc, pg, pq;
    jac_set_infinity(&acc);
    jac_from_affine(&e, &pg, c->gx, c->gy);
    jac_from_affine(&e, &pq, pub, pub + c->bytes);

    uint8_t ubuf[48];
    for (int which = 0; which < 2; which++) {
        const tls_bn_t* k = which ? &u2 : &u1;
        tls_bn_to_bytes(k, ubuf, c->bytes);
        jac_t sumG, sumQ;
        jac_set_infinity(&sumG);
        jac_set_infinity(&sumQ);
        for (uint32_t bit = 0; bit < c->bytes * 8; bit++) {
            jac_double(&e, &sumG);
            jac_double(&e, &sumQ);
            if (be_bit(ubuf, c->bytes, bit)) {
                jac_add(&e, &sumG, &pg);
                jac_add(&e, &sumQ, &pq);
            }
        }
        if (which == 0) acc = sumG;
        else jac_add(&e, &acc, &sumQ);
    }

    if (jac_is_infinity(&acc)) return -1;
    tls_bn_t vx;
    jac_to_affine_x(&e, &acc, &vx);
    // vx is in Montgomery form w.r.t. p: leave it with a multiply by the plain
    // 1, then bring the plain x back through Montgomery mod n, which reduces
    // it at the same time.
    tls_bn_t vx_plain, plain_one_p;
    tls_bn_set_u32(&plain_one_p, 1);
    mont_mul_ctx(&e.fp, &vx_plain, &vx, &plain_one_p);
    tls_bn_t vmont, vred, plain_one_n;
    tls_bn_set_u32(&plain_one_n, 1);
    mont_mul_ctx(&e.fn, &vmont, &vx_plain, &e.fn.r2);
    mont_mul_ctx(&e.fn, &vred, &vmont, &plain_one_n);
    return tls_bn_cmp(&vred, &r) == 0 ? 0 : -1;
}
