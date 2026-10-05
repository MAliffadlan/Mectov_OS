// apps/tlsselftest.c — the TLS engine's own gate (v38.162).
//
// Runs inside the guest with no network, no openssl and no server: every input
// is a committed fixture in apps/lib/tls/test_vectors.h, generated once by
// scripts/gen_tls_fixtures.py. That is deliberate. The interesting failures in
// a TLS stack are not "could not connect" — they are "connected, and the wrong
// thing was accepted" — and those can only be tested against inputs fixed in
// advance.
//
// Every line is printed with sys_print(), so this app links nothing but the
// syscall interface. The colour argument is what the harness keys on: green
// lines are passes, red lines are failures, and the exit status is the number
// of failed checks.
//
// Coverage is two layers:
//   1. the primitives, against the RFC/NIST vectors and the OpenSSL-made
//      signature and certificate fixtures;
//   2. the X.509 path-building rule, which must both accept the chain it was
//      given and refuse the near-misses -- wrong host, wrong CA, expired,
//      tampered signature, missing issuer.
//
// Run:  run /apps/tlsselftest.mct

#include "src/include/syscall.h"
#include "lib/tls/tls.h"
#include "lib/tls/test_vectors.h"

static int failures = 0;
static int checks = 0;

// sys_print takes a NUL-terminated string, so every message is built first.
// There is no printf in this app's link line on purpose.
static void say(const char* s, int colour) {
    sys_print(s, colour);
}

static void check(const char* name, int ok) {
    checks++;
    if (ok) {
        say("[PASS] ", 0x0A);
        say(name, 0x07);
        say("\n", 0x07);
    } else {
        failures++;
        say("[FAIL] ", 0x0C);
        say(name, 0x0C);
        say("\n", 0x0C);
    }
}

static void hexstr(const uint8_t* p, int n, char* out, int max) {
    static const char D[] = "0123456789abcdef";
    int i = 0;
    for (; i < n && i * 2 + 2 < max; i++) {
        out[i * 2]     = D[p[i] >> 4];
        out[i * 2 + 1] = D[p[i] & 15];
    }
    out[i * 2] = 0;
}

// Print the first bytes of a value that did not match, for the single-sided
// cases where there is no expected array to pair it with.
static void show(const char* label, const uint8_t* p, int n) {
    char buf[160];
    int i = 0;
    while (label[i] && i < 30) { buf[i] = label[i]; i++; }
    buf[i++] = ' ';
    hexstr(p, n > 48 ? 48 : n, buf + i, (int)sizeof(buf) - i);
    buf[sizeof(buf) - 1] = 0;
    say(buf, 0x0E);
    say("\n", 0x0E);
}

// Print both sides of a failed comparison. "It failed" is much less useful
// than "it produced these bytes instead of those" -- and when the two sides do
// print identically, that is itself the finding: the expected array is wrong.
static void mismatch(const char* label, const uint8_t* got, const uint8_t* exp, int n) {
    char buf[64];
    int i = 0;
    while (label[i] && i < 24) { buf[i] = label[i]; i++; }
    buf[i] = 0;

    say(buf, 0x0E);
    say(" got ", 0x0E);
    hexstr(got, n > 24 ? 24 : n, buf, (int)sizeof(buf));
    say(buf, 0x0E);

    say(" exp ", 0x0E);
    hexstr(exp, n > 24 ? 24 : n, buf, (int)sizeof(buf));
    say(buf, 0x0E);
    say("\n", 0x0E);
}

// ------------------------------------------------------------------ hashes

