// apps/lib/tls/tls.h — Mectov OS Ring 3 TLS 1.3 client (v38.162)
//
// A self-contained TLS 1.3 client with real certificate verification, built
// for this kernel's userland: no libc, no libgcc (no 64-bit division anywhere
// — the 32-bit build would need __umoddi3/__udivdi3, which nothing links),
// no dynamic allocation. Everything lives in fixed static buffers that each
// caller owns.
//
// Scope (deliberate, and stated in docs/architecture/tls.md):
//   * TLS 1.3 only. No TLS 1.2, no renegotiation, no session resumption/PSK,
//     no 0-RTT, no client certificates, no OCSP/CRL/AIA fetching.
//   * Cipher suites: TLS_CHACHA20_POLY1305_SHA256 (0x1303) and
//     TLS_AES_128_GCM_SHA256 (0x1301).
//   * Key exchange: X25519.
//   * Certificate signatures: RSA PKCS#1 v1.5 and RSA-PSS (SHA-256/384/512),
//     ECDSA P-256 and P-384 (SHA-256/384). The chain must reach a trust anchor
//     in tls_roots.h; the server must send its intermediates (no AIA).
//
// Transport is two caller-supplied callbacks, so the same engine drives a
// blocking app (tlstest) and the browser's non-blocking poll loop.

#ifndef MCT_TLS_H
#define MCT_TLS_H

#include "src/include/types.h"

// ------------------------------------------------------------------ errors
#define TLS_OK              0
#define TLS_WANT_READ       1   // need more bytes from the peer
#define TLS_WANT_WRITE      2   // pending output could not be flushed yet
#define TLS_ERR_IO         -1   // transport refused/closed
#define TLS_ERR_PROTOCOL   -2   // malformed or unexpected TLS data
#define TLS_ERR_VERIFY     -3   // chain / signature / hostname verification failed
#define TLS_ERR_UNSUPPORTED -4  // no common cipher suite or unsupported curve/algo
#define TLS_ERR_ENTROPY    -5   // getrandom failed — never fall back to weak RNG
#define TLS_ERR_OVERFLOW   -6   // fixed buffer too small for the peer's message
#define TLS_CLOSED         -7   // peer sent close_notify / clean EOF

// ------------------------------------------------------------------ sizing
#define TLS_MAX_RECORD     16640   // 2^14 + 256: the largest legal TLS record
#define TLS_MAX_PLAIN      16384
#define TLS_HS_MAX         12288   // reassembled handshake message (certs fit)
#define TLS_CHAIN_MAX      5       // certificates in the presented chain
#define TLS_HOSTNAME_MAX   64
#define TLS_ALERT_MAX      64

// ------------------------------------------------------------------ helpers
void tls_memcpy(void* d, const void* s, uint32_t n);
void tls_memset(void* d, int v, uint32_t n);
int  tls_memcmp(const void* a, const void* b, uint32_t n);
int  tls_const_eq(const void* a, const void* b, uint32_t n);
uint32_t tls_load24(const uint8_t* p);
uint32_t tls_load32(const uint8_t* p);
void tls_store16(uint8_t* p, uint32_t v);
void tls_store24(uint8_t* p, uint32_t v);

// ------------------------------------------------------------------ SHA-256 / HMAC / HKDF
typedef struct {
    uint32_t h[8];
    uint8_t  blk[64];
    uint32_t blen;
    uint64_t total;
} tls_sha256_t;

void tls_sha256_init(tls_sha256_t* c);
void tls_sha256_update(tls_sha256_t* c, const void* data, uint32_t len);
void tls_sha256_final(tls_sha256_t* c, uint8_t out[32]);
void tls_sha256(const void* data, uint32_t len, uint8_t out[32]);
// SHA-384/SHA-512 (FIPS 180-4): needed for P-384 chains and RSA-PSS SHA-384/512.
// Only the one-shot form is used by the verifier.
void tls_sha384(const void* data, uint32_t len, uint8_t out[48]);
void tls_sha512(const void* data, uint32_t len, uint8_t out[64]);
void tls_hmac_sha256(const uint8_t* key, uint32_t klen,
                     const uint8_t* data, uint32_t dlen, uint8_t out[32]);
void tls_hkdf_extract(const uint8_t* salt, uint32_t slen,
                      const uint8_t* ikm, uint32_t ilen, uint8_t out[32]);
