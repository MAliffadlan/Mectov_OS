# TLS 1.3 Architecture (v38.162)

Mectov OS ships a TLS 1.3 client that runs entirely in Ring 3. It is a library
(`apps/lib/tls/`) linked into the apps that need it, not a kernel service: there
is **no new syscall** and no kernel change in this release. The kernel supplies
two things and nothing else — entropy (`SYS_GETRANDOM`, 117) and a byte stream
over `SYS_TCP_CONNECT` / `SYS_TCP_SEND` / `SYS_TCP_RECV` (40/41/42).

The design rule that shapes everything here: **the library is written from the
RFCs and checked against them before anything is allowed to depend on it.** A TLS
stack's interesting failures are not "could not connect" — they are "connected,
and the wrong thing was accepted". Those can only be tested against inputs fixed
in advance, so the gate for this release is offline and byte-exact, and the
browser integration (v38.163) comes after it.

---

## 📁 Layout

| File | Contents |
|---|---|
| `apps/lib/tls/tls.h` | Public API, error codes, sizing, `struct tls_conn` |
| `apps/lib/tls/tls_hash.c` | SHA-256, SHA-384, SHA-512, streaming HMAC, HKDF (`extract`/`expand`/`expand_label`), constant-time compare, endian helpers |
| `apps/lib/tls/tls_cipher.c` | ChaCha20, Poly1305 (26-limb), `AEAD_CHACHA20_POLY1305`, AES-128 (key schedule + block), GHASH/GCM, X25519 |
| `apps/lib/tls/tls_pubkey.c` | 32-bit-limb bignum, Montgomery exponentiation (CIOS), RSA PKCS#1 v1.5 + PSS verify, ECDSA P-256/P-384 verify (Jacobian) |
| `apps/lib/tls/asn1_x509.c` | DER reader, X.509 certificate parse, SPKI extraction, signature verification, SAN/CN hostname match, path building (`tls_verify_chain`) |
| `apps/lib/tls/tls13.c` | Record layer, ClientHello builder, handshake state machine, key schedule, CertificateVerify/Finished checks, app-data read/write |
| `apps/lib/tls/tls_roots.h` | **Generated** trust store (121 root certificates, DER) |
| `apps/lib/tls/test_vectors.h` | **Generated** test fixtures (RFC vectors, RSA/ECDSA signatures, real certificates) |
| `apps/tlsselftest.c` | The offline gate app (`run /apps/tlsselftest.mct`) |

Sizing constants live in one place in `tls.h`: `TLS_MAX_RECORD` 16640 (2^14 +
256, the largest legal record), `TLS_MAX_PLAIN` 16384, `TLS_HS_MAX` 12288 (a
reassembled handshake message — a certificate chain fits), `TLS_CHAIN_MAX` 5,
`TLS_HOSTNAME_MAX` 64, `TLS_BN_LIMBS` 128 (4096-bit RSA).

---

## 🤝 What the client offers (`tls13.c`)

`build_client_hello()` sends `legacy_version` 1.2 (required by the spec, and
never what gets negotiated), a random 32-byte `legacy_session_id` for middlebox
compatibility, and four extensions:

- **server_name (SNI)** — the host the caller asked for.
- **supported_groups** — X25519 only.
- **signature_algorithms** — `rsa_pss_rsae` SHA-256/384/512,
  `rsa_pss_pss` SHA-256/384/512, `rsa_pkcs1` SHA-256/384/512, and `ecdsa_secp256r1`
  / `ecdsa_secp384r1`.
- **supported_versions** — **TLS 1.3 and nothing else.** A TLS 1.2-only server
  fails the handshake here instead of being negotiated down to, so there is no
  downgrade path in this client at all.
- **key_share** — one X25519 share.

Two cipher suites are offered: `TLS_AES_128_GCM_SHA256` (0x1301) and
`TLS_CHACHA20_POLY1305_SHA256` (0x1303). A `ServerHello` naming anything else is
rejected rather than ignored.

### Entropy is a precondition, not a best effort

Every random value in the handshake — client random, session id, X25519 private
key — comes from `SYS_GETRANDOM`. **If the CSPRNG fails, `build_client_hello()`
returns 0 and the handshake is aborted.** There is no `rand()`-shaped fallback,
because a guessable key share is worse than a failed connection. A zero X25519
shared secret (the low-order-point case) is also rejected outright.

---

## 🔒 Record layer, and the one kernel-shaped constraint

TLS records are not one-to-one with TCP writes. `net_tcp_send()` in
`src/drivers/net.c` clamps a call at **1400 bytes** and the kernel keeps a
**single** retransmit slot, so `tls_flush()` splits outgoing records to fit that
clamp. A handshake record is small enough to be unaffected; application data is
where it matters.

Incoming data is buffered in `c->rec` and reassembled into `c->hs` until a whole
handshake message is available. Handshake messages that arrive coalesced in one
record are processed in order. `ChangeCipherSpec` records are **ignored** on
purpose — TLS 1.3 middleboxes still inject them, and they are not part of the
handshake transcript.