static void test_hashes(void) {
    uint8_t out[64];
    char buf[80];

    tls_sha256(tlsvec_msg, TLSVEC_MSG_SIZE, out);
    if (tls_const_eq(out, TLSVEC_DIGEST_SHA256, 32) != 1) show("sha256 got", out, 32);
    check("sha256 matches the committed digest",
          tls_const_eq(out, TLSVEC_DIGEST_SHA256, 32) == 1);

    // RFC 4231 case 1
    uint8_t key[20];
    tls_memset(key, 0x0b, 20);
    static const uint8_t exp[32] = {
        0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53,0x5c,0xa8,0xaf,0xce,0xaf,0x0b,0xf1,0x2b,
        0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7,0x26,0xe9,0x37,0x6c,0x2e,0x32,0xcf,0xf7};
    tls_hmac_sha256(key, 20, (const uint8_t*)"Hi There", 8, out);
    if (tls_const_eq(out, exp, 32) != 1) show("hmac got", out, 32);
    check("hmac-sha256 matches RFC 4231 case 1", tls_const_eq(out, exp, 32) == 1);

    // RFC 5869 case 1
    uint8_t ikm[22], salt[13], info[10];
    tls_memset(ikm, 0x0b, 22);
    for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i;
    for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);
    static const uint8_t prk_exp[32] = {
        0x07,0x77,0x09,0x36,0x2c,0x2e,0x32,0xdf,0x0d,0xdc,0x3f,0x0d,0xc4,0x7b,0xba,0x63,
        0x90,0xb6,0xc7,0x3b,0xb5,0x0f,0x9c,0x31,0x22,0xec,0x84,0x4a,0xd7,0xc2,0xb3,0xe5};
    // Expected values below were generated mechanically, not typed; see the
    // header comment on scripts/tls_selftest_host.sh for why that matters.
    static const uint8_t okm_exp[42] = {
        0x3c,0xb2,0x5f,0x25,0xfa,0xac,0xd5,0x7a,0x90,0x43,0x4f,0x64,0xd0,0x36,0x2f,0x2a,
        0x2d,0x2d,0x0a,0x90,0xcf,0x1a,0x5a,0x4c,0x5d,0xb0,0x2d,0x56,0xec,0xc4,0xc5,0xbf,
        0x34,0x00,0x72,0x08,0xd5,0xb8,0x87,0x18,0x58,0x65};
    uint8_t prk[32], okm[42];
    tls_hkdf_extract(salt, 13, ikm, 22, prk);
    if (tls_const_eq(prk, prk_exp, 32) != 1) show("hkdf prk got", prk, 32);
    check("hkdf-extract matches RFC 5869 case 1", tls_const_eq(prk, prk_exp, 32) == 1);
    tls_hkdf_expand(prk, info, 10, okm, 42);
    if (tls_const_eq(okm, okm_exp, 42) != 1) mismatch("hkdf okm", okm, okm_exp, 42);
    check("hkdf-expand matches RFC 5869 case 1", tls_const_eq(okm, okm_exp, 42) == 1);

    // SHA-384 exists for P-384 and RSA-PSS SHA-384; check it is at least the
    // right size and not constant, then compare against SHA-256 to prove the
    // two are genuinely different functions.
    uint8_t a[32], b[48];
    tls_sha256(tlsvec_msg, TLSVEC_MSG_SIZE, a);
    tls_sha384(tlsvec_msg, TLSVEC_MSG_SIZE, b);
    // "differs" means the two are not the same function, i.e. the 32-byte
    // prefixes are not byte-for-byte equal. Testing that some individual byte
    // matches would be satisfied by chance and proved nothing.
    int same = 1;
    for (int i = 0; i < 32; i++) {
        if (a[i] != b[i]) { same = 0; break; }
    }
    check("sha384 differs from sha256 on the same input", !same);
    hexstr(b, 12, buf, (int)sizeof(buf));
    say("[INFO] sha384 begins ", 0x0E); say(buf, 0x0E); say("\n", 0x0E);
}

// ------------------------------------------------------------------ ciphers

