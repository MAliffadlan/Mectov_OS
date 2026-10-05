// apps/lib/tls/tls13.c — the TLS 1.3 client itself (RFC 8446).
//
// Split into three layers that do not know about each other:
//
//   record layer   read/write TLS records, encrypt and decrypt them
//   handshake      the message state machine (ClientHello ... Finished)
//   key schedule   the RFC 8446 section 7.1 secret derivation
//
// Everything is driven by tls_step(). The caller supplies two transport
// callbacks, so the same code serves the blocking tlstest app and the
// browser's poll loop without either of them knowing about the other.
//
// Deliberate omissions, all of them stated in docs/architecture/tls.md:
// TLS 1.2, session resumption, 0-RTT, client certificates, HelloRetryRequest,
// and any form of certificate fetching beyond what the server sends.

#include "tls.h"
#include "tls_roots.h"
#include "src/include/syscall.h"

// ------------------------------------------------------------------ constants
#define TLS_CHACHA20_POLY1305_SHA256 0x1303
#define TLS_AES_128_GCM_SHA256       0x1301

#define REC_CHANGE_CIPHER 20
#define REC_ALERT         21
#define REC_HANDSHAKE     22
#define REC_APPLICATION   23

#define HS_CLIENT_HELLO   1
#define HS_SERVER_HELLO   2
#define HS_NEW_SESSION_TICKET 4
#define HS_ENCRYPTED_EXT  8
#define HS_CERTIFICATE   11
#define HS_CERT_REQUEST  13
#define HS_CERT_VERIFY   15
#define HS_FINISHED      20

#define EXT_SERVER_NAME        0
#define EXT_SUPPORTED_GROUPS  10
#define EXT_SIG_ALGS          13
#define EXT_SUPPORTED_VERSIONS 43
#define EXT_KEY_SHARE         51

#define GROUP_X25519        0x001d

#define HS_CLIENT 0
#define HS_SERVER 1

// Handshake states.
enum {
    ST_START = 0,
    ST_WAIT_SERVER_HELLO,
    ST_WAIT_ENCRYPTED_EXT,
    ST_WAIT_CERT,
    ST_WAIT_CERT_VERIFY,
    ST_WAIT_SERVER_FINISHED,
    ST_SEND_FINISHED,
    ST_DONE,
    ST_FAILED,
};

// ------------------------------------------------------------------ small bits

static void tls_logmsg(tls_conn_t* c, const char* m) {
    if (c->log) c->log(c->io, m);
}