---

## 🧾 What the X.509 rule accepts and refuses

`tls_verify_chain()` walks the chain the server presented, verifying each
certificate's signature under its issuer's key, and stops at the first
certificate that matches an entry in the trust store byte for byte — a trust
anchor's own signature is not checked, because that is what being an anchor
means. The leaf must additionally:

- be currently valid (`notBefore <= now <= notAfter` on every certificate in
  the chain, using `sys_get_ticks()` — the OS has no wall clock, so the caller
  passes "now" and the gate derives it from the fixture rather than trusting a
  hardcoded timestamp);
- match the requested hostname, preferring **SAN dNSName / iPAddress** and
  falling back to the legacy CN only when no SAN is present;
- not be a CA when it is a leaf.

A literal IPv6 SAN is deliberately not matched: the OS network stack is IPv4-only
(`SYS_TCP_CONNECT` takes a 4-byte address), so an IPv6 literal can never name a
reachable host. Refusing it is correct; shipping an untested matcher would not
have been.

The trust store is compiled in and can only change by editing a file and
rebuilding. An OS image that can be argued into trusting a new CA is worse than
one that cannot. `scripts/gen_tls_roots.py` regenerates `tls_roots.h` from
`/etc/ssl/certs/ca-certificates.crt` (121 certificates, ~131 KB of DER).

There is no AIA chasing and no OCSP: the server must send its own intermediates.
That is a deliberate simplification, and it is why the failure is
`TLS_ERR_VERIFY` rather than a stall.

---

## ✅ The gate

`apps/tlsselftest.mct` links the library sources **directly** — not a prebuilt
libc — so the guest exercises the same object code the browser will use. It
needs no network and no server.

- **Primitives**, against RFC 4231 (HMAC-SHA-256), RFC 5869 (HKDF), RFC 8439
  (ChaCha20 2.3.2, Poly1305 2.5.2, AEAD 2.8.2), RFC 7748 (X25519 6.1),
  FIPS-197 (AES-128) and NIST GCM case 4.
- **Signatures**, against RSA PKCS#1 and ECDSA P-256/P-384 fixtures produced by
  OpenSSL — plus the negative cases, because a verifier that accepts everything
  passes every positive test.
- **X.509**, against real DER certificates: parse, CA/leaf distinction, RSA and
  EC SPKI extraction, validity window, SAN matching, and the path-building rule
  both accepting the issued chain and refusing the near-misses (wrong host,
  wrong CA, expired, tampered signature byte, missing issuer, and an empty trust
  store).

The app prints `checks N, failures M` and exits with the failure count.
`scripts/tls_selftest.py` drives it from the Terminal and asserts three things:
the tally reports **zero failures**, the count is at least the expected floor (so
a stack that answers two assertions and dies cannot look green from outside),
and no `[FAIL]` line appears anywhere in the serial log. Run it with
`python3 scripts/tls_selftest.py` or as the `tls` suite in `make check`.

For faster iteration, `scripts/tls_selftest_host.sh` runs the same file on the
host by substituting only `sys_print` and the entry symbol, so a failure can be
driven through a debugger without booting.

### What writing against those vectors actually found

Twelve real defects, eight in the primitives and four in the X.509 layer. The
partial list is worth keeping because each one is a class of mistake:

- the RSA exponent was read in two different bit orders, silently reducing
  `0x010001` to the exponent **1** — every positive verification failed while
  every negative "passed", which is the shape that makes a broken verifier look
  strict;
- leaving Montgomery form multiplied by the Montgomery image of 1 instead of by
  1, cancelling nothing;
- `jac_to_affine_x` applied one extra R;
- `jac_add`'s Y3 used the un-doubled `r` that X3 had already doubled, producing
  points that are not on the curve at all;
- `fsub` left a non-zero `n` for a zero result, so adding a point and its
  negation never yielded the point at infinity;
- the `AEAD_CHACHA20_POLY1305` trailer counted **bits** where RFC 8439 counts
  **bytes**;
- ChaCha20 loaded its key and nonce big-endian;
- the P-384 modulus and curve constant `b` were mistyped — twice, the second
  time in the hand-written Python "reference". Those constants are generated
  now, never typed;
- an OID comparison tested the body length against the whole TLV length;
- a cursor was reset to 0 between `notBefore` and `notAfter`, so the validity
  window was read as "starts before it starts";
- an offset was reused for an inner SEQUENCE, so `RSAPublicKey` was parsed 11
  bytes into the structure;
- the RSA modulus kept its DER sign byte (257 bytes for a 2048-bit key).

---

## ⛔ Not in this release

`apps/browser.c` still speaks plain HTTP. `tls_roots[]` is linked into the
engine but the browser does not call it yet, and `Content-Length` / chunked
transfer decoding for an HTTPS response body is v38.163. This release claims a
verified TLS 1.3 engine and an offline gate, not a working HTTPS browser.
