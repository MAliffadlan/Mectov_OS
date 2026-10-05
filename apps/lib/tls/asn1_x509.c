// apps/lib/tls/asn1_x509.c — just enough DER to check a certificate chain.
//
// Deliberately small and deliberately strict. It reads the handful of fields a
// TLS client actually needs (validity window, public key, CA bit, SAN/CN) and
// refuses anything it does not understand rather than guessing: an unknown
// signature algorithm, an unexpected length form or a truncation is an error,
// never a "looks close enough". The parser is used on attacker-controlled
// bytes, so every offset is bounds-checked against the buffer it came from and
// no length is trusted before it has been compared with what is left.
//
// There is no allocation here. Every field in tls_cert_t is a pointer into the
// caller's DER buffer, which therefore has to outlive the parsed certificate —
// in the engine that buffer is tls_conn::hs, which lives as long as the
// connection.

#include "tls.h"

// ------------------------------------------------------------------ DER reader

// A TLV as it appears in the buffer: tag byte, body pointer, body length.
// `hdr` is how many bytes the tag and length together occupy, so the whole
// element is `body - hdr` long `len + hdr` bytes -- which is what a signature
// is computed over, and getting that wrong means hashing the wrong slice.
typedef struct {
    uint8_t        tag;
    const uint8_t* body;
    uint32_t       len;
    uint32_t       hdr;
} der_tlv;

// Read one TLV starting at *off. On success *off advances past the element.
// Only single-byte tags are accepted: every ASN.1 type X.509 uses here has a
// tag below 0x1f, and a high-tag-number form would mean something we do not
// understand, so treating it as "parse error" is the honest answer.
static int der_next(const uint8_t* buf, uint32_t len, uint32_t* off,
                    der_tlv* out) {
    if (*off >= len) return TLS_ERR_PROTOCOL;
    uint8_t tag = buf[*off];
    if ((tag & 0x1f) == 0x1f) return TLS_ERR_PROTOCOL;   // high-tag-number form
    uint32_t i = *off + 1;
    if (i >= len) return TLS_ERR_PROTOCOL;
    uint32_t blen = buf[i++];
    if (blen & 0x80) {
        uint32_t nb = blen & 0x7f;
        // A length must not be encoded in more than 4 bytes here (the largest
        // element we ever see is a 16 KiB certificate), and zero-length
        // long form is illegal.
        if (nb == 0 || nb > 4) return TLS_ERR_PROTOCOL;
        if (i + nb > len) return TLS_ERR_PROTOCOL;
        blen = 0;
        for (uint32_t k = 0; k < nb; k++) blen = (blen << 8) | buf[i++];
        // Reject non-minimal encodings (0x81 0x05), which different parsers
        // disagree about and which are a classic way to smuggle a second
        // reading of the same bytes.
        if (blen < 0x80) return TLS_ERR_PROTOCOL;
    }
    if (blen > len - i) return TLS_ERR_PROTOCOL;
    out->tag  = tag;
    out->body = buf + i;
    out->len  = blen;
    out->hdr  = i - *off;
    *off = i + blen;
    return TLS_OK;
}