void tls_hkdf_expand(const uint8_t* prk, const uint8_t* info, uint32_t ilen,
                     uint8_t* out, uint32_t outlen);
void tls_hkdf_expand_label(const uint8_t* secret, const char* label,
                           const uint8_t* ctx, uint32_t ctxlen,
                           uint8_t* out, uint32_t outlen);

// ------------------------------------------------------------------ ChaCha20 / Poly1305
void tls_chacha20_block(const uint8_t key[32], uint32_t counter,
                        const uint8_t nonce[12], uint8_t out[64]);
void tls_chacha20_xor(const uint8_t key[32], uint32_t counter,
                      const uint8_t nonce[12], const uint8_t* in,
                      uint8_t* out, uint32_t len);
void tls_poly1305(const uint8_t key[32], const uint8_t* msg, uint32_t len,
                  uint8_t tag[16]);
// RFC 8439 AEAD_CHACHA20_POLY1305. `out` may alias `in` (seal) / `ct` (open).
int tls_chacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t* aad, uint32_t aadlen,
                               const uint8_t* pt, uint32_t ptlen,
                               uint8_t* ct, uint8_t tag[16]);
int tls_chacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t* aad, uint32_t aadlen,
                               const uint8_t* ct, uint32_t ctlen,
                               const uint8_t tag[16], uint8_t* pt);

// ------------------------------------------------------------------ AES-128-GCM
void tls_aes128_key_expand(const uint8_t key[16], uint8_t rk[176]);
void tls_aes128_encrypt_block(const uint8_t rk[176], const uint8_t in[16],
                              uint8_t out[16]);
int tls_aes128_gcm_seal(const uint8_t key[16], const uint8_t nonce[12],
                        const uint8_t* aad, uint32_t aadlen,
                        const uint8_t* pt, uint32_t ptlen,
                        uint8_t* ct, uint8_t tag[16]);
int tls_aes128_gcm_open(const uint8_t key[16], const uint8_t nonce[12],
                        const uint8_t* aad, uint32_t aadlen,
                        const uint8_t* ct, uint32_t ctlen,
                        const uint8_t tag[16], uint8_t* pt);

// ------------------------------------------------------------------ X25519
void tls_x25519(uint8_t out[32], const uint8_t scalar[32],
                const uint8_t point[32]);
void tls_x25519_base(uint8_t out[32], const uint8_t scalar[32]);

// ------------------------------------------------------------------ big integer (fixed width, 32-bit limbs)
#define TLS_BN_LIMBS 128              // 4096 bits
typedef struct { uint32_t w[TLS_BN_LIMBS]; int n; } tls_bn_t;

void tls_bn_zero(tls_bn_t* a);
void tls_bn_from_bytes(tls_bn_t* a, const uint8_t* be, uint32_t len);
void tls_bn_to_bytes(const tls_bn_t* a, uint8_t* be, uint32_t len);
int  tls_bn_bitlen(const tls_bn_t* a);
int  tls_bn_is_zero(const tls_bn_t* a);
int  tls_bn_cmp(const tls_bn_t* a, const tls_bn_t* b);
void tls_bn_set_u32(tls_bn_t* a, uint32_t v);
// out = base^exp mod m (exp given as big-endian bytes)
void tls_bn_modexp(tls_bn_t* out, const tls_bn_t* base, const uint8_t* exp,
                   uint32_t explen, const tls_bn_t* m);
uint32_t tls_bn_mont_n0inv(const tls_bn_t* m);   // -m^-1 mod 2^32

// ------------------------------------------------------------------ signature verification
// RSA: `sig` is the modulus-sized big-endian signature; `digest` is the hash of
// the signed data (32/48/64 bytes — the DigestInfo prefix is looked up from its
// length, which is exactly the three hashes X.509 uses with RSA).
int tls_rsa_pkcs1_verify(const uint8_t* n, uint32_t nlen, const uint8_t* e,
                         uint32_t elen, const uint8_t* sig,
                         const uint8_t* digest, uint32_t digest_len);
int tls_rsa_pss_verify(const uint8_t* n, uint32_t nlen, const uint8_t* e,
                       uint32_t elen, const uint8_t* sig,
                       const uint8_t* digest, uint32_t digest_len);
// ECDSA over P-256 (curve=1) or P-384 (curve=2). `pub` is the uncompressed
// point without its 0x04 tag (x||y) and `sig` is r||s, both field-width.
int tls_ecdsa_verify(int curve, const uint8_t* pub, uint32_t publen,
                     const uint8_t* digest, uint32_t digest_len,
                     const uint8_t* sig, uint32_t siglen);