// Log a short tag plus a number. The log callback existed from the start and
// was never called from anywhere, which is why a handshake that broke on real
// server input could only be diagnosed by reading the wire with a packet
// capture. Recording the handshake message type as it is processed makes the
// failure point visible from the serial log instead.
static void tls_lognum(tls_conn_t* c, const char* tag, int v) {
    if (!c->log) return;
    char b[48];
    int n = 0;
    while (tag[n] && n < 30) { b[n] = tag[n]; n++; }
    // The values logged here include error codes, which are negative, so the
    // sign has to be printed or -2 comes out as "." (the byte after '0').
    uint32_t u;
    if (v < 0) { b[n++] = '-'; u = (uint32_t)(-v); }
    else u = (uint32_t)v;
    if (u >= 100) b[n++] = (char)('0' + (u / 100) % 10);
    if (u >= 10)  b[n++] = (char)('0' + (u / 10) % 10);
    b[n++] = (char)('0' + (u % 10));
    b[n] = 0;
    tls_logmsg(c, b);
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

int tls_random(void* buf, uint32_t len) {
    // SYS_GETRANDOM draws from the kernel's ChaCha8 DRBG. If it fails there is
    // no second source worth having: a ClientHello built from a weak random
    // value is a downgrade waiting to happen, so the caller must abort.
    int rc = syscall(SYS_GETRANDOM, (int)(uintptr_t)buf, (int)len, 0);
    return rc == 0 ? 0 : -1;
}

// RFC 8446 7.1: HkdfLabel = uint16 length || "tls13 " + Label || context
static uint32_t label_len(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static void hkdf_label(const char* label, const uint8_t* ctx, uint32_t ctxlen,
                       uint8_t* out, uint32_t* outlen) {
    uint32_t n = 0;
    const uint32_t llen = 6 + label_len(label);
    tls_store16(out, *outlen); n += 2;
    out[n++] = (uint8_t)llen;
    tls_memcpy(out + n, "tls13 ", 6); n += 6;
    tls_memcpy(out + n, label, label_len(label)); n += label_len(label);
    out[n++] = (uint8_t)ctxlen;
    if (ctxlen) { tls_memcpy(out + n, ctx, ctxlen); n += ctxlen; }
    *outlen = n;
}

// A NULL Context is passed as a zero-length buffer, never as a pointer, because
// HKDF-Expand-Label encodes it with a one-byte length.
static void derive(uint8_t* out, uint32_t outlen, const uint8_t* secret,
                   const char* label, const uint8_t* ctx, uint32_t ctxlen) {
    uint8_t info[272];
    uint32_t infolen = (uint32_t)outlen;
    hkdf_label(label, ctx, ctxlen, info, &infolen);
    tls_hkdf_expand(secret, info, infolen, out, outlen);
}

// Derive-Secret with an EMPTY transcript, which is a value of its own and not
// the same thing as no context at all.
//
// The key schedule writes this step as Derive-Secret(Secret, "derived", ""),
// and the third argument is a transcript -- an empty one -- so the context is
// Transcript-Hash(""), i.e. SHA-256 of the empty string. Passing a zero-length
// context instead (the obvious reading of "no messages") is what RFC 8448
// catches: it pins the result at
// 6f2615a108c702c5678f54fc9dbab69716c076189c48250cebeac3576c3611ba for the
// early secret, and the zero-length-context form gives a different value.
//
// That one difference poisons the handshake secret, so BOTH handshake traffic
// secrets come out wrong. Every record up to the ServerHello is read in the
// clear, so nothing looks broken: the version, the key share and the transcript
// all check out, the suite is accepted, and then the peer's first encrypted
// record fails to authenticate with nothing on the wire to say why. The
// self-test could not see it either -- it drives the primitives with RFC
// vectors directly and never runs the key schedule through a live handshake.
static void derive_empty(uint8_t* out, uint32_t outlen, const uint8_t* secret,
                         const char* label) {
    uint8_t empty[32];
    tls_sha256("", 0, empty);
    derive(out, outlen, secret, label, empty, 32);
}

// ------------------------------------------------------------------ key schedule

static void transcript_hash(tls_conn_t* c, uint8_t out[32]) {
    // The transcript is hashed over the handshake messages exactly as they
    // appear on the wire, so the running SHA-256 is the only copy kept.
    tls_sha256_t t = c->transcript;
    tls_sha256_final(&t, out);
}

static void install_keys(uint8_t key[32], uint8_t iv[12],
                         const uint8_t* secret, int is_chacha) {
    uint32_t klen = is_chacha ? 32u : 16u;
    derive(key, klen, secret, "key", NULL, 0);
    derive(iv, 12, secret, "iv", NULL, 0);
}

// ------------------------------------------------------------------ record layer

static void nonce_for(uint8_t nonce[12], const uint8_t iv[12], uint64_t seq) {
    tls_memcpy(nonce, iv, 12);
    for (int i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(seq >> (8 * i));
}

// Encrypt one TLS 1.3 record into c->out. Returns TLS_OK or an error.
static int emit_record(tls_conn_t* c, int type, const uint8_t* data, uint32_t len) {
    if (len > TLS_MAX_PLAIN) return TLS_ERR_OVERFLOW;
    if (!c->enc_wr) {
        uint32_t n = 0;
        if (c->out_len - c->out_off + len + 5 > TLS_MAX_PLAIN) return TLS_ERR_OVERFLOW;
        uint8_t* o = c->out + c->out_off;
        o[n++] = (uint8_t)type;
        o[n++] = 0x03;
        o[n++] = 0x03;
        tls_store16(o + n, len); n += 2;
        tls_memcpy(o + n, data, len); n += len;
        c->out_len = c->out_off + n;
        return TLS_OK;
    }
    // inner plaintext = content || content type, no padding
    uint8_t inner[TLS_MAX_PLAIN + 1];
    tls_memcpy(inner, data, len);
    inner[len] = (uint8_t)type;
    uint8_t nonce[12];
    nonce_for(nonce, c->wr_iv, c->wr_seq);
    uint8_t tag[16];
    int is_chacha = (c->suite == TLS_CHACHA20_POLY1305_SHA256);
    uint32_t reclen = (uint32_t)len + 1 + 16;
    if (reclen > TLS_MAX_RECORD) return TLS_ERR_OVERFLOW;
    if (c->out_len - c->out_off + reclen + 5 > TLS_MAX_PLAIN) return TLS_ERR_OVERFLOW;
    uint8_t* o = c->out + c->out_off;
    // The header goes out first because it is not decoration: RFC 8446 5.2 makes
    // the AEAD's additional data the record header itself (type, version and
    // length). Passing a NULL/zero-length AAD instead still produces a
    // well-formed record -- the tag simply covers less than the peer's does --
    // so the failure only shows up as an unauthenticated record on the way IN,
    // which is why it took a live handshake to find.
    o[0] = REC_APPLICATION;           // outer type is always application_data
    o[1] = 0x03;
    o[2] = 0x03;
    tls_store16(o + 3, reclen);
    uint8_t aad[5];
    tls_memcpy(aad, o, 5);
    int rc;
    if (is_chacha) {
        rc = tls_chacha20_poly1305_seal(c->wr_key, nonce, aad, 5, inner, len + 1,
                                        o + 5, tag);
    } else {
        rc = tls_aes128_gcm_seal(c->wr_key, nonce, aad, 5, inner, len + 1,
                                 o + 5, tag);
    }
    if (rc != 0) return TLS_ERR_PROTOCOL;
    tls_memcpy(o + 5 + len + 1, tag, 16);
    c->out_len = c->out_off + 5 + reclen;
    c->wr_seq++;
    return TLS_OK;
}

int tls_flush(tls_conn_t* c) {
    while (c->out_off < c->out_len) {
        int n = c->send(c->io, c->out + c->out_off, c->out_len - c->out_off);
        if (n < 0) { c->err = TLS_ERR_IO; return TLS_ERR_IO; }
        if (n == 0) return TLS_WANT_WRITE;
        c->out_off += (uint32_t)n;
    }
    c->out_len = 0;
    c->out_off = 0;
    return TLS_OK;
}

// Pull one record's header + body out of the transport into c->rec.
// Returns TLS_OK with the record available, TLS_WANT_READ, or an error.
static int read_record(tls_conn_t* c) {
    // header
    while (c->hdr_have < 5) {
        int n = c->recv(c->io, c->hdr + c->hdr_have, 5 - c->hdr_have);
        if (n < 0) { c->err = TLS_ERR_IO; return TLS_ERR_IO; }
        if (n == 0) return TLS_WANT_READ;
        c->hdr_have += (uint32_t)n;
    }
    uint32_t rlen = rd16(c->hdr + 3);
    if (rlen > TLS_MAX_RECORD || rlen == 0) { c->err = TLS_ERR_PROTOCOL; return TLS_ERR_PROTOCOL; }
    c->rec_want = rlen;
    c->rec_have = 0;
    // body
    while (c->rec_have < c->rec_want) {
        int n = c->recv(c->io, c->rec + c->rec_have, c->rec_want - c->rec_have);
        if (n < 0) { c->err = TLS_ERR_IO; return TLS_ERR_IO; }
        if (n == 0) return TLS_WANT_READ;
        c->rec_have += (uint32_t)n;
    }
    c->hdr_have = 0;
    return TLS_OK;
}

// Decrypt the record just read into c->rec. On success *type is the inner
// content type and *plain/*plen point into c->rec.
static int open_record(tls_conn_t* c, int* type, uint8_t** plain, uint32_t* plen) {
    if (!c->enc_rd) {
        if (c->rec_want < 1) { c->err = TLS_ERR_PROTOCOL; return TLS_ERR_PROTOCOL; }
        // read_record() splits the record: c->hdr[0] is the content type and
        // c->rec holds ONLY the fragment. Reading the type out of c->rec[0]
        // returned whatever handshake message came first instead -- a
        // ServerHello announced itself as "record type 2" -- and no plaintext
        // record could ever be processed. The encrypted path is unaffected
        // because there the outer type is always application_data and the real
        // type is the last byte of the inner plaintext.
        *type  = c->hdr[0];
        *plain = c->rec;
        *plen  = c->rec_want;
        return TLS_OK;
    }
    if (c->rec_want < 17) { c->err = TLS_ERR_PROTOCOL; return TLS_ERR_PROTOCOL; }
    uint32_t ctlen = c->rec_want - 16;
    uint8_t nonce[12];
    nonce_for(nonce, c->rd_iv, c->rd_seq);
    int rc;
    // Decrypt IN PLACE. The output must not be offset from the input: writing
    // the plaintext one byte ahead of the ciphertext makes every write land on
    // the next input byte before it is read, so the result is corrupt from the
    // second byte on. It does not look like a decryption failure either --
    // these AEADs verify the tag over the ciphertext FIRST, so the tag passes
    // and the damage only shows up later as an unparsable handshake message.
    // Both ciphers read a byte before writing it, so in-place is safe here.
    // The additional data is the record header, which read_record() left intact
    // in c->hdr. Both ciphers read a byte before writing it, so in-place is
    // safe here; the plaintext goes back over the ciphertext at the same
    // offset, not one byte ahead of it.
    if (c->suite == TLS_CHACHA20_POLY1305_SHA256) {
        rc = tls_chacha20_poly1305_open(c->rd_key, nonce, c->hdr, 5, c->rec, ctlen,
                                        c->rec + ctlen, c->rec);
    } else {
        rc = tls_aes128_gcm_open(c->rd_key, nonce, c->hdr, 5, c->rec, ctlen,
                                 c->rec + ctlen, c->rec);
    }
    if (rc != 0) {
        tls_lognum(c, "record decrypt failed, ctlen ", (int)ctlen);
        c->err = TLS_ERR_PROTOCOL;
        return TLS_ERR_PROTOCOL;
    }
    c->rd_seq++;
    // The inner plaintext is content || content type || zero padding, so the
    // content type is the LAST non-zero byte; everything before it is content.
    uint32_t i = ctlen;
    while (i > 0 && c->rec[i - 1] == 0) i--;
    if (i == 0) {
        tls_logmsg(c, "record has no content type");
        c->err = TLS_ERR_PROTOCOL;
        return TLS_ERR_PROTOCOL;
    }
    *type  = c->rec[i - 1];
    *plen  = i - 1;
    *plain = c->rec;
    return TLS_OK;
}

// ------------------------------------------------------------------ alerts

static const char* alert_name(int d) {
    switch (d) {
        case 0:  return "close_notify";
        case 10: return "unexpected_message";
        case 20: return "bad_record_mac";
        case 40: return "handshake_failure";
        case 42: return "bad_certificate";
        case 43: return "unsupported_certificate";
        case 44: return "certificate_revoked";
        case 45: return "certificate_expired";
        case 46: return "certificate_unknown";
        case 47: return "illegal_parameter";
        case 48: return "unknown_ca";
        case 49: return "access_denied";
        case 50: return "decode_error";
        case 51: return "decrypt_error";
        case 70: return "protocol_version";
        case 80: return "internal_error";
        case 109: return "missing_extension";
        case 112: return "unrecognized_name";
        case 116: return "certificate_required";
        case 120: return "no_application_protocol";
        default:  return "alert";
    }
}

const char* tls_last_alert(const tls_conn_t* c) {
    return (const char*)c->last_alert;
}

void tls_send_alert(tls_conn_t* c, int fatal, int desc) {
    c->last_alert[0] = (uint8_t)desc;
    c->last_alert[1] = 0;
    if (c->done && !c->enc_wr) return;
    uint8_t body[2];
    body[0] = (uint8_t)(fatal ? 2 : 1);
    body[1] = (uint8_t)desc;
    emit_record(c, REC_ALERT, body, 2);
    tls_flush(c);
}

void tls_send_close_notify(tls_conn_t* c) {
    if (!c->done) return;
    tls_send_alert(c, 0, 0);
    c->done = 1;
}

// ------------------------------------------------------------------ handshake
// Signature algorithms advertised in the ClientHello, restricted to what this
// verifier can actually check. Advertising a suite the client cannot verify
// would only invite the server to pick it.
static const uint16_t SIG_ALGS[] = {
    0x0804, 0x0805, 0x0806,   // rsa_pss_rsae sha256/384/512
    0x0809, 0x080a, 0x080b,   // rsa_pss_pss  sha256/384/512
    0x0401, 0x0501, 0x0601,   // rsa_pkcs1    sha256/384/512
    0x0403, 0x0503,           // ecdsa        secp256r1/secp384r1
};

static uint32_t str_len(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

// Write a uint16-prefixed extension header and return the offset of the BODY --
// the two bytes of type and two of length are the four bytes before it.
//
// Returning the length-field offset instead cost this file its only
// client-visible bug: five callers did `ext_begin(...); n += 2;`, which left n
// pointing AT the length field, so each extension body overwrote its own
// header and every client hello was two bytes short per extension. Nothing
// offline could see it -- the handshake only ever ran against the stub in the
// self-test -- and the first real server answered BAD_EXTENSION.
static uint32_t ext_begin(uint8_t* b, uint32_t n, uint16_t type) {
    b[n] = (uint8_t)(type >> 8);
    b[n + 1] = (uint8_t)type;
    b[n + 2] = 0;              // patched by ext_end
    b[n + 3] = 0;
    return n + 4;
}

static void ext_end(uint8_t* b, uint32_t len_at, uint32_t body_start, uint32_t end) {
    tls_store16(b + len_at, end - body_start);
}

// Build the ClientHello body (everything after the 4-byte handshake header).
// Returns 0 if the CSPRNG failed, which aborts the handshake rather than
// sending a guessable key share.
static uint32_t build_client_hello(tls_conn_t* c, uint8_t* b) {
    uint32_t n = 0;
    uint8_t client_random[32];
    if (tls_random(client_random, 32) != 0) return 0;
    if (tls_random(c->session_id, 32) != 0) return 0;
    if (tls_random(c->ks_priv, 32) != 0) return 0;
    tls_x25519_base(c->ks_pub, c->ks_priv);

    b[n++] = 0x03; b[n++] = 0x03;                   // legacy_version = TLS 1.2
    tls_memcpy(b + n, client_random, 32); n += 32;
    b[n++] = 32;                                    // legacy_session_id length
    tls_memcpy(b + n, c->session_id, 32); n += 32;  // (middlebox compat; unused)
    tls_store16(b + n, 4); n += 2;                  // cipher_suites
    b[n++] = (uint8_t)(TLS_AES_128_GCM_SHA256 >> 8);
    b[n++] = (uint8_t)TLS_AES_128_GCM_SHA256;
    b[n++] = (uint8_t)(TLS_CHACHA20_POLY1305_SHA256 >> 8);
    b[n++] = (uint8_t)TLS_CHACHA20_POLY1305_SHA256;
    b[n++] = 1; b[n++] = 0;                         // legacy_compression_methods

    const uint32_t ext_len_at = n;
    n += 2;

    // server_name
    {
        uint32_t body = ext_begin(b, n, EXT_SERVER_NAME); n = body;
        tls_store16(b + n, (uint32_t)str_len(c->host) + 3); n += 2;
        b[n++] = 0;                                  // host_name
        uint32_t hl = str_len(c->host);
        tls_store16(b + n, hl); n += 2;
        tls_memcpy(b + n, c->host, hl); n += hl;
        ext_end(b, body - 2, body, n);
    }
    // supported_groups
    {
        uint32_t body = ext_begin(b, n, EXT_SUPPORTED_GROUPS); n = body;
        tls_store16(b + n, 2); n += 2;
        b[n++] = 0; b[n++] = GROUP_X25519;
        ext_end(b, body - 2, body, n);
    }
    // signature_algorithms
    {
        uint32_t body = ext_begin(b, n, EXT_SIG_ALGS); n = body;
        uint32_t cnt = (uint32_t)(sizeof(SIG_ALGS) / 2);
        tls_store16(b + n, cnt * 2); n += 2;
        for (uint32_t i = 0; i < cnt; i++) {
            b[n++] = (uint8_t)(SIG_ALGS[i] >> 8);
            b[n++] = (uint8_t)SIG_ALGS[i];
        }
        ext_end(b, body - 2, body, n);
    }
    // supported_versions: TLS 1.3 and nothing else, so a 1.2-only server
    // fails here rather than being negotiated down to.
    //
    // The version list is `versions<2..254>` -- a ONE-byte length, unlike
    // every other list in this hello. Writing the two-byte form put 0x00 first,
    // which a server reads as an EMPTY list of versions followed by two stray
    // bytes, and the first real server answered LENGTH_MISMATCH. The offline
    // gate never saw it because nothing there parses a client hello.
    {
        uint32_t body = ext_begin(b, n, EXT_SUPPORTED_VERSIONS); n = body;
        b[n++] = 2;                                  // versions<2..254>
        b[n++] = 0x03; b[n++] = 0x04;                // TLS 1.3
        ext_end(b, body - 2, body, n);
    }
    // key_share with our X25519 public key
    {
        uint32_t body = ext_begin(b, n, EXT_KEY_SHARE); n = body;
        tls_store16(b + n, 36); n += 2;
        b[n++] = 0; b[n++] = GROUP_X25519;
        tls_store16(b + n, 32); n += 2;
        tls_memcpy(b + n, c->ks_pub, 32); n += 32;
        ext_end(b, body - 2, body, n);
    }

    tls_store16(b + ext_len_at, n - ext_len_at - 2);
    return n;
}

static int send_handshake(tls_conn_t* c, int type, const uint8_t* body, uint32_t len) {
    uint8_t msg[TLS_HS_MAX + 4];
    msg[0] = (uint8_t)type;
    tls_store24(msg + 1, len);
    tls_memcpy(msg + 4, body, len);
    // The transcript covers every handshake message, including this one, from
    // the type byte onwards.
    tls_sha256_update(&c->transcript, msg, len + 4);
    int rc = emit_record(c, REC_HANDSHAKE, msg, len + 4);
    if (rc != TLS_OK) return rc;
    return tls_flush(c);
}

static int send_client_hello(tls_conn_t* c) {
    static uint8_t body[512];
    uint32_t len = build_client_hello(c, body);
    if (len == 0) return TLS_ERR_ENTROPY;
    return send_handshake(c, HS_CLIENT_HELLO, body, len);
}

// ------------------------------------------------------------------ ServerHello

// Walk a ServerHello's extensions, returning the key share or failing.
static int parse_server_hello(tls_conn_t* c, const uint8_t* b, uint32_t len) {
    if (len < 38 + 4) return TLS_ERR_PROTOCOL;
    uint32_t n = 0;
    n += 2;                                        // legacy_version
    n += 32;                                       // random
    n += 1 + b[n];                                 // legacy_session_id_echo
    c->suite = rd16(b + n); n += 2;
    if (b[n++] != 0) return TLS_ERR_PROTOCOL;       // legacy_compression_method
    if (n + 2 > len) return TLS_ERR_PROTOCOL;
    uint32_t extlen = rd16(b + n); n += 2;
    if (n + extlen != len) return TLS_ERR_PROTOCOL;

    int have_group = 0, have_version = 0;
    while (n + 4 <= len) {
        uint16_t et = rd16(b + n);
        uint16_t el = rd16(b + n + 2);
        n += 4;
        if (n + el > len) return TLS_ERR_PROTOCOL;
        if (et == EXT_SUPPORTED_VERSIONS) {
            if (el != 2 || rd16(b + n) != 0x0304) return TLS_ERR_UNSUPPORTED;
            have_version = 1;
        } else if (et == EXT_KEY_SHARE) {
            if (el < 4) return TLS_ERR_PROTOCOL;
            uint16_t grp = rd16(b + n);
            uint16_t kl = rd16(b + n + 2);
            if (grp != GROUP_X25519) return TLS_ERR_UNSUPPORTED;
            if (kl != 32 || 4u + kl != el) return TLS_ERR_PROTOCOL;
            tls_memcpy(c->ks_peer, b + n + 4, 32);
            have_group = 1;
        }
        n += el;
    }
    if (!have_version) return TLS_ERR_PROTOCOL;
    if (!have_group) return TLS_ERR_UNSUPPORTED;     // HelloRetryRequest not implemented
    if (c->suite != TLS_AES_128_GCM_SHA256 &&
        c->suite != TLS_CHACHA20_POLY1305_SHA256)
        return TLS_ERR_UNSUPPORTED;
    return TLS_OK;
}

// Derive the handshake secrets and switch both directions to encrypted records.
static int derive_handshake_keys(tls_conn_t* c) {
    uint8_t shared[32], early[32], zero[32];
    tls_memset(zero, 0, 32);
    tls_memset(early, 0, 32);
    tls_x25519(shared, c->ks_priv, c->ks_peer);
    // An all-zero shared secret means the peer echoed a low-order point; the
    // key exchange must not continue with a secret we chose.
    if (tls_const_eq(shared, zero, 32)) return TLS_ERR_PROTOCOL;

    uint8_t derived[32], hs_secret[32];
    tls_hkdf_extract(NULL, 0, zero, 32, early);          // early secret
    derive_empty(derived, 32, early, "derived");
    tls_hkdf_extract(derived, 32, shared, 32, hs_secret);
    // Kept for the next stage: the master secret comes off the Handshake
    // Secret, and nothing else in the struct holds it.
    tls_memcpy(c->handshake_secret, hs_secret, 32);

    uint8_t th[32];
    transcript_hash(c, th);
    derive(c->client_hs_secret, 32, hs_secret, "c hs traffic", th, 32);
    derive(c->server_hs_secret, 32, hs_secret, "s hs traffic", th, 32);
#ifdef TLS_DEBUG_SECRETS
    {   // Development aid: compare against the peer's keylog.
        extern void tls_debug_hex(tls_conn_t*, const char*, const uint8_t*, uint32_t);
        tls_debug_hex(c, "shared   ", shared, 32);
        tls_debug_hex(c, "th       ", th, 32);
        tls_debug_hex(c, "c hs sec ", c->client_hs_secret, 32);
        tls_debug_hex(c, "s hs sec ", c->server_hs_secret, 32);
    }
#endif

    int is_chacha = (c->suite == TLS_CHACHA20_POLY1305_SHA256);
    uint8_t k[32], iv[12];
    install_keys(k, iv, c->client_hs_secret, is_chacha);
    tls_memcpy(c->wr_key, k, is_chacha ? 32u : 16u);
    tls_memcpy(c->wr_iv, iv, 12);
    install_keys(k, iv, c->server_hs_secret, is_chacha);
    tls_memcpy(c->rd_key, k, is_chacha ? 32u : 16u);
    tls_memcpy(c->rd_iv, iv, 12);

    c->enc_wr = 1;
    c->enc_rd = 1;
    c->wr_seq = 0;
    c->rd_seq = 0;
    return TLS_OK;
}

// ------------------------------------------------------------------ Certificate

static int split_certificates(tls_conn_t* c, const uint8_t* b, uint32_t len) {
    if (len < 4) return TLS_ERR_PROTOCOL;
    uint32_t ctxlen = b[0];
    if (1u + ctxlen + 3 > len) return TLS_ERR_PROTOCOL;
    uint32_t n = 1 + ctxlen;
    uint32_t total = (uint32_t)tls_load24(b + n); n += 3;
    if (n + total != len) return TLS_ERR_PROTOCOL;
    if (total > TLS_HS_MAX) return TLS_ERR_OVERFLOW;
    // The wire list interleaves framing with certificates:
    //   CertificateEntry = cert<3..2^24-1> || extensions<0..2^16-1>
    // so the entries are copied out TIGHTLY, one after another, with the
    // prefixes dropped. That is what tls_verify_chain() walks -- it takes a
    // concatenation and advances by lens[i] -- so leaving the 3-byte lengths
    // and 2-byte extension lengths in place made every chain parse as garbage
    // starting at its first byte.
    c->cert_count = 0;
    uint32_t o = 0;                 // read cursor, into the wire list
    uint32_t w = 0;                 // write cursor, into c->certs
    while (o + 3 <= total) {
        uint32_t cl = tls_load24(b + n + o); o += 3;
        if (o + cl > total) return TLS_ERR_PROTOCOL;
        if (c->cert_count >= TLS_CHAIN_MAX) return TLS_ERR_PROTOCOL;
        if (w + cl > TLS_HS_MAX) return TLS_ERR_OVERFLOW;
        tls_memcpy(c->certs + w, b + n + o, cl);
        c->cert_off[c->cert_count] = w;
        c->cert_len[c->cert_count] = cl;
        c->cert_count++;
        w += cl;
        o += cl;
        // The per-certificate extensions belong to the list even though they
        // are never part of a certificate, so their length still has to be
        // consumed here. Skipping them left o two bytes short per entry and the
        // final length check failed on every real chain.
        if (o + 2 > total) return TLS_ERR_PROTOCOL;
        uint32_t el = rd16(b + n + o); o += 2;
        if (o + el > total) return TLS_ERR_PROTOCOL;
        o += el;
    }
    if (o != total) return TLS_ERR_PROTOCOL;
    if (c->cert_count == 0) return TLS_ERR_PROTOCOL;
    return TLS_OK;
}

// The chain rule itself lives in asn1_x509.c so the self-test can drive it with
// a store of its own; this is the engine's call into it.
static int verify_chain(tls_conn_t* c, uint32_t now) {
    uint32_t lens[TLS_CHAIN_MAX];
    for (int i = 0; i < c->cert_count; i++) lens[i] = c->cert_len[i];
    uint32_t na = 0;
    int rc = tls_verify_chain(c->certs, lens, c->cert_count, c->host, now,
                              tls_roots, tls_roots_count, &na);
    if (rc == TLS_OK) {
        c->verify_ok = 1;
        c->server_not_after = na;
    }
    return rc;
}

// ------------------------------------------------------------------ CertificateVerify

static int verify_certificate_verify(tls_conn_t* c, const uint8_t* b, uint32_t len) {
    if (len < 4) return TLS_ERR_PROTOCOL;
    uint16_t alg = rd16(b);
    uint16_t siglen = rd16(b + 2);
    if (4u + siglen != len) return TLS_ERR_PROTOCOL;
    const uint8_t* sig = b + 4;

    // signed_data = 64 spaces || context || 0x00 || Transcript-Hash(Certificate)
    uint8_t signed_data[64 + 34 + 1 + 32];
    tls_memset(signed_data, 0x20, 64);
    static const char CTX[] = "TLS 1.3, server CertificateVerify";
    tls_memcpy(signed_data + 64, CTX, sizeof(CTX) - 1);
    uint32_t n = 64 + (uint32_t)(sizeof(CTX) - 1);
    signed_data[n++] = 0;
    uint8_t th[32];
    transcript_hash(c, th);
    tls_memcpy(signed_data + n, th, 32);
    n += 32;

    uint8_t d256[32], d384[48];
    tls_sha256(signed_data, n, d256);
    tls_sha384(signed_data, n, d384);

    tls_cert_t leaf;
    if (tls_x509_parse(c->certs + c->cert_off[0], c->cert_len[0], &leaf) != TLS_OK)
        return TLS_ERR_PROTOCOL;

    // The signature must be over the hash the algorithm names -- choosing the
    // hash by what happens to verify is the algorithm-confusion bug.
    int rc = TLS_ERR_VERIFY;
    if (alg == 0x0401 || alg == 0x0804 || alg == 0x0809) {
        if (leaf.is_rsa) {
            uint8_t nk[512], ek[8];
            uint32_t nl, el;
            if (tls_spki_rsa(leaf.pub, leaf.pub_len, nk, &nl, ek, &el) != TLS_OK)
                return TLS_ERR_UNSUPPORTED;
            rc = (alg == 0x0401)
               ? (tls_rsa_pkcs1_verify(nk, nl, ek, el, sig, d256, 32) == 0 ? TLS_OK : TLS_ERR_VERIFY)
               : (tls_rsa_pss_verify(nk, nl, ek, el, sig, d256, 32) == 0 ? TLS_OK : TLS_ERR_VERIFY);
        }
    } else if (alg == 0x0501 || alg == 0x0805 || alg == 0x080a) {
        if (leaf.is_rsa) {
            uint8_t nk[512], ek[8];
            uint32_t nl, el;
            if (tls_spki_rsa(leaf.pub, leaf.pub_len, nk, &nl, ek, &el) != TLS_OK)
                return TLS_ERR_UNSUPPORTED;
            rc = (alg == 0x0501)
               ? (tls_rsa_pkcs1_verify(nk, nl, ek, el, sig, d384, 48) == 0 ? TLS_OK : TLS_ERR_VERIFY)
               : (tls_rsa_pss_verify(nk, nl, ek, el, sig, d384, 48) == 0 ? TLS_OK : TLS_ERR_VERIFY);
        }
    } else if (alg == 0x0403 && leaf.ec_curve == 1) {
        uint8_t pt[128];
        uint32_t ptlen;
        int curve;
        if (tls_spki_ec(leaf.pub, leaf.pub_len, pt, &ptlen, &curve) != TLS_OK)
            return TLS_ERR_UNSUPPORTED;
        rc = tls_ecdsa_verify(curve, pt, ptlen, d256, 32, sig, siglen) == 0
             ? TLS_OK : TLS_ERR_VERIFY;
    } else if (alg == 0x0503 && leaf.ec_curve == 2) {
        uint8_t pt[128];
        uint32_t ptlen;
        int curve;
        if (tls_spki_ec(leaf.pub, leaf.pub_len, pt, &ptlen, &curve) != TLS_OK)
            return TLS_ERR_UNSUPPORTED;
        rc = tls_ecdsa_verify(curve, pt, ptlen, d384, 48, sig, siglen) == 0
             ? TLS_OK : TLS_ERR_VERIFY;
    } else {
        return TLS_ERR_UNSUPPORTED;
    }
    return rc;
}

// ------------------------------------------------------------------ app secrets

// `th` is the transcript hash through the SERVER's Finished. It is passed in
// rather than re-read from the running transcript, because by the time this
// runs our own Finished has already been appended to it and folded in.
static int derive_app_keys(tls_conn_t* c, const uint8_t th[32]) {
    uint8_t derived[32], zero[32], master[32];
    tls_memset(zero, 0, 32);
    // From the HANDSHAKE SECRET, not from a handshake traffic secret. Feeding
    // the traffic secret here is an easy slip -- both are 32 bytes and the
    // handshake secret is not otherwise kept around -- and it produces app
    // keys that agree with nothing. The handshake completes either way, because
    // the Finished is checked with the handshake keys, so the first sign of
    // trouble is the peer refusing to decrypt the request that follows.
    derive_empty(derived, 32, c->handshake_secret, "derived");
    tls_hkdf_extract(derived, 32, zero, 32, master);
    derive(c->client_ap_secret, 32, master, "c ap traffic", th, 32);
    derive(c->server_ap_secret, 32, master, "s ap traffic", th, 32);

    int is_chacha = (c->suite == TLS_CHACHA20_POLY1305_SHA256);
    uint8_t k[32], iv[12];
    install_keys(k, iv, c->client_ap_secret, is_chacha);
    tls_memcpy(c->wr_key, k, is_chacha ? 32u : 16u);
    tls_memcpy(c->wr_iv, iv, 12);
    install_keys(k, iv, c->server_ap_secret, is_chacha);
    tls_memcpy(c->rd_key, k, is_chacha ? 32u : 16u);
    tls_memcpy(c->rd_iv, iv, 12);
    c->wr_seq = 0;
    c->rd_seq = 0;
#ifdef TLS_DEBUG_SECRETS
    {   extern void tls_debug_hex(tls_conn_t*, const char*, const uint8_t*, uint32_t);
        tls_debug_hex(c, "c ap sec ", c->client_ap_secret, 32);
        tls_debug_hex(c, "s ap sec ", c->server_ap_secret, 32);
        tls_debug_hex(c, "wr key   ", c->wr_key, is_chacha ? 32u : 16u);
        tls_debug_hex(c, "wr iv    ", c->wr_iv, 12);
    }
#endif
    return TLS_OK;
}

#ifdef TLS_DEBUG_SECRETS
// Hex-dump a secret to the log callback. Built only when TLS_DEBUG_SECRETS is
// defined (see the browser_https_debug Makefile target); it must never be in a
// shipped image, because it writes key material to the serial console.
void tls_debug_hex(tls_conn_t* c, const char* tag, const uint8_t* p, uint32_t n) {
    if (!c->log) return;
    char line[100];
    int k = 0;
    while (tag[k] && k < 12) { line[k] = tag[k]; k++; }
    static const char D[] = "0123456789abcdef";
    for (uint32_t i = 0; i < n && k < 96; i++) {
        line[k++] = D[p[i] >> 4];
        line[k++] = D[p[i] & 15];
    }
    line[k] = 0;
    tls_logmsg(c, line);
}
#endif

// ------------------------------------------------------------------ public API

void tls_init(tls_conn_t* c, const char* hostname) {
    tls_memset(c, 0, sizeof(*c));
    c->state = ST_START;
    uint32_t n = 0;
    if (hostname) {
        while (hostname[n] && n < TLS_HOSTNAME_MAX) { c->host[n] = hostname[n]; n++; }
    }
    c->host[n] = 0;
    tls_sha256_init(&c->transcript);
}

void tls_set_transport(tls_conn_t* c, tls_send_fn send, tls_recv_fn recv,
                       void* io, tls_log_fn log) {
    c->send = send;
    c->recv = recv;
    c->io = io;
    c->log = log;
}

int tls_cipher_id(const tls_conn_t* c) { return c->suite; }

const char* tls_cipher_name(int suite) {
    switch (suite) {
        case TLS_AES_128_GCM_SHA256:       return "TLS_AES_128_GCM_SHA256";
        case TLS_CHACHA20_POLY1305_SHA256: return "TLS_CHACHA20_POLY1305_SHA256";
        default: return "none";
    }
}

// Wall-clock seconds from the RTC, for the certificate validity check.
static uint32_t now_unix(void) {
    rtc_time_t tm;
    tls_memset(&tm, 0, sizeof(tm));
    sys_get_time(&tm);
    // Same civil-from-days conversion asn1_time() uses, spelled out once more
    // because the two live in different layers.
    uint32_t y = tm.year, m = tm.month, d = tm.day;
    if (m < 3) y -= 1;
    uint32_t era = y / 400;
    uint32_t yoe = y - era * 400;
    uint32_t doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    uint32_t days = era * 146097 + doe - 719468;
    return days * 86400u + tm.hour * 3600u + tm.minute * 60u + tm.second;
}

// Handle one complete handshake message. `b`/`len` is the whole message
// including its 4-byte header; the transcript is updated here.
static int handle_handshake(tls_conn_t* c, const uint8_t* b, uint32_t len) {
    int type = b[0];
    uint32_t blen = tls_load24(b + 1);
    if (4u + blen != len) return TLS_ERR_PROTOCOL;
    const uint8_t* body = b + 4;
    int rc;
    tls_lognum(c, "hs ", type);

    switch (type) {
        case HS_SERVER_HELLO: {
            tls_sha256_update(&c->transcript, b, len);
            rc = parse_server_hello(c, body, blen);
            if (rc != TLS_OK) return rc;
            return derive_handshake_keys(c);
        }
        case HS_ENCRYPTED_EXT: {
            tls_sha256_update(&c->transcript, b, len);
            c->state = ST_WAIT_CERT;
            return TLS_OK;
        }
        case HS_CERT_REQUEST:
            // We send no client certificate, so this is expected and skipped --
            // but it still belongs in the transcript.
            tls_sha256_update(&c->transcript, b, len);
            return TLS_OK;
        case HS_CERTIFICATE: {
            tls_sha256_update(&c->transcript, b, len);
            rc = split_certificates(c, body, blen);
            if (rc != TLS_OK) {
                // "the chain does not parse" and "the chain is untrusted" are
                // different verdicts and the browser reports them differently,
                // so the distinction has to survive to the log.
                tls_lognum(c, "certificate framing rc ", rc);
                return rc;
            }
            rc = verify_chain(c, now_unix());
            if (rc != TLS_OK) {
                tls_lognum(c, "chain verdict rc ", rc);
                tls_logmsg(c, "certificate chain rejected");
                return rc;
            }
            c->state = ST_WAIT_CERT_VERIFY;
            return TLS_OK;
        }
        case HS_CERT_VERIFY: {
            // The CertificateVerify signature covers the transcript up to and
            // including Certificate, so it is checked BEFORE it is hashed in.
            rc = verify_certificate_verify(c, body, blen);
            tls_sha256_update(&c->transcript, b, len);
            if (rc != TLS_OK) return rc;
            c->cert_verify_ok = 1;
            c->state = ST_WAIT_SERVER_FINISHED;
            return TLS_OK;
        }
        case HS_FINISHED: {
            if (!c->cert_verify_ok) return TLS_ERR_PROTOCOL;   // Finished first?
            uint8_t th[32], fk[32], want[32];
            transcript_hash(c, th);
            derive(fk, 32, c->server_hs_secret, "finished", NULL, 0);
            tls_hmac_sha256(fk, 32, th, 32, want);
            // Compare before hashing the Finished in: the value we are checking
            // was computed over the transcript as it stood before this message.
            if (!tls_const_eq(want, body, blen) || blen != 32) return TLS_ERR_VERIFY;
            tls_sha256_update(&c->transcript, b, len);
            tls_memcpy(c->peer_finished, body, 32);
            c->state = ST_SEND_FINISHED;
            return TLS_OK;
        }
        case HS_NEW_SESSION_TICKET:
            // Resumption is not implemented, so tickets are ignored -- but they
            // still go into the transcript or the next Finished would not match.
            tls_sha256_update(&c->transcript, b, len);
            return TLS_OK;
        default:
            return TLS_ERR_UNSUPPORTED;
    }
}

static int finish_handshake(tls_conn_t* c) {
    uint8_t th[32], fk[32], verify[32];
    transcript_hash(c, th);              // through the server's Finished
#ifdef TLS_DEBUG_SECRETS
    {   extern void tls_debug_hex(tls_conn_t*, const char*, const uint8_t*, uint32_t);
        tls_debug_hex(c, "th(fin)  ", th, 32);
    }
#endif
    derive(fk, 32, c->client_hs_secret, "finished", NULL, 0);
    tls_memcpy(c->finished_key, fk, 32);
    tls_hmac_sha256(fk, 32, th, 32, verify);
    int rc = send_handshake(c, HS_FINISHED, verify, 32);
    if (rc != TLS_OK && rc != TLS_WANT_WRITE) return rc;
    // The application secrets are derived from the transcript through the
    // SERVER's Finished, and send_handshake() just appended OUR Finished to the
    // running transcript (as it must, for the Finished MAC to be right).
    // Re-reading the transcript here would fold our own Finished in as well and
    // yield a key the peer never computes -- which shows up as the server
    // failing to decrypt the first request we send, long after a handshake that
    // looked entirely successful. Hence the saved hash, and hence this order:
    // the Finished goes out under the handshake key, then the keys switch.
    rc = derive_app_keys(c, th);
    if (rc != TLS_OK) return rc;
    c->done = 1;
    c->state = ST_DONE;
    return tls_flush(c);
}

int tls_step(tls_conn_t* c) {
    if (c->state == ST_FAILED) return c->err;
    if (c->state == ST_DONE) {
        // The handshake is over, and from here tls_step() is a record pump.
        // It has to be one, because tls_read() drives the connection through
        // this same call: returning TLS_OK here without touching the socket
        // left every post-handshake byte -- the response, a NewSessionTicket, a
        // close_notify -- sitting unread in the kernel buffer for the life of
        // the connection, with tls_read() dutifully reporting "nothing yet"
        // every time it was asked. That is a silent hang for every HTTPS fetch,
        // and nothing before it looks wrong.
        if (c->out_off < c->out_len) {
            int rc = tls_flush(c);
            if (rc != TLS_OK) return rc;
        }
    }

    if (c->state == ST_START) {
        int rc = send_client_hello(c);
        if (rc == TLS_WANT_WRITE || rc == TLS_WANT_READ) return rc;
        if (rc != TLS_OK) { c->err = rc; c->state = ST_FAILED; return rc; }
        c->state = ST_WAIT_SERVER_HELLO;
    }
    if (c->state == ST_SEND_FINISHED) {
        int rc = finish_handshake(c);
        if (rc != TLS_OK && rc != TLS_WANT_WRITE) { c->err = rc; c->state = ST_FAILED; }
        return rc;
    }

    // Pull records until the handshake finishes or the transport stalls.
    for (;;) {
        int rc = read_record(c);
        if (rc == TLS_WANT_READ) return TLS_WANT_READ;
        if (rc != TLS_OK) { c->err = rc; c->state = ST_FAILED; return rc; }
        // RFC 8446 5: a middlebox may still inject a PLAINTEXT
        // change_cipher_spec after the ServerHello has switched this side to
        // encrypted records, and it must be ignored. Checking the header here
        // -- before the cipher layer -- is the only place that works: by then
        // open_record() is decrypting everything, and a one-byte record has no
        // room for a tag, so it would fail as a protocol error. An encrypted
        // record can never be mistaken for this (its outer type is
        // application_data and it is at least 17 bytes long).
        if (c->hdr[0] == REC_CHANGE_CIPHER && c->rec_want == 1 && c->rec[0] == 1) {
            continue;
        }
        int type;
        uint8_t* plain;
        uint32_t plen;
        rc = open_record(c, &type, &plain, &plen);
        if (rc != TLS_OK) { c->err = rc; c->state = ST_FAILED; return rc; }

        if (type == REC_CHANGE_CIPHER) continue;       // middlebox noise: ignore
        if (type == REC_ALERT) {
            if (plen >= 2) {
                c->last_alert[0] = plain[1];
                c->last_alert[1] = 0;
                if (plain[1] != 0) {
                    c->msg[0] = (uint8_t)(plain[1] < 10 ? '0' + plain[1] : '?');
                    c->msg[1] = 0;
                    tls_logmsg(c, alert_name(plain[1]));
                    c->err = TLS_ERR_PROTOCOL;
                    c->state = ST_FAILED;
                    return c->err;
                }
            }
            c->recv_closed = 1;
            return TLS_CLOSED;
        }
        if (type == REC_HANDSHAKE) {
            // A record can carry part of a message or several of them.
            uint32_t off = 0;
            while (off + 4 <= plen) {
                uint32_t mlen = tls_load24(plain + off + 1);
                if (mlen + 4 > TLS_HS_MAX) { c->err = TLS_ERR_OVERFLOW; c->state = ST_FAILED; return c->err; }
                if (off + 4 + mlen > plen) break;       // message continues
                rc = handle_handshake(c, plain + off, mlen + 4);
                if (rc != TLS_OK) { c->err = rc; c->state = ST_FAILED; return rc; }
                off += mlen + 4;
                if (c->state == ST_SEND_FINISHED) return TLS_WANT_WRITE;
            }
            continue;
        }
        if (type == REC_APPLICATION) {
            // Application data before the handshake finished, or before we sent
            // our own Finished, is not something TLS 1.3 permits to be trusted.
            if (c->state != ST_DONE) { c->err = TLS_ERR_PROTOCOL; c->state = ST_FAILED; return c->err; }
            uint32_t space = TLS_MAX_PLAIN - c->in_len;
            uint32_t take = plen < space ? plen : space;
            tls_memcpy(c->in + c->in_len, plain, take);
            c->in_len += take;
            continue;
        }
        tls_lognum(c, "unexpected record type ", type);
        c->err = TLS_ERR_PROTOCOL;
        c->state = ST_FAILED;
        return c->err;
    }
}

int tls_handshake(tls_conn_t* c, tls_now_fn now_ms, void* now_ctx, uint32_t timeout_ms) {
    uint32_t start = now_ms ? now_ms(now_ctx) : 0;
    for (uint32_t tries = 0; tries < 2000; tries++) {
        int rc = tls_step(c);
        if (rc == TLS_OK || (rc < 0 && rc != TLS_WANT_READ && rc != TLS_WANT_WRITE))
            return rc;
        if (now_ms && (now_ms(now_ctx) - start) > timeout_ms) return TLS_ERR_IO;
    }
    return TLS_ERR_IO;
}

int tls_write(tls_conn_t* c, const void* buf, uint32_t len) {
    if (!c->done) return TLS_ERR_PROTOCOL;
    if (len > TLS_MAX_PLAIN) len = TLS_MAX_PLAIN;
    int rc = emit_record(c, REC_APPLICATION, (const uint8_t*)buf, len);
    if (rc != TLS_OK) return rc;
    return TLS_OK;
}

int tls_read(tls_conn_t* c, void* buf, uint32_t len) {
    if (c->in_off >= c->in_len) {
        if (c->recv_closed) return TLS_CLOSED;
        // A single tls_step() can pull a record and still end on WANT_READ,
        // because it keeps reading until the transport runs dry. Bailing out on
        // that return value would throw away the plaintext it just buffered, so
        // the buffer -- not the return code -- decides whether to hand data
        // back,
        int rc = tls_step(c);
        // Buffered plaintext wins over the reason the pump stopped. A peer that
        // answers and closes in one burst -- which is what `Connection: close`
        // does, and therefore what most HTTP/1.1 servers do -- hands tls_step()
        // its last record and then EOF in the same call, so returning the error
        // here would throw away a complete, already-decrypted response. Only
        // report the error or the close once there is nothing left to hand
        // back.
        if (c->in_off >= c->in_len) {
            if (rc < 0 && rc != TLS_WANT_READ && rc != TLS_WANT_WRITE) return rc;
            return c->recv_closed ? TLS_CLOSED : 0;
        }
    }
    uint32_t have = c->in_len - c->in_off;
    uint32_t take = have < len ? have : len;
    tls_memcpy(buf, c->in + c->in_off, take);
    c->in_off += take;
    if (c->in_off >= c->in_len) { c->in_off = 0; c->in_len = 0; }
    return (int)take;
}