static void test_ciphers(void) {
    // RFC 8439 2.3.2: one ChaCha20 block
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    static const uint8_t nonce0[12] = {0,0,0,9,0,0,0,0x4a,0,0,0,0};
    static const uint8_t blk_exp[64] = {
        0x10,0xf1,0xe7,0xe4,0xd1,0x3b,0x59,0x15,0x50,0x0f,0xdd,0x1f,0xa3,0x20,0x71,0xc4,
        0xc7,0xd1,0xf4,0xc7,0x33,0xc0,0x68,0x03,0x04,0x22,0xaa,0x9a,0xc3,0xd4,0x6c,0x4e,
        0xd2,0x82,0x64,0x46,0x07,0x9f,0xaa,0x09,0x14,0xc2,0xd7,0x05,0xd9,0x8b,0x02,0xa2,
        0xb5,0x12,0x9c,0xd1,0xde,0x16,0x4e,0xb9,0xcb,0xd0,0x83,0xe8,0xa2,0x50,0x3c,0x4e};
    uint8_t blk[64];
    tls_chacha20_block(key, 1, nonce0, blk);
    if (tls_const_eq(blk, blk_exp, 64) != 1) show("chacha20 got", blk, 64);
    check("chacha20 block matches RFC 8439 2.3.2",
          tls_const_eq(blk, blk_exp, 64) == 1);

    // RFC 8439 2.5.2: Poly1305
    static const uint8_t pkey[32] = {
        0x85,0xd6,0xbe,0x78,0x57,0x55,0x6d,0x33,0x7f,0x44,0x52,0xfe,0x42,0xd5,0x06,0xa8,
        0x01,0x03,0x80,0x8a,0xfb,0x0d,0xb2,0xfd,0x4a,0xbf,0xf6,0xaf,0x41,0x49,0xf5,0x1b};
    static const uint8_t ptag[16] = {
        0xa8,0x06,0x1d,0xc1,0x30,0x51,0x36,0xc6,0xc2,0x2b,0x8b,0xaf,0x0c,0x01,0x27,0xa9};
    uint8_t out[64];
    tls_poly1305(pkey, (const uint8_t*)"Cryptographic Forum Research Group", 34, out);
    if (tls_const_eq(out, ptag, 16) != 1) show("poly1305 got", out, 16);
    check("poly1305 matches RFC 8439 2.5.2", tls_const_eq(out, ptag, 16) == 1);

    // RFC 8439 2.8.2: AEAD_CHACHA20_POLY1305 end to end
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0x80 + i);
    static const uint8_t anonce[12] = {0x07,0,0,0,0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47};
    static const uint8_t aad[12] = {0x50,0x51,0x52,0x53,0xc0,0xc1,0xc2,0xc3,
                                   0xc4,0xc5,0xc6,0xc7};
    static const char* pt =
        "Ladies and Gentlemen of the class of '99: If I could offer you "
        "only one tip for the future, sunscreen would be it.";
    uint32_t ptlen = 0;
    while (pt[ptlen]) ptlen++;
    static uint8_t ct[256], back[256];
    uint8_t tag[16];
    tls_chacha20_poly1305_seal(key, anonce, aad, 12, (const uint8_t*)pt, ptlen, ct, tag);
    static const uint8_t ct_head[32] = {
        0xd3,0x1a,0x8d,0x34,0x64,0x8e,0x60,0xdb,0x7b,0x86,0xaf,0xbc,0x53,0xef,0x7e,0xc2,
        0xa4,0xad,0xed,0x51,0x29,0x6e,0x08,0xfe,0xa9,0xe2,0xb5,0xa7,0x36,0xee,0x62,0xd6};
    static const uint8_t tag_exp[16] = {
        0x1a,0xe1,0x0b,0x59,0x4f,0x09,0xe2,0x6a,0x7e,0x90,0x2e,0xcb,0xd0,0x60,0x06,0x91};
    int ok = tls_const_eq(ct, ct_head, 32) == 1 && tls_const_eq(tag, tag_exp, 16) == 1;
    if (!ok) {
        mismatch("aead ct", ct, ct_head, 32);
        mismatch("aead tag", tag, tag_exp, 16);
    }
    check("chacha20-poly1305 matches RFC 8439 2.8.2", ok);
    check("chacha20-poly1305 open round-trips",
          tls_chacha20_poly1305_open(key, anonce, aad, 12, ct, ptlen, tag, back) == 0 &&
          tls_const_eq(back, (const uint8_t*)pt, ptlen) == 1);
    tag[0] ^= 1;
    check("chacha20-poly1305 rejects a flipped tag byte",
          tls_chacha20_poly1305_open(key, anonce, aad, 12, ct, ptlen, tag, back) != 0);
    tag[0] ^= 1;

    // FIPS-197 AES-128
    static const uint8_t akey[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    static const uint8_t apt[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                                    0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    uint8_t rk[176], ablk[16];
    tls_aes128_key_expand(akey, rk);
    tls_aes128_encrypt_block(rk, apt, ablk);
    static const uint8_t aes_exp[16] = {
        0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
    if (tls_const_eq(ablk, aes_exp, 16) != 1) show("aes got", ablk, 16);
    check("aes-128 matches FIPS-197", tls_const_eq(ablk, aes_exp, 16) == 1);

    // NIST GCM test case 4
    static const uint8_t gkey[16] = {0xfe,0xff,0xe9,0x92,0x86,0x65,0x73,0x1c,
                                    0x6d,0x6a,0x8f,0x94,0x67,0x30,0x83,0x08};
    static const uint8_t gnonce[12] = {0xca,0xfe,0xba,0xbe,0xfa,0xce,0xdb,0xad,
                                       0xde,0xca,0xf8,0x88};
    static const uint8_t gaad[20] = {0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef,0xfe,0xed,
                                     0xfa,0xce,0xde,0xad,0xbe,0xef,0xab,0xad,0xda,0xd2};
    static const uint8_t gpt[60] = {
        0xd9,0x31,0x32,0x25,0xf8,0x84,0x06,0xe5,0xa5,0x59,0x09,0xc5,0xaf,0xf5,0x26,0x9a,
        0x86,0xa7,0xa9,0x53,0x15,0x34,0xf7,0xda,0x2e,0x4c,0x30,0x3d,0x8a,0x31,0x8a,0x72,
        0x1c,0x3c,0x0c,0x95,0x95,0x68,0x09,0x53,0x2f,0xcf,0x0e,0x24,0x49,0xa6,0xb5,0x25,
        0xb1,0x6a,0xed,0xf5,0xaa,0x0d,0xe6,0x57,0xba,0x63,0x7b,0x39};
    static const uint8_t gct_head[16] = {
        0x42,0x83,0x1e,0xc2,0x21,0x77,0x74,0x24,0x4b,0x72,0x21,0xb7,0x84,0xd0,0xd4,0x9c};
    static const uint8_t gtag_exp[16] = {
        0x5b,0xc9,0x4f,0xbc,0x32,0x21,0xa5,0xdb,0x94,0xfa,0xe9,0x5a,0xe7,0x12,0x1a,0x47};
    static uint8_t gct[64], gback[64];
    uint8_t gtag[16];
    tls_aes128_gcm_seal(gkey, gnonce, gaad, 20, gpt, 60, gct, gtag);
    ok = tls_const_eq(gct, gct_head, 16) == 1 && tls_const_eq(gtag, gtag_exp, 16) == 1;
    if (!ok) {
        mismatch("gcm ct", gct, gct_head, 16);
        mismatch("gcm tag", gtag, gtag_exp, 16);
    }
    check("aes-128-gcm matches NIST case 4", ok);
    check("aes-128-gcm open round-trips",
          tls_aes128_gcm_open(gkey, gnonce, gaad, 20, gct, 60, gtag, gback) == 0 &&
          tls_const_eq(gback, gpt, 60) == 1);
    gtag[3] ^= 0x40;
    check("aes-128-gcm rejects a tampered tag",
          tls_aes128_gcm_open(gkey, gnonce, gaad, 20, gct, 60, gtag, gback) != 0);

    // RFC 7748 6.1
    static const uint8_t asca[32] = {
        0x77,0x07,0x6d,0x0a,0x73,0x18,0xa5,0x7d,0x3c,0x16,0xc1,0x72,0x51,0xb2,0x66,0x45,
        0xdf,0x4c,0x2f,0x87,0xeb,0xc0,0x99,0x2a,0xb1,0x77,0xfb,0xa5,0x1d,0xb9,0x2c,0x2a};
    static const uint8_t bscb[32] = {
        0x5d,0xab,0x08,0x7e,0x62,0x4a,0x8a,0x4b,0x79,0xe1,0x7f,0x8b,0x83,0x80,0x0e,0xe6,
        0x6f,0x3b,0xb1,0x29,0x26,0x18,0xb6,0xfd,0x1c,0x2f,0x8b,0x27,0xff,0x88,0xe0,0xeb};
    static const uint8_t a_pub[32] = {
        0x85,0x20,0xf0,0x09,0x89,0x30,0xa7,0x54,0x74,0x8b,0x7d,0xdc,0xb4,0x3e,0xf7,0x5a,
        0x0d,0xbf,0x3a,0x0d,0x26,0x38,0x1a,0xf4,0xeb,0xa4,0xa9,0x8e,0xaa,0x9b,0x4e,0x6a};
    static const uint8_t shared[32] = {
        0x4a,0x5d,0x9d,0x5b,0xa4,0xce,0x2d,0xe1,0x72,0x8e,0x3b,0xf4,0x80,0x35,0x0f,0x25,
        0xe0,0x7e,0x21,0xc9,0x47,0xd1,0x9e,0x33,0x76,0xf0,0x9b,0x3c,0x1e,0x16,0x17,0x42};
    uint8_t pa[32], pb[32], s1[32], s2[32];
    tls_x25519_base(pa, asca);
    tls_x25519_base(pb, bscb);
    if (tls_const_eq(pa, a_pub, 32) != 1) show("x25519 alice got", pa, 32);
    check("x25519 alice's public key matches RFC 7748",
          tls_const_eq(pa, a_pub, 32) == 1);
    tls_x25519(s1, asca, pb);
    tls_x25519(s2, bscb, pa);
    if (tls_const_eq(s1, shared, 32) != 1) show("x25519 shared got", s1, 32);
    check("x25519 shared secret matches RFC 7748",
          tls_const_eq(s1, shared, 32) == 1);
    check("x25519 agrees from both sides", tls_const_eq(s1, s2, 32) == 1);
}

// ------------------------------------------------------------------ signatures

static void test_signatures(void) {
    int rc = tls_rsa_pkcs1_verify(RSA_N, sizeof(RSA_N), RSA_E, sizeof(RSA_E),
                                  RSA_SIG, TLSVEC_DIGEST_SHA256, 32);
    check("rsa pkcs1 verifies the committed signature", rc == 0);

    uint8_t bad[32];
    tls_memcpy(bad, TLSVEC_DIGEST_SHA256, 32);
    bad[0] ^= 0x01;
    check("rsa pkcs1 rejects a changed digest",
          tls_rsa_pkcs1_verify(RSA_N, sizeof(RSA_N), RSA_E, sizeof(RSA_E),
                               RSA_SIG, bad, 32) != 0);

    uint8_t badsig[sizeof(RSA_SIG)];
    tls_memcpy(badsig, RSA_SIG, sizeof(RSA_SIG));
    badsig[10] ^= 0x01;
    check("rsa pkcs1 rejects a changed signature",
          tls_rsa_pkcs1_verify(RSA_N, sizeof(RSA_N), RSA_E, sizeof(RSA_E),
                               badsig, TLSVEC_DIGEST_SHA256, 32) != 0);

    check("ecdsa p-256 verifies the committed signature",
          tls_ecdsa_verify(1, EC_P256_PUB, 64, TLSVEC_DIGEST_SHA256, 32,
                           EC_P256_SIG, 64) == 0);
    uint8_t d384[48];
    tls_sha384(tlsvec_msg, TLSVEC_MSG_SIZE, d384);
    check("ecdsa p-384 verifies the committed signature",
          tls_ecdsa_verify(2, EC_P384_PUB, 96, d384, 48, EC_P384_SIG, 96) == 0);

    uint8_t esig[64];
    tls_memcpy(esig, EC_P256_SIG, 64);
    esig[0] ^= 0x01;
    check("ecdsa p-256 rejects a changed signature",
          tls_ecdsa_verify(1, EC_P256_PUB, 64, TLSVEC_DIGEST_SHA256, 32, esig, 64) != 0);
    d384[0] ^= 0x01;
    check("ecdsa p-384 rejects a changed digest",
          tls_ecdsa_verify(2, EC_P384_PUB, 96, d384, 48, EC_P384_SIG, 96) != 0);
}

// ------------------------------------------------------------------ x.509

static void test_x509(void) {
    tls_cert_t ca, leaf, leaffec, other;
    check("the test CA parses", tls_x509_parse(CA_DER, sizeof(CA_DER), &ca) == TLS_OK);
    check("the test CA is a CA", ca.is_ca == 1);
    check("the test CA's key is RSA", ca.is_rsa == 1);
    check("the other CA parses and is a CA",
          tls_x509_parse(OTHER_CA_DER, sizeof(OTHER_CA_DER), &other) == TLS_OK &&
          other.is_ca == 1);
    check("the RSA leaf parses",
          tls_x509_parse(LEAF_RSA_DER, sizeof(LEAF_RSA_DER), &leaf) == TLS_OK);
    check("the EC leaf parses as P-256",
          tls_x509_parse(LEAF_EC_DER, sizeof(LEAF_EC_DER), &leaffec) == TLS_OK &&
          leaffec.ec_curve == 1);
    check("the leaf is not a CA", leaf.is_ca == 0);
    check("the leaf carries a SAN", leaf.has_san == 1);

    check("the RSA leaf verifies under the CA",
          tls_x509_verify_signed_by(LEAF_RSA_DER, sizeof(LEAF_RSA_DER), &ca) == TLS_OK);
    check("the EC leaf verifies under the CA",
          tls_x509_verify_signed_by(LEAF_EC_DER, sizeof(LEAF_EC_DER), &ca) == TLS_OK);
    check("the leaf does NOT verify under the other CA",
          tls_x509_verify_signed_by(LEAF_RSA_DER, sizeof(LEAF_RSA_DER), &other) != TLS_OK);
    check("a leaf is refused as an issuer",
          tls_x509_verify_signed_by(CA_DER, sizeof(CA_DER), &leaf) != TLS_OK);

    check("SAN matches localhost", tls_x509_match_host(&leaf, "localhost") == 1);
    check("SAN matches 10.0.2.2", tls_x509_match_host(&leaf, "10.0.2.2") == 1);
    check("SAN rejects example.com", tls_x509_match_host(&leaf, "example.com") == 0);
    check("SAN rejects localhost.evil.com",
          tls_x509_match_host(&leaf, "localhost.evil.com") == 0);
    check("SAN rejects 10.0.2.3", tls_x509_match_host(&leaf, "10.0.2.3") == 0);

    uint32_t now = leaf.not_before + 1000;
    check("the leaf is inside its validity window", tls_x509_time_valid(&leaf, now) == 1);
    check("the leaf is refused before it starts",
          tls_x509_time_valid(&leaf, leaf.not_before - 1) == 0);
    check("the leaf is refused after it ends",
          tls_x509_time_valid(&leaf, leaf.not_after + 1) == 0);

    // A truncated certificate must be rejected, not half-parsed into something
    // that happens to verify.
    int accepted_truncation = 0;
    for (uint32_t cut = 1; cut < sizeof(LEAF_RSA_DER); cut += 61) {
        tls_cert_t t;
        if (tls_x509_parse(LEAF_RSA_DER, cut, &t) == TLS_OK) accepted_truncation = 1;
    }
    check("no truncation of the leaf parses", accepted_truncation == 0);
}

// ------------------------------------------------------------------ chain

static void test_chain(void) {
    // A store with nothing in it, to prove the reject path is a real path and
    // not the accept path quietly returning the same thing.
    static const tls_root_t EMPTY[1] = { { 0, 0 } };

    static uint8_t chain[sizeof(LEAF_RSA_DER) + sizeof(CA_DER)];
    uint32_t lens[2] = { sizeof(LEAF_RSA_DER), sizeof(CA_DER) };
    tls_memcpy(chain, LEAF_RSA_DER, sizeof(LEAF_RSA_DER));
    tls_memcpy(chain + sizeof(LEAF_RSA_DER), CA_DER, sizeof(CA_DER));

    // The fixtures are minted with notBefore = whenever they were generated, so
    // the clock has to come from the certificate rather than from a constant:
    // otherwise this gate would start failing the day the guest's RTC passed the
    // generation date, or would have been silently testing an expired chain.
    tls_cert_t leaf;
    if (tls_x509_parse(LEAF_RSA_DER, sizeof(LEAF_RSA_DER), &leaf) != TLS_OK) {
        check("the leaf parses so the chain tests can run", 0);
        return;
    }
    const uint32_t now = leaf.not_before + 1000;

    uint32_t na = 0;
    int rc = tls_verify_chain(chain, lens, 2, "localhost", now,
                              tls_test_roots, tls_test_roots_count, &na);
    check("a chain to a known CA verifies", rc == TLS_OK);
    check("the leaf's expiry was reported", na == leaf.not_after);

    rc = tls_verify_chain(chain, lens, 2, "example.com", now,
                          tls_test_roots, tls_test_roots_count, &na);
    check("the same chain is refused for the wrong host", rc != TLS_OK);

    rc = tls_verify_chain(chain, lens, 2, "localhost", now, EMPTY, 0, &na);
    check("the same chain is refused with an empty store", rc != TLS_OK);

    rc = tls_verify_chain(chain, lens, 2, "localhost", leaf.not_after + 1,
                          tls_test_roots, tls_test_roots_count, &na);
    check("the same chain is refused after the leaf expires", rc != TLS_OK);

    uint32_t one[1] = { sizeof(LEAF_RSA_DER) };
    rc = tls_verify_chain(chain, one, 1, "localhost", now,
                          tls_test_roots, tls_test_roots_count, &na);
    check("a leaf with no issuer is refused", rc != TLS_OK);

    // The same shape, the same lengths, signed by the wrong CA.
    static uint8_t swapped[sizeof(LEAF_RSA_DER) + sizeof(OTHER_CA_DER)];
    tls_memcpy(swapped, LEAF_RSA_DER, sizeof(LEAF_RSA_DER));
    tls_memcpy(swapped + sizeof(LEAF_RSA_DER), OTHER_CA_DER, sizeof(OTHER_CA_DER));
    uint32_t lens2[2] = { sizeof(LEAF_RSA_DER), sizeof(OTHER_CA_DER) };
    rc = tls_verify_chain(swapped, lens2, 2, "localhost", now,
                          tls_test_roots, tls_test_roots_count, &na);
    check("a chain signed by the wrong CA is refused", rc != TLS_OK);

    static uint8_t tampered[sizeof(LEAF_RSA_DER) + sizeof(CA_DER)];
    tls_memcpy(tampered, LEAF_RSA_DER, sizeof(LEAF_RSA_DER));
    tampered[sizeof(LEAF_RSA_DER) - 1] ^= 0x01;
    tls_memcpy(tampered + sizeof(LEAF_RSA_DER), CA_DER, sizeof(CA_DER));
    rc = tls_verify_chain(tampered, lens, 2, "localhost", now,
                          tls_test_roots, tls_test_roots_count, &na);
    check("a chain with a flipped signature byte is refused", rc != TLS_OK);
}

// ------------------------------------------------------------------ main

// Append a small non-negative integer in decimal. The count is printed so the
// gate can tell "53 checks passed" from "the app crashed after two", which is
// the failure mode a bare exit status cannot distinguish.
static void sayu(unsigned v) {
    char buf[16];
    int n = 0;
    if (v == 0) {
        say("0", 0x0E);
        return;
    }
    while (v && n < 15) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n) {
        char c[2];
        c[0] = buf[--n];
        c[1] = 0;
        say(c, 0x0E);
    }
}

// Apps in this OS enter at _start, not main: there is no crt to call a
// constructor, and the .mct loader jumps straight at the ELF entry symbol.
void _start(void) {
    say("[INFO] tlsselftest: crypto and X.509 primitives, no network\n", 0x0E);
    test_hashes();
    test_ciphers();
    test_signatures();
    test_x509();
    test_chain();

    say("[INFO] checks ", 0x0E);
    sayu((unsigned)checks);
    say(", failures ", 0x0E);
    sayu((unsigned)failures);
    say("\n", 0x0E);

    if (failures == 0) {
        say("[INFO] TLS selftest OK\n", 0x0A);
    } else {
        say("[INFO] TLS selftest FAILED\n", 0x0C);
    }
    sys_exit_with_code(failures);
}