// ------------------------------------------------------------------ X.509
typedef struct {
    const uint8_t* spki_alg;    uint32_t spki_alg_len;   // outer signatureAlgorithm AlgorithmIdentifier (DER)
    const uint8_t* pub;         uint32_t pub_len;        // whole SubjectPublicKeyInfo element (DER TLV)
    int   is_ca;
    int   is_rsa;               // else EC
    int   ec_curve;             // 1 = P-256, 2 = P-384, 0 = unknown
    uint32_t not_before, not_after;                      // unix seconds
    const uint8_t* san;         uint32_t san_len;        // SAN GeneralNames SEQUENCE contents
    const uint8_t* cn;          uint32_t cn_len;         // last CN in subject, or NULL
    int   has_san;              // a SAN extension was present at all
} tls_cert_t;

// Parse one DER certificate; returns TLS_OK or a negative error.
int tls_x509_parse(const uint8_t* der, uint32_t len, tls_cert_t* out);
// Verify `child` against `issuer` (signature + CA flag). hostname check is separate.
int tls_x509_verify_signed_by(const uint8_t* child_der, uint32_t child_len,
                              const tls_cert_t* issuer);
// Does the cert's SAN (or CN fallback) match `host`? IP literals compared as IP.
int tls_x509_match_host(const tls_cert_t* c, const char* host);
// Is `now` (unix seconds) inside [not_before, not_after]? A certificate with no
// parseable validity is accepted, so an unusual-but-valid encoding cannot lock
// the client out of an otherwise good chain.
int tls_x509_time_valid(const tls_cert_t* c, uint32_t now);
// Is the cert allowed to sign other certificates (basicConstraints CA + the
// keyCertSign bit when a keyUsage extension is present)?
int tls_x509_is_ca(const tls_cert_t* c);
// SubjectPublicKeyInfo key extraction, used by the verifier and by anything
// that needs to know which algorithm an issuer's key speaks.
int tls_spki_rsa(const uint8_t* spki, uint32_t len,
                 uint8_t* n, uint32_t* nlen, uint8_t* e, uint32_t* elen);
int tls_spki_ec(const uint8_t* spki, uint32_t len, uint8_t* out,
                uint32_t* outlen, int* curve);

// Trust anchors, generated from a PEM directory or bundle by
// scripts/gen_tls_roots.py. Each entry is a DER Certificate.
typedef struct { const uint8_t* der; uint32_t len; } tls_root_t;
extern const tls_root_t tls_roots[];
extern const int tls_roots_count;

// Verify a presented chain against an explicit store. `certs` points at the
// concatenated certificates; lens[i] is each one's length. `host` is the name
// that must match the LEAF. This is the whole path-building rule in one
// function, parameterised on the store so the self-test can check a chain that
// terminates at a CA the real store has never heard of.
int tls_verify_chain(const uint8_t* certs, const uint32_t* lens, int count,
                     const char* host, uint32_t now,
                     const tls_root_t* roots, int root_count,
                     uint32_t* leaf_not_after);

// ------------------------------------------------------------------ connection
typedef struct tls_conn tls_conn_t;

// Transport: send returns bytes accepted (may be short; 0 = try later, <0 =
// error); recv returns bytes read (>0), 0 = nothing available yet, <0 = closed
// or error. Both are called only from tls_step()/tls_handshake().
typedef int (*tls_send_fn)(void* ctx, const uint8_t* buf, uint32_t len);
typedef int (*tls_recv_fn)(void* ctx, uint8_t* buf, uint32_t len);

// Called for each human-readable milestone ([TLS] ... lines). May be NULL.
typedef void (*tls_log_fn)(void* ctx, const char* msg);

struct tls_conn {
    // ---- transport
    tls_send_fn send;
    tls_recv_fn recv;
    void*       io;
    tls_log_fn  log;

    // ---- identity
    char        host[TLS_HOSTNAME_MAX + 1];

    // ---- handshake state
    int         state;
    int         suite;          // 0x1303 / 0x1301 / 0
    int         done;
    int         err;
    int         verify_ok;
    uint32_t    server_not_after;