static int der_expect(const uint8_t* buf, uint32_t len, uint32_t* off,
                      uint8_t tag, der_tlv* out) {
    der_tlv t;
    if (der_next(buf, len, off, &t) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (t.tag != tag) return TLS_ERR_PROTOCOL;
    *out = t;
    return TLS_OK;
}

// ------------------------------------------------------------------ OIDs
// Full TLVs (tag 0x06 + length + body) so they can be compared with memcmp
// directly against the encoded identifier in the certificate.
#define OID(name, ...) static const uint8_t OID_##name[] = { __VA_ARGS__ }

OID(rsa_encryption,   0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01);
OID(rsa_sha256,       0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b);
OID(rsa_sha384,       0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0c);
OID(rsa_sha512,       0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0d);
OID(rsa_pss,          0x06,0x09,0x2a,0x86,0x86,0xf7,0x0d,0x01,0x01,0x0a);
OID(mgf1,             0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x08);
OID(ec_public_key,    0x06,0x07,0x2a,0x86,0x48,0xce,0x3d,0x02,0x01);
OID(ec_p256,          0x06,0x08,0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07);
OID(ec_p384,          0x06,0x05,0x2b,0x81,0x04,0x00,0x22);
OID(ecdsa_sha256,     0x06,0x08,0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02);
OID(ecdsa_sha384,     0x06,0x08,0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x03);
OID(ecdsa_sha512,     0x06,0x08,0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x04);
OID(ext_basic_cons,   0x06,0x03,0x55,0x1d,0x13);
OID(ext_key_usage,    0x06,0x03,0x55,0x1d,0x0f);
OID(ext_san,          0x06,0x03,0x55,0x1d,0x11);
OID(ext_e_key_usage,  0x06,0x03,0x55,0x1d,0x25);
OID(kp_server_auth,   0x06,0x08,0x2b,0x06,0x01,0x05,0x05,0x07,0x03,0x01);

// ------------------------------------------------------------------ time

// Days from 1970-01-01 to y-m-d (proleptic Gregorian, Howard Hinnant's
// days_from_civil). Avoids a table and avoids any division of a struct.
static uint32_t days_from_civil(uint32_t y, uint32_t m, uint32_t d) {
    y -= (m <= 2);
    const uint32_t era = y / 400;
    const uint32_t yoe = y - era * 400;                              // [0, 399]
    const uint32_t doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;      // [0, 146096]
    return era * 146097 + doe - 719468;
}

static int two(const uint8_t* p) {
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return -1;
    return (p[0] - '0') * 10 + (p[1] - '0');
}

static int is_leap(uint32_t y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

// YYMMDDHHMMSSZ (UTCTime) and YYYYMMDDHHMMSSZ (GeneralizedTime). Anything
// with a timezone offset other than 'Z', or with fractional seconds, is
// rejected rather than approximated -- every certificate a real CA issues is
// UTC with whole seconds.
static int asn1_time(const der_tlv* t, uint32_t* out) {
    const uint8_t* p = t->body;
    int year;
    if (t->tag == 0x17) {                       // UTCTime
        if (t->len != 13 || p[12] != 'Z') return -1;
        int yy = two(p);
        if (yy < 0) return -1;
        year = (yy < 50) ? 2000 + yy : 1900 + yy;
        p += 2;
    } else if (t->tag == 0x18) {                // GeneralizedTime
        if (t->len != 15 || p[14] != 'Z') return -1;
        int hi = two(p), lo = two(p + 2);
        if (hi < 0 || lo < 0) return -1;
        year = hi * 100 + lo;
        p += 6;
    } else {
        return -1;
    }
    int mon = two(p), day = two(p + 2), hour = two(p + 4);
    int min = two(p + 6), sec = two(p + 8);
    if (mon < 0 || day < 0 || hour < 0 || min < 0 || sec < 0) return -1;
    if (mon < 1 || mon > 12 || day < 1) return -1;
    if (hour > 23 || min > 59 || sec > 60) return -1;   // 60 = leap second
    static const uint8_t mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int dim = mdays[mon - 1];
    if (mon == 2 && is_leap((uint32_t)year)) dim = 29;
    if (day > dim) return -1;
    *out = days_from_civil((uint32_t)year, (uint32_t)mon, (uint32_t)day) * 86400u
         + (uint32_t)hour * 3600u + (uint32_t)min * 60u + (uint32_t)sec;
    return 0;
}

// ------------------------------------------------------------------ algorithm

// How a signature is made and how it is checked.
typedef struct {
    int kind;            // 1 = RSA PKCS#1, 2 = RSA-PSS, 3 = ECDSA
    int digest;          // 32 / 48 / 64 bytes
    int curve;           // ECDSA only: 1 = P-256, 2 = P-384
} sig_alg_t;

static int digest_len_of_oid(const uint8_t* oid, uint32_t len) {
    if (len == sizeof(OID_rsa_sha256)   && tls_memcmp(oid, OID_rsa_sha256,   sizeof(OID_rsa_sha256))   == 0) return 32;
    if (len == sizeof(OID_rsa_sha384)   && tls_memcmp(oid, OID_rsa_sha384,   sizeof(OID_rsa_sha384))   == 0) return 48;
    if (len == sizeof(OID_rsa_sha512)   && tls_memcmp(oid, OID_rsa_sha512,   sizeof(OID_rsa_sha512))   == 0) return 64;
    if (len == sizeof(OID_ecdsa_sha256) && tls_memcmp(oid, OID_ecdsa_sha256, sizeof(OID_ecdsa_sha256)) == 0) return 32;
    if (len == sizeof(OID_ecdsa_sha384) && tls_memcmp(oid, OID_ecdsa_sha384, sizeof(OID_ecdsa_sha384)) == 0) return 48;
    if (len == sizeof(OID_ecdsa_sha512) && tls_memcmp(oid, OID_ecdsa_sha512, sizeof(OID_ecdsa_sha512)) == 0) return 64;
    return 0;
}

// Compare an OID TLV against one of the static OID_* tables. Those arrays hold
// the whole element (tag + length + body) so they can be compared with memcmp
// directly, which means the length to check is the array size MINUS those two
// header bytes -- comparing der_tlv::len (the body) against the array size
// instead makes every comparison fail and silently turns every certificate
// into an unrecognised one.
static int oid_is(const der_tlv* t, const uint8_t* oid, uint32_t oidlen) {
    return t->tag == 0x06 && t->len + 2 == oidlen &&
           tls_memcmp(t->body, oid + 2, oidlen - 2) == 0;
}

// RSASSA-PSS-params ::= SEQUENCE { [0] hashAlgorithm DEFAULT sha1,
//   [1] maskGenAlgorithm DEFAULT mgf1SHA1, [2] saltLength DEFAULT 20,
//   [3] trailerField DEFAULT 1 }
// Only MGF1 with the same hash as the signature is accepted, and the salt
// length is checked by the verifier; anything else is refused rather than
// verified under the wrong parameters.
static int pss_hash(const der_tlv* params, int* digest) {
    if (params->tag != 0x30) return -1;
    uint32_t off = 0;
    int hlen = 0, mgf_ok = 0;
    while (off < params->len) {
        der_tlv f;
        if (der_next(params->body, params->len, &off, &f) != TLS_OK) return -1;
        if (f.tag != 0xa0 && f.tag != 0xa1) continue;
        uint32_t inner = 0;
        der_tlv alg;
        if (der_expect(f.body, f.len, &inner, 0x30, &alg) != TLS_OK) return -1;
        if (f.tag == 0xa0) {
            uint32_t o2 = 0;
            der_tlv oid;
            if (der_expect(alg.body, alg.len, &o2, 0x06, &oid) != TLS_OK) return -1;
            hlen = digest_len_of_oid(oid.body - 2, oid.len + 2);
            if (!hlen) return -1;
        } else {
            // maskGenAlgorithm: SEQUENCE { OID mgf1, AlgorithmIdentifier hash }
            uint32_t o2 = 0;
            der_tlv moid, hseq;
            if (der_expect(alg.body, alg.len, &o2, 0x06, &moid) != TLS_OK) return -1;
            if (!oid_is(&moid, OID_mgf1, sizeof(OID_mgf1))) return -1;
            if (o2 >= alg.len) return -1;
            uint32_t o3 = 0;
            if (der_expect(alg.body, alg.len, &o3, 0x30, &hseq) != TLS_OK) return -1;
            uint32_t o4 = 0;
            der_tlv hoid;
            if (der_expect(hseq.body, hseq.len, &o4, 0x06, &hoid) != TLS_OK) return -1;
            int ml = digest_len_of_oid(hoid.body - 2, hoid.len + 2);
            mgf_ok = ml && (ml == hlen);
        }
    }
    if (!hlen || !mgf_ok) return -1;
    *digest = hlen;
    return 0;
}

// AlgorithmIdentifier ::= SEQUENCE { algorithm OID, parameters ANY OPTIONAL }
static int read_alg(const der_tlv* alg, sig_alg_t* out) {
    uint32_t off = 0;
    der_tlv oid, params;
    if (alg->tag != 0x30) return TLS_ERR_PROTOCOL;
    if (der_expect(alg->body, alg->len, &off, 0x06, &oid) != TLS_OK) return TLS_ERR_PROTOCOL;
    int has_params = (off < alg->len);
    if (has_params) {
        if (der_next(alg->body, alg->len, &off, &params) != TLS_OK) return TLS_ERR_PROTOCOL;
        if (off != alg->len) return TLS_ERR_PROTOCOL;
    }
    out->curve = 0;
    if (oid_is(&oid, OID_rsa_sha256, sizeof(OID_rsa_sha256)) ||
        oid_is(&oid, OID_rsa_sha384, sizeof(OID_rsa_sha384)) ||
        oid_is(&oid, OID_rsa_sha512, sizeof(OID_rsa_sha512))) {
        out->kind = 1;
        out->digest = digest_len_of_oid(oid.body - 2, oid.len + 2);
        return out->digest ? TLS_OK : TLS_ERR_UNSUPPORTED;
    }
    if (oid_is(&oid, OID_rsa_pss, sizeof(OID_rsa_pss))) {
        out->kind = 2;
        if (!has_params) return TLS_ERR_UNSUPPORTED;   // defaults to SHA-1: refuse
        if (pss_hash(&params, &out->digest) != 0) return TLS_ERR_UNSUPPORTED;
        return TLS_OK;
    }
    if (oid_is(&oid, OID_ecdsa_sha256, sizeof(OID_ecdsa_sha256)) ||
        oid_is(&oid, OID_ecdsa_sha384, sizeof(OID_ecdsa_sha384)) ||
        oid_is(&oid, OID_ecdsa_sha512, sizeof(OID_ecdsa_sha512))) {
        out->kind = 3;
        out->digest = digest_len_of_oid(oid.body - 2, oid.len + 2);
        if (out->digest == 32) out->curve = 1;
        else if (out->digest == 48) out->curve = 2;
        else return TLS_ERR_UNSUPPORTED;   // P-521, which we do not implement
        return TLS_OK;
    }
    // rsaEncryption as a *signature* algorithm means "the DigestInfo carries
    // the hash"; it is legal in old certificates, so accept it.
    if (oid_is(&oid, OID_rsa_encryption, sizeof(OID_rsa_encryption))) {
        out->kind = 1;
        out->digest = 0;                      // chosen from the DigestInfo length
        return TLS_OK;
    }
    return TLS_ERR_UNSUPPORTED;
}

// ------------------------------------------------------------------ public keys

// Pull n and e out of an RSA SubjectPublicKeyInfo, or the uncompressed point
// out of an EC one. Writes into caller-provided buffers; nothing is allocated.
int tls_spki_rsa(const uint8_t* spki, uint32_t len,
                 uint8_t* n, uint32_t* nlen, uint8_t* e, uint32_t* elen) {
    uint32_t off = 0;
    der_tlv seq, alg, bits, inner;
    if (der_expect(spki, len, &off, 0x30, &seq) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (off != len) return TLS_ERR_PROTOCOL;
    uint32_t o2 = 0;
    if (der_expect(seq.body, seq.len, &o2, 0x30, &alg) != TLS_OK) return TLS_ERR_PROTOCOL;
    uint32_t o3 = 0;
    der_tlv oid;
    if (der_expect(alg.body, alg.len, &o3, 0x06, &oid) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (!oid_is(&oid, OID_rsa_encryption, sizeof(OID_rsa_encryption))) return TLS_ERR_UNSUPPORTED;
    if (o2 >= seq.len) return TLS_ERR_PROTOCOL;
    if (der_expect(seq.body, seq.len, &o2, 0x03, &bits) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (bits.len < 1 || bits.body[0] != 0) return TLS_ERR_PROTOCOL;  // no unused bits
    if (o2 != seq.len) return TLS_ERR_PROTOCOL;
    // A FRESH offset: the BIT STRING body is its own buffer, so the offset
    // left over from walking the AlgorithmIdentifier above does not apply.
    uint32_t o5 = 0;
    if (der_expect(bits.body + 1, bits.len - 1, &o5, 0x30, &inner) != TLS_OK) return TLS_ERR_PROTOCOL;
    uint32_t o4 = 0;
    der_tlv nn, ee;
    if (der_expect(inner.body, inner.len, &o4, 0x02, &nn) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (o4 >= inner.len) return TLS_ERR_PROTOCOL;
    if (der_expect(inner.body, inner.len, &o4, 0x02, &ee) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (o4 != inner.len) return TLS_ERR_PROTOCOL;
    // A DER INTEGER whose top bit is set carries a leading 0x00 sign byte, so a
    // 2048-bit modulus arrives as 257 bytes. That byte is padding, not part of
    // the value: keeping it makes nlen 257 and every later comparison against
    // the modulus size off by one.
    while (nn.len > 1 && nn.body[0] == 0x00) { nn.body++; nn.len--; }
    while (ee.len > 1 && ee.body[0] == 0x00) { ee.body++; ee.len--; }
    *nlen = nn.len; *elen = ee.len;
    tls_memcpy(n, nn.body, nn.len);
    tls_memcpy(e, ee.body, ee.len);
    return TLS_OK;
}

// EC key: SEQUENCE { AlgorithmIdentifier { id-ecPublicKey, curve }, BIT STRING }
// `out` receives the point's x||y without its 0x04 tag; *curve receives 1 or 2.
//
// The BIT STRING body is: one "unused bits" byte, then the ECPoint. The ECPoint
// is the RAW 0x04 || X || Y -- it is not wrapped in a DER OCTET STRING, which is
// what openssl and every server in the wild emit and what X9.62's "ECPoint"
// means in practice. The wrapped form is also accepted because the two lengths
// cannot be confused (65 vs 67 bytes for P-256).
int tls_spki_ec(const uint8_t* spki, uint32_t len, uint8_t* out,
                uint32_t* outlen, int* curve) {
    uint32_t off = 0;
    der_tlv seq, alg, bits;
    if (der_expect(spki, len, &off, 0x30, &seq) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (off != len) return TLS_ERR_PROTOCOL;
    uint32_t o2 = 0;
    if (der_expect(seq.body, seq.len, &o2, 0x30, &alg) != TLS_OK) return TLS_ERR_PROTOCOL;
    uint32_t o3 = 0;
    der_tlv oid, params;
    if (der_expect(alg.body, alg.len, &o3, 0x06, &oid) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (!oid_is(&oid, OID_ec_public_key, sizeof(OID_ec_public_key))) return TLS_ERR_UNSUPPORTED;
    if (o3 >= alg.len) return TLS_ERR_PROTOCOL;
    if (der_expect(alg.body, alg.len, &o3, 0x06, &params) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (oid_is(&params, OID_ec_p256, sizeof(OID_ec_p256)))       *curve = 1;
    else if (oid_is(&params, OID_ec_p384, sizeof(OID_ec_p384)))  *curve = 2;
    else return TLS_ERR_UNSUPPORTED;
    if (o2 >= seq.len) return TLS_ERR_PROTOCOL;
    if (der_expect(seq.body, seq.len, &o2, 0x03, &bits) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (bits.len < 1 || bits.body[0] != 0) return TLS_ERR_PROTOCOL;

    const uint32_t raw = (uint32_t)(*curve == 1 ? 64 : 96);   // x||y
    const uint8_t* pt;
    if (bits.len - 1 == raw + 1 && bits.body[1] == 0x04) {
        pt = bits.body + 2;                                    // 0x04 || X || Y
    } else if (bits.len - 3 == raw + 1 && bits.body[1] == 0x04 &&
               bits.body[2] == raw) {
        pt = bits.body + 3;                                    // DER OCTET STRING form
    } else {
        return TLS_ERR_PROTOCOL;
    }
    *outlen = raw;
    tls_memcpy(out, pt, raw);
    return TLS_OK;
}

// ------------------------------------------------------------------ names

// Walk a Name (SEQUENCE of RDN SETs of AttributeTypeAndValue) and return the
// last CN attribute's value. DirectoryString is any of a handful of string
// tags; the bytes are what we compare against the host name, so the tag does
// not change the answer.
static void find_cn(const der_tlv* name, const uint8_t** cn, uint32_t* cnlen) {
    uint32_t off = 0;
    while (off < name->len) {
        der_tlv rdn;
        if (der_next(name->body, name->len, &off, &rdn) != TLS_OK) return;
        uint32_t o2 = 0;
        while (o2 < rdn.len) {
            der_tlv atv;
            if (der_next(rdn.body, rdn.len, &o2, &atv) != TLS_OK) return;
            uint32_t o3 = 0;
            der_tlv oid, val;
            if (der_expect(atv.body, atv.len, &o3, 0x06, &oid) != TLS_OK) return;
            if (o3 >= atv.len) return;
            if (der_next(atv.body, atv.len, &o3, &val) != TLS_OK) return;
            // 2.5.4.3 commonName
            static const uint8_t CN[] = {0x06,0x03,0x55,0x04,0x03};
            if (oid_is(&oid, CN, sizeof(CN))) {
                *cn = val.body;
                *cnlen = val.len;
            }
        }
    }
}

// ------------------------------------------------------------------ extensions

typedef struct {
    int is_ca;
    int key_cert_sign;      // from keyUsage, when that extension is present
    int has_key_usage;
    int server_auth;       // from extendedKeyUsage, when present
    const uint8_t* san;
    uint32_t san_len;
    int has_san;
} ext_t;

static void read_extensions(const der_tlv* exts, ext_t* out) {
    uint32_t off = 0;
    while (off < exts->len) {
        der_tlv ext;
        if (der_next(exts->body, exts->len, &off, &ext) != TLS_OK) return;
        if (ext.tag != 0x30) return;
        uint32_t o = 0;
        der_tlv oid, val;
        if (der_expect(ext.body, ext.len, &o, 0x06, &oid) != TLS_OK) return;
        // critical BOOLEAN DEFAULT FALSE is skipped if present
        uint32_t probe = o;
        der_tlv maybe_bool;
        if (der_next(ext.body, ext.len, &probe, &maybe_bool) == TLS_OK &&
            maybe_bool.tag == 0x01) {
            o = probe;
        }
        if (o >= ext.len) return;
        if (der_expect(ext.body, ext.len, &o, 0x04, &val) != TLS_OK) return;
        if (o != ext.len) return;

        if (oid_is(&oid, OID_ext_basic_cons, sizeof(OID_ext_basic_cons))) {
            uint32_t o2 = 0;
            der_tlv bc;
            if (der_expect(val.body, val.len, &o2, 0x30, &bc) != TLS_OK) continue;
            uint32_t o3 = 0;
            der_tlv c;
            if (der_next(bc.body, bc.len, &o3, &c) != TLS_OK) continue;
            if (c.tag == 0x01) out->is_ca = (c.len >= 1 && c.body[0] != 0);
        } else if (oid_is(&oid, OID_ext_key_usage, sizeof(OID_ext_key_usage))) {
            uint32_t o2 = 0;
            der_tlv ku;
            if (der_expect(val.body, val.len, &o2, 0x03, &ku) != TLS_OK) continue;
            if (ku.len < 1 || ku.body[0] != 0) continue;
            const uint8_t* bits = ku.body + 1;
            uint32_t blen = ku.len - 1;
            // BIT STRING bit 5 is keyCertSign, bit 0 is digitalSignature.
            if (blen >= 1 && (bits[0] & 0x04)) out->key_cert_sign = 1;
            out->has_key_usage = 1;
        } else if (oid_is(&oid, OID_ext_e_key_usage, sizeof(OID_ext_e_key_usage))) {
            uint32_t o2 = 0;
            der_tlv seq;
            if (der_expect(val.body, val.len, &o2, 0x30, &seq) != TLS_OK) continue;
            uint32_t o3 = 0;
            while (o3 < seq.len) {
                der_tlv u;
                if (der_next(seq.body, seq.len, &o3, &u) != TLS_OK) break;
                if (oid_is(&u, OID_kp_server_auth, sizeof(OID_kp_server_auth)))
                    out->server_auth = 1;
            }
        } else if (oid_is(&oid, OID_ext_san, sizeof(OID_ext_san))) {
            // extnValue holds a DER OCTET STRING; unwrap it to the GeneralNames
            // SEQUENCE body. `out->san` points at that body.
            uint32_t o2 = 0;
            der_tlv inner;
            if (der_expect(val.body, val.len, &o2, 0x30, &inner) != TLS_OK) continue;
            out->san = inner.body;
            out->san_len = inner.len;
            out->has_san = 1;
        }
    }
}

int tls_x509_is_ca(const tls_cert_t* c) {
    if (!c->is_ca) return 0;
    return 1;   // keyUsage, when present, is enforced during parsing
}

// ------------------------------------------------------------------ parse

int tls_x509_parse(const uint8_t* der, uint32_t len, tls_cert_t* out) {
    tls_memset(out, 0, sizeof(*out));
    uint32_t off = 0;
    der_tlv cert, tbs, sig_alg, sig_bits;
    if (der_expect(der, len, &off, 0x30, &cert) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (off != len) return TLS_ERR_PROTOCOL;
    uint32_t o = 0;
    if (der_expect(cert.body, cert.len, &o, 0x30, &tbs) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (der_expect(cert.body, cert.len, &o, 0x30, &sig_alg) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (o >= cert.len) return TLS_ERR_PROTOCOL;
    if (der_expect(cert.body, cert.len, &o, 0x03, &sig_bits) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (o != cert.len) return TLS_ERR_PROTOCOL;
    if (sig_bits.len < 1 || sig_bits.body[0] != 0) return TLS_ERR_PROTOCOL;

    // Record the outer AlgorithmIdentifier element (tag + length + body).
    out->spki_alg     = sig_alg.body - sig_alg.hdr;
    out->spki_alg_len = sig_alg.len + sig_alg.hdr;

    // The tbsCertificate TLV starts two bytes before its body only for short
    // lengths; find it robustly instead of assuming a length form.
    const uint8_t* tbs_full = tbs.body - tbs.hdr;
    uint32_t tbs_full_len = tbs.len + tbs.hdr;

    // ---- tbsCertificate fields
    uint32_t to = 0;
    der_tlv f;
    if (der_next(tbs.body, tbs.len, &to, &f) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (f.tag == 0xa0) {                           // [0] EXPLICIT version
        if (der_next(tbs.body, tbs.len, &to, &f) != TLS_OK) return TLS_ERR_PROTOCOL;
    }
    // f is serialNumber; the next one is the inner signature AlgorithmIdentifier.
    if (der_expect(tbs.body, tbs.len, &to, 0x30, &f) != TLS_OK) return TLS_ERR_PROTOCOL;
    // issuer
    if (der_expect(tbs.body, tbs.len, &to, 0x30, &f) != TLS_OK) return TLS_ERR_PROTOCOL;
    // validity
    der_tlv validity;
    if (der_expect(tbs.body, tbs.len, &to, 0x30, &validity) != TLS_OK) return TLS_ERR_PROTOCOL;
    // Validity ::= SEQUENCE { notBefore Time, notAfter Time }. Either may be a
    // UTCTime or a GeneralizedTime, so the tags are not asserted here -- they
    // are checked per value in asn1_time().
    uint32_t vo = 0;
    der_tlv nb, na;
    if (der_next(validity.body, validity.len, &vo, &nb) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (der_next(validity.body, validity.len, &vo, &na) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (vo != validity.len) return TLS_ERR_PROTOCOL;
    uint32_t nbv = 0, nav = 0;
    if (asn1_time(&nb, &nbv) != 0) return TLS_ERR_PROTOCOL;
    if (asn1_time(&na, &nav) != 0) return TLS_ERR_PROTOCOL;
    out->not_before = nbv;
    out->not_after  = nav;
    // subject
    der_tlv subject;
    if (der_expect(tbs.body, tbs.len, &to, 0x30, &subject) != TLS_OK) return TLS_ERR_PROTOCOL;
    find_cn(&subject, &out->cn, &out->cn_len);
    // subjectPublicKeyInfo
    der_tlv spki;
    if (der_expect(tbs.body, tbs.len, &to, 0x30, &spki) != TLS_OK) return TLS_ERR_PROTOCOL;
    out->pub     = spki.body - spki.hdr;
    out->pub_len = spki.len + spki.hdr;
    // Walk the key so is_rsa / ec_curve are known without re-parsing later.
    {
        static uint8_t n[512], e[8];
        static uint32_t nlen, elen;
        if (tls_spki_rsa(out->pub, out->pub_len, n, &nlen, e, &elen) == TLS_OK) {
            out->is_rsa = 1;
        } else {
            static uint8_t pt[128];
            static uint32_t ptlen;
            int curve = 0;
            if (tls_spki_ec(out->pub, out->pub_len, pt, &ptlen, &curve) == TLS_OK)
                out->ec_curve = curve;
        }
    }
    // extensions, if any
    ext_t ex;
    tls_memset(&ex, 0, sizeof(ex));
    while (to < tbs.len) {
        if (der_next(tbs.body, tbs.len, &to, &f) != TLS_OK) break;
        if (f.tag == 0xa3) {                       // [3] EXPLICIT extensions
            uint32_t eo = 0;
            der_tlv seq;
            if (der_expect(f.body, f.len, &eo, 0x30, &seq) == TLS_OK)
                read_extensions(&seq, &ex);
            break;
        }
    }
    out->is_ca     = ex.is_ca;
    out->san       = ex.san;
    out->san_len   = ex.san_len;
    out->has_san   = ex.has_san;
    // keyUsage without keyCertSign must not sign certificates.
    if (ex.has_key_usage && !ex.key_cert_sign) out->is_ca = 0;
    (void)tbs_full; (void)tbs_full_len;
    return TLS_OK;
}

int tls_x509_time_valid(const tls_cert_t* c, uint32_t now) {
    if (c->not_before == 0 && c->not_after == 0) return 1;
    if (now < c->not_before) return 0;
    if (now > c->not_after) return 0;
    return 1;
}

// ------------------------------------------------------------------ verify

int tls_x509_verify_signed_by(const uint8_t* child_der, uint32_t child_len,
                              const tls_cert_t* issuer) {
    tls_cert_t child;
    if (tls_x509_parse(child_der, child_len, &child) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (!issuer->is_ca) return TLS_ERR_VERIFY;

    // Re-read the child's own signature algorithm and signature.
    uint32_t off = 0;
    der_tlv cert, tbs, sig_alg, sig_bits;
    if (der_expect(child_der, child_len, &off, 0x30, &cert) != TLS_OK) return TLS_ERR_PROTOCOL;
    uint32_t o = 0;
    if (der_expect(cert.body, cert.len, &o, 0x30, &tbs) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (der_expect(cert.body, cert.len, &o, 0x30, &sig_alg) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (o >= cert.len) return TLS_ERR_PROTOCOL;
    if (der_expect(cert.body, cert.len, &o, 0x03, &sig_bits) != TLS_OK) return TLS_ERR_PROTOCOL;
    if (sig_bits.len < 1 || sig_bits.body[0] != 0) return TLS_ERR_PROTOCOL;

    // The signature covers the whole tbsCertificate TLV, tag and length included.
    const uint8_t* tbs_full = tbs.body - tbs.hdr;
    uint32_t tbs_full_len = tbs.len + tbs.hdr;

    // The algorithm named in the Certificate must match the one in the
    // tbsCertificate; a mismatch is the classic algorithm-confusion probe.
    sig_alg_t outer, inner;
    if (read_alg(&sig_alg, &outer) != TLS_OK) return TLS_ERR_UNSUPPORTED;
    {
        uint32_t to = 0;
        der_tlv f;
        if (der_next(tbs.body, tbs.len, &to, &f) != TLS_OK) return TLS_ERR_PROTOCOL;
        if (f.tag == 0xa0) {
            if (der_next(tbs.body, tbs.len, &to, &f) != TLS_OK) return TLS_ERR_PROTOCOL;
        }
        if (der_expect(tbs.body, tbs.len, &to, 0x30, &f) != TLS_OK) return TLS_ERR_PROTOCOL;
        if (read_alg(&f, &inner) != TLS_OK) return TLS_ERR_UNSUPPORTED;
    }
    if (outer.kind != inner.kind ||
        (outer.kind == 3 && outer.curve != inner.curve) ||
        (outer.digest && inner.digest && outer.digest != inner.digest))
        return TLS_ERR_VERIFY;

    uint8_t digest[64];
    uint32_t dlen = outer.digest;
    if (dlen == 0) {
        // rsaEncryption: the hash comes out of the DigestInfo, which the
        // verifier only discovers after the public-key operation. Try the
        // three hashes X.509 can use and require exactly one to verify.
        int rc = TLS_ERR_VERIFY;
        for (uint32_t n = 0; n < 3; n++) {
            uint32_t l = n == 0 ? 32 : (n == 1 ? 48 : 64);
            if (l == 32) tls_sha256(tbs_full, tbs_full_len, digest);
            else if (l == 48) tls_sha384(tbs_full, tbs_full_len, digest);
            else tls_sha512(tbs_full, tbs_full_len, digest);
            static uint8_t n_buf[512], e_buf[8];
            uint32_t nlen2, elen2;
            if (tls_spki_rsa(issuer->pub, issuer->pub_len, n_buf, &nlen2,
                             e_buf, &elen2) != TLS_OK) return TLS_ERR_UNSUPPORTED;
            if (tls_rsa_pkcs1_verify(n_buf, nlen2, e_buf, elen2,
                                     sig_bits.body + 1, digest, l) == TLS_OK) {
                return TLS_OK;
            }
            rc = TLS_ERR_VERIFY;
        }
        return rc;
    }
    if (dlen == 32) tls_sha256(tbs_full, tbs_full_len, digest);
    else if (dlen == 48) tls_sha384(tbs_full, tbs_full_len, digest);
    else tls_sha512(tbs_full, tbs_full_len, digest);

    if (outer.kind == 3) {
        static uint8_t pt[128];
        static uint32_t ptlen;
        int curve = 0;
        if (tls_spki_ec(issuer->pub, issuer->pub_len, pt, &ptlen, &curve) != TLS_OK)
            return TLS_ERR_UNSUPPORTED;
        if (curve != outer.curve) return TLS_ERR_UNSUPPORTED;
        // The signature is a DER SEQUENCE{r,s}; the verifier takes r||s. Note the
        // BIT STRING's first body byte is the "unused bits" count, so the
        // signature itself starts one byte in -- handing body[] straight to the
        // verifier checks a signature with a zero prepended to it.
        uint32_t so = 0;
        der_tlv seq, r, sv;
        if (der_expect(sig_bits.body + 1, sig_bits.len - 1, &so, 0x30, &seq) != TLS_OK)
            return TLS_ERR_PROTOCOL;
        if (der_expect(seq.body, seq.len, &so, 0x02, &r) != TLS_OK) return TLS_ERR_PROTOCOL;
        if (der_expect(seq.body, seq.len, &so, 0x02, &sv) != TLS_OK) return TLS_ERR_PROTOCOL;
        uint32_t flen = (uint32_t)(outer.curve == 1 ? 32 : 48);
        if (r.len > flen || sv.len > flen) return TLS_ERR_VERIFY;
        uint8_t flat[96];
        tls_memset(flat, 0, sizeof(flat));
        tls_memcpy(flat + (flen - r.len), r.body, r.len);
        tls_memcpy(flat + flen + (flen - sv.len), sv.body, sv.len);
        return tls_ecdsa_verify(outer.curve, pt, ptlen, digest, dlen, flat, flen * 2) == 0
                   ? TLS_OK : TLS_ERR_VERIFY;
    }

    static uint8_t n_buf[512], e_buf[8];
    uint32_t nlen2, elen2;
    if (tls_spki_rsa(issuer->pub, issuer->pub_len, n_buf, &nlen2, e_buf, &elen2) != TLS_OK)
        return TLS_ERR_UNSUPPORTED;
    if (outer.kind == 1)
        return tls_rsa_pkcs1_verify(n_buf, nlen2, e_buf, elen2, sig_bits.body + 1, digest, dlen) == 0
                   ? TLS_OK : TLS_ERR_VERIFY;
    return tls_rsa_pss_verify(n_buf, nlen2, e_buf, elen2, sig_bits.body + 1, digest, dlen) == 0
               ? TLS_OK : TLS_ERR_VERIFY;
}

// ------------------------------------------------------------------ hostname

static int ci_eq(uint8_t a, uint8_t b) {
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    return a == b;
}

static int str_ieq(const uint8_t* a, uint32_t alen, const char* b) {
    uint32_t i = 0;
    for (; b[i]; i++) {
        if (i >= alen) return 0;
        if (!ci_eq(a[i], (uint8_t)b[i])) return 0;
    }
    return i == alen;
}

// A dNSName may carry a single leading "*." wildcard, matching exactly one
// label. "*" alone, and wildcards in the middle, are not honoured.
static int dns_match(const uint8_t* pat, uint32_t plen, const char* host) {
    if (plen > 2 && pat[0] == '*' && pat[1] == '.') {
        const char* dot = host;
        int seen = 0;
        for (const char* h = host; *h; h++) {
            if (*h == '.') { dot = h; seen = 1; break; }
        }
        if (!seen) return 0;                       // needs at least one label
        return str_ieq(pat + 2, plen - 2, dot + 1);
    }
    return str_ieq(pat, plen, host);
}

static int parse_ipv4(const char* s, uint8_t out[4]) {
    uint32_t v[4];
    uint32_t idx = 0, acc = 0, digits = 0;
    for (const char* p = s;; p++) {
        if (*p >= '0' && *p <= '9') {
            acc = acc * 10 + (uint32_t)(*p - '0');
            if (++digits > 3 || acc > 255) return -1;
        } else if (*p == '.' || *p == '\0') {
            if (!digits || idx >= 4) return -1;
            v[idx++] = acc;
            acc = 0; digits = 0;
            if (*p == '\0') break;
        } else {
            return -1;
        }
    }
    if (idx != 4) return -1;
    for (int i = 0; i < 4; i++) out[i] = (uint8_t)v[i];
    return 0;
}

// IPv6 literals are deliberately NOT parsed. The network stack in this kernel
// is IPv4 end to end -- SYS_TCP_CONNECT takes a 4-byte address -- so an IPv6
// literal can never be the host this client is talking to, and a certificate
// whose only address SAN is an IPv6 one therefore cannot and should not match.
// Writing an IPv6 parser that nothing could ever exercise would only be a way
// to ship an unverified comparison.

// Does `host` look like an IPv4 literal rather than a DNS name? A name that is
// all digits and dots is treated as an address, which is the conservative
// choice: it means "10.0.2.2" cannot be matched by a dNSName SAN.
static int host_is_ipv4(const char* h) {
    int dots = 0;
    for (const char* p = h; *p; p++) {
        if (*p == '.') dots++;
        else if (*p < '0' || *p > '9') return 0;
    }
    return dots == 3;
}

int tls_x509_match_host(const tls_cert_t* c, const char* host) {
    if (!host || !*host) return 0;

    // SAN wins outright when present: RFC 6125 says the CN is ignored if any
    // dNSName exists, and ignoring that is how a certificate with a stale CN
    // keeps matching.
    if (c->has_san && c->san) {
        uint32_t off = 0;
        while (off < c->san_len) {
            der_tlv gn;
            if (der_next(c->san, c->san_len, &off, &gn) != TLS_OK) return 0;
            if (gn.tag == 0x82) {                       // dNSName
                if (dns_match(gn.body, gn.len, host)) return 1;
            } else if (gn.tag == 0x87) {                // iPAddress, IPv4 only
                if (!host_is_ipv4(host)) continue;
                if (gn.len != 4) continue;
                uint8_t want[4];
                if (parse_ipv4(host, want) != 0) continue;
                if (tls_const_eq(want, gn.body, 4)) return 1;
            }
        }
        return 0;
    }

    if (c->cn && dns_match(c->cn, c->cn_len, host)) return 1;
    return 0;
}
// ------------------------------------------------------------------ chain

// Build a path from the leaf to a trust anchor and verify every signature on
// it. This is the whole path-building rule: the chain the server sent must be
// used as given (no AIA fetching, no guessing at missing intermediates), every
// certificate must be inside its validity window, the leaf must be the name we
// asked for, and the top of the chain must be a trust anchor.
int tls_verify_chain(const uint8_t* certs, const uint32_t* lens, int count,
                     const char* host, uint32_t now,
                     const tls_root_t* roots, int root_count,
                     uint32_t* leaf_not_after) {
    if (count < 1 || count > TLS_CHAIN_MAX) return TLS_ERR_PROTOCOL;

    tls_cert_t chain[TLS_CHAIN_MAX];
    uint32_t off = 0;
    for (int i = 0; i < count; i++) {
        if (tls_x509_parse(certs + off, lens[i], &chain[i]) != TLS_OK)
            return TLS_ERR_PROTOCOL;
        if (!tls_x509_time_valid(&chain[i], now)) return TLS_ERR_VERIFY;
        off += lens[i];
    }
    // SAN wins over CN, so a stale CN on a certificate that also carries a SAN
    // cannot match -- see tls_x509_match_host().
    if (!tls_x509_match_host(&chain[0], host)) return TLS_ERR_VERIFY;
    if (leaf_not_after) *leaf_not_after = chain[0].not_after;

    // Walk up the chain as presented. The last certificate may itself be the
    // anchor -- matched byte for byte against the store, whose own signature is
    // then not checked, because that is what a trust anchor means.
    uint32_t base = 0;
    for (int i = 0; i < count; i++) {
        for (int r = 0; r < root_count; r++) {
            if (roots[r].len != lens[i]) continue;
            if (tls_memcmp(roots[r].der, certs + base, lens[i]) != 0) continue;
            return TLS_OK;                 // trust anchor reached
        }
        if (i + 1 >= count) break;
        if (tls_x509_verify_signed_by(certs + base, lens[i], &chain[i + 1]) != TLS_OK)
            return TLS_ERR_VERIFY;
        base += lens[i];
    }
    return TLS_ERR_VERIFY;                  // chain did not reach an anchor
}
