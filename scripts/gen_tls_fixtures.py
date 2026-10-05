#!/usr/bin/env python3
"""Generate the fixed crypto/X.509 fixtures used by apps/tlsselftest.c.

The keys are generated once and the resulting header is committed, so the
self-test runs inside the guest with no network and no openssl. Re-run this
only when a fixture must actually change -- regenerating it produces different
keys, which invalidates nothing but makes a noisy diff.

Output is a C header with byte arrays:

  tlsvec_msg / TLSVEC_DIGEST_SHA256   the message and its SHA-256
  RSA_N / RSA_E / RSA_SIG             RSA-2048 PKCS#1 v1.5 signature fixture
  EC_P256_PUB / EC_P256_SIG           ECDSA P-256 signature (r||s)
  EC_P384_PUB / EC_P384_SIG           ECDSA P-384 signature (r||s)
  CA_DER                              self-signed RSA test CA
  OTHER_CA_DER                        a second CA, which must NOT validate
  LEAF_RSA_DER                        leaf signed by the CA (RSA, PKCS#1)
  LEAF_EC_DER                         leaf signed by the CA (ECDSA P-256)

Certificates are emitted as DER, not PEM. The name says so; the first version
of this script copied the .pem files verbatim, and a DER parser fed a PEM file
fails on the very first byte -- which looks like a parser bug for an hour.

Usage:  scripts/gen_tls_fixtures.py [--out apps/lib/tls/test_vectors.h]
"""
import argparse
import base64
import hashlib
import os
import subprocess
import tempfile


def run(*args, **kw):
    return subprocess.run(args, check=True, capture_output=True, **kw)


def unhex(s):
    s = "".join(s.split())
    if len(s) % 2:
        s = "0" + s
    return bytes.fromhex(s)


def cert_der(pem_path):
    """A PEM file -> its DER bytes. This is what tls_x509_parse() wants."""
    return run("openssl", "x509", "-in", pem_path, "-outform", "DER").stdout


def rsa_parts(pem):
    """(n, e) for an RSA private key. e is the 65537 this script generates with."""
    out = run("openssl", "rsa", "-in", pem, "-noout", "-modulus").stdout.decode().strip()
    return unhex(out.split("=", 1)[1]), unhex("010001")


def der_read_tlv(buf, i):
    tag = buf[i]
    ln = buf[i + 1]
    i += 2
    if ln & 0x80:
        n = ln & 0x7f
        ln = int.from_bytes(buf[i:i + n], "big")
        i += n
    return tag, ln, i


def ec_pub(pem):
    """SubjectPublicKeyInfo DER -> the uncompressed point x||y, without the 0x04 tag."""
    der = run("openssl", "ec", "-in", pem, "-pubout", "-outform", "DER").stdout
    _, _, i = der_read_tlv(der, 0)          # outer SEQUENCE -> content
    _, alg_len, i = der_read_tlv(der, i)     # AlgorithmIdentifier
    i += alg_len                            # skip its content
    _, bs_len, i = der_read_tlv(der, i)     # subjectPublicKey BIT STRING
    assert der[i] == 0x00, "expected 0 unused bits"
    assert der[i + 1] == 0x04, "expected an uncompressed point"
    return der[i + 2:i + bs_len]


def der_sig_to_fixed(sig, size):
    """DER SEQUENCE{r,s} -> r||s, each left-padded to the field size."""
    tag, _, i = der_read_tlv(sig, 0)
    assert tag == 0x30
    vals = []
    for _ in range(2):
        tag, ln, i = der_read_tlv(sig, i)
        assert tag == 0x02
        vals.append(sig[i:i + ln].lstrip(b"\x00").rjust(size, b"\x00"))
        i += ln
    return vals[0] + vals[1]


def write_ext(tmp, name, text):
    p = os.path.join(tmp, name)
    with open(p, "w") as f:
        f.write(text)
    return p


def leaf_ext(tmp, name):
    """The extension block shared by both leaves. `basicConstraints=CA:FALSE` is
    explicit so the self-test can prove a leaf is refused as an issuer."""
    return write_ext(tmp, name,
                     "subjectAltName=DNS:localhost,IP:10.0.2.2\n"
                     "basicConstraints=CA:FALSE\n"
                     "extendedKeyUsage=serverAuth\n")