    // ---- secrets
    // handshake_secret is the RFC 8446 Handshake Secret itself, kept because the
    // Master Secret is derived from IT ("derived" off the handshake stage) and
    // not from either handshake traffic secret.
    uint8_t     handshake_secret[32];
    uint8_t     client_hs_secret[32], server_hs_secret[32];
    uint8_t     client_ap_secret[32], server_ap_secret[32];
    uint8_t     client_key[32], server_key[32];
    uint8_t     client_iv[12],  server_iv[12];
    uint64_t    rd_seq, wr_seq;

    // ---- transcript
    tls_sha256_t transcript;

    // ---- record reader
    uint8_t     hdr[5];
    uint32_t    hdr_have;
    uint8_t     rec[TLS_MAX_RECORD];
    uint32_t    rec_have;
    uint32_t    rec_want;

    // ---- handshake reassembly
    uint8_t     hs[TLS_HS_MAX];
    uint32_t    hs_len;

    // ---- plaintext input for tls_read()
    uint8_t     in[TLS_MAX_PLAIN];
    uint32_t    in_len, in_off;

    // ---- pending output
    uint8_t     out[TLS_MAX_PLAIN];
    uint32_t    out_len;
    uint32_t    out_off;

    // ---- scratch the caller may use for logging
    char        msg[160];

    // ---- X25519 key share
    uint8_t     ks_priv[32], ks_pub[32], ks_peer[32];
    uint8_t     session_id[32];

    // ---- per-phase record keys. rd_*/wr_* are the CURRENT read/write key and
    // nonce base; enc_* says whether records are protected at all. Before the
    // ServerHello both directions are plaintext, after it both are encrypted.
    uint8_t     rd_key[32], rd_iv[12];
    uint8_t     wr_key[32], wr_iv[12];
    int         enc_rd, enc_wr;
    uint64_t    hs_rd_seq, hs_wr_seq;

    // ---- the presented chain, repacked from the wire list as one tightly
    // concatenated DER blob (the CertificateEntry framing is stripped), which
    // is the shape tls_verify_chain() walks. cert_off[] indexes into it.
    uint8_t     certs[TLS_HS_MAX];
    uint32_t    cert_off[TLS_CHAIN_MAX];
    uint32_t    cert_len[TLS_CHAIN_MAX];
    int         cert_count;

    uint8_t     finished_key[32];   // key for OUR Finished HMAC
    uint8_t     peer_finished[32];   // what the server claimed
    int         cert_verify_ok;
    uint8_t     last_alert[8];
    int         recv_closed;
};

// Initialise for `hostname` (used for SNI + certificate hostname check).
void tls_init(tls_conn_t* c, const char* hostname);
void tls_set_transport(tls_conn_t* c, tls_send_fn send, tls_recv_fn recv,
                       void* io, tls_log_fn log);
// Drive the handshake. Returns TLS_OK when established, TLS_WANT_READ /
// TLS_WANT_WRITE when the caller should call again (after the transport made
// progress), or a negative TLS_ERR_*.
int  tls_step(tls_conn_t* c);
// Blocking convenience wrapper: calls tls_step() until done or `timeout_ms`
// (monotonic, caller-supplied) passes. Pass now_ms = NULL to use a bounded
// retry count instead, which is what the non-browser callers want.
typedef uint32_t (*tls_now_fn)(void* ctx);
int  tls_handshake(tls_conn_t* c, tls_now_fn now_ms, void* now_ctx, uint32_t timeout_ms);
// Fill `buf` from the kernel CSPRNG. Returns 0 or -1; there is deliberately no
// fallback, because a weak handshake secret is worse than no handshake.
int  tls_random(void* buf, uint32_t len);

// Application data. tls_write encrypts and queues (returns bytes accepted, may
// be short — call again with the remainder). tls_read returns plaintext bytes,
// 0 when none is ready, TLS_CLOSED on close_notify, or a negative error.
int  tls_write(tls_conn_t* c, const void* buf, uint32_t len);
int  tls_read(tls_conn_t* c, void* buf, uint32_t len);
// Flush pending ciphertext; returns TLS_OK, TLS_WANT_WRITE, or an error.
int  tls_flush(tls_conn_t* c);
void tls_send_alert(tls_conn_t* c, int fatal, int desc);
void tls_send_close_notify(tls_conn_t* c);

int  tls_cipher_id(const tls_conn_t* c);
const char* tls_cipher_name(int suite);
const char* tls_last_alert(const tls_conn_t* c);

#endif // MCT_TLS_H