def arr(name, data):
    body = ", ".join("0x%02x" % b for b in data)
    return "static const uint8_t %s[%d] = {\n    %s\n};\n" % (name, len(data), body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="apps/lib/tls/test_vectors.h")
    args = ap.parse_args()

    message = b"mectov tls selftest message"
    digest = hashlib.sha256(message).digest()

    tmp = tempfile.mkdtemp(prefix="tlsfix")
    ca_key = os.path.join(tmp, "ca.key")
    ca_crt = os.path.join(tmp, "ca.crt")
    other_key = os.path.join(tmp, "other.key")
    other_crt = os.path.join(tmp, "other.crt")
    leaf_key = os.path.join(tmp, "leaf.key")
    leaf_crt = os.path.join(tmp, "leaf.crt")
    leaf_ec_key = os.path.join(tmp, "leaf_ec.key")
    leaf_ec_crt = os.path.join(tmp, "leaf_ec.crt")
    p384_key = os.path.join(tmp, "p384.key")
    msg = os.path.join(tmp, "msg.bin")

    with open(msg, "wb") as f:
        f.write(message)

    # --- test CAs: RSA, CA:TRUE, keyCertSign so they may sign leaves
    for key, crt, cn in ((ca_key, ca_crt, "Mectov TLS Test CA"),
                         (other_key, other_crt, "Mectov TLS Other CA")):
        run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-keyout", key,
            "-out", crt, "-days", "3650", "-nodes", "-subj", f"/CN={cn}",
            "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign")

    # --- RSA leaf signed by the CA (SAN localhost + IP 10.0.2.2)
    run("openssl", "req", "-newkey", "rsa:2048", "-keyout", leaf_key,
        "-out", os.path.join(tmp, "leaf.csr"), "-nodes", "-subj", "/CN=localhost")
    run("openssl", "x509", "-req", "-in", os.path.join(tmp, "leaf.csr"),
        "-CA", ca_crt, "-CAkey", ca_key, "-CAcreateserial", "-out", leaf_crt,
        "-days", "3650", "-sha256",
        "-extfile", leaf_ext(tmp, "leafext.cnf"))

    # --- ECDSA P-256 leaf signed by the same CA
    run("openssl", "ecparam", "-name", "prime256v1", "-genkey",
        "-noout", "-out", leaf_ec_key)
    run("openssl", "req", "-new", "-key", leaf_ec_key,
        "-out", os.path.join(tmp, "leaf_ec.csr"), "-subj", "/CN=localhost")
    run("openssl", "x509", "-req", "-in", os.path.join(tmp, "leaf_ec.csr"),
        "-CA", ca_crt, "-CAkey", ca_key, "-CAcreateserial", "-out", leaf_ec_crt,
        "-days", "3650", "-sha256",
        "-extfile", leaf_ext(tmp, "leafec.ext.cnf"))

    # --- signature fixtures over `message`
    sig_rsa = run("openssl", "dgst", "-sha256", "-sign", ca_key, msg).stdout
    n, e = rsa_parts(ca_key)
    sig_ec = run("openssl", "dgst", "-sha256", "-sign", leaf_ec_key, msg).stdout
    ec_pub_p256 = ec_pub(leaf_ec_key)
    sig_ec_fixed = der_sig_to_fixed(sig_ec, 32)

    run("openssl", "ecparam", "-name", "secp384r1", "-genkey",
        "-noout", "-out", p384_key)
    sig384 = run("openssl", "dgst", "-sha384", "-sign", p384_key, msg).stdout
    sig384_raw = der_sig_to_fixed(sig384, 48)
    pub384 = ec_pub(p384_key)

    out = []
    out.append("// apps/lib/tls/test_vectors.h - GENERATED by scripts/gen_tls_fixtures.py\n")
    out.append("// Do not edit by hand. Fixed signature and X.509 fixtures for\n")
    out.append("// apps/tlsselftest.c: a self-contained gate for the primitives the\n")
    out.append("// TLS client's certificate verification depends on. No network and no\n")
    out.append("// openssl at test time.\n")
    out.append("//\n")
    out.append("// The message behind every digest is: \"%s\"\n" % message.decode())
    out.append("#ifndef MCT_TLS_TEST_VECTORS_H\n#define MCT_TLS_TEST_VECTORS_H\n\n")
    out.append("#include \"src/include/types.h\"\n\n")
    out.append("#define TLSVEC_MSG_SIZE %d\n" % len(message))
    out.append(arr("tlsvec_msg", message))
    out.append(arr("TLSVEC_DIGEST_SHA256", digest))
    out.append(arr("RSA_N", n))
    out.append(arr("RSA_E", e))
    out.append(arr("RSA_SIG", sig_rsa.rjust(len(n), b"\x00")))
    out.append(arr("EC_P256_PUB", ec_pub_p256))
    out.append(arr("EC_P256_SIG", sig_ec_fixed))
    out.append(arr("EC_P384_PUB", pub384))
    out.append(arr("EC_P384_SIG", sig384_raw))
    out.append(arr("CA_DER", cert_der(ca_crt)))
    out.append(arr("OTHER_CA_DER", cert_der(other_crt)))
    out.append(arr("LEAF_RSA_DER", cert_der(leaf_crt)))
    out.append(arr("LEAF_EC_DER", cert_der(leaf_ec_crt)))
    out.append("\n#define TEST_CA_LEN %d\n#define TEST_OTHER_CA_LEN %d\n"
               % (len(cert_der(ca_crt)), len(cert_der(other_crt))))

    # A trust store for the self-test: the CA that actually issued the leaves,
    # and the one that must NOT validate. The real engine uses tls_roots.h;
    # this exists so the gate can prove both the accept and the reject path
    # without needing a live server.
    out.append("\nstatic const uint8_t tls_test_root[TEST_CA_LEN] = {\n    ")
    ca_der = cert_der(ca_crt)
    out.append(", ".join("0x%02x" % b for b in ca_der))
    out.append("\n};\nstatic const uint8_t tls_test_other_root[TEST_OTHER_CA_LEN] = {\n    ")
    other_der = cert_der(other_crt)
    out.append(", ".join("0x%02x" % b for b in other_der))
    out.append("\n};\n\nstatic const tls_root_t tls_test_roots[] = {\n"
               "    { tls_test_root, %d },\n" % len(ca_der) +
               "    { tls_test_other_root, %d },\n" % len(other_der) +
               "};\nstatic const int tls_test_roots_count = 2;\n")
    out.append("\n#endif // MCT_TLS_TEST_VECTORS_H\n")

    with open(args.out, "w") as f:
        f.write("".join(out))
    print("wrote %s (%d bytes)" % (args.out, os.path.getsize(args.out)))


if __name__ == "__main__":
    main()