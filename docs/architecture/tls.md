# TLS 1.3 Architecture (v38.163)

Mectov OS ships a TLS 1.3 client that runs entirely in Ring 3. It is a library
(`apps/lib/tls/`) linked into the apps that need it, not a kernel service: there
is **no new syscall** and no kernel change in this release. The kernel supplies
two things and nothing else — entropy (`SYS_GETRANDOM`, 117) and a byte stream
over `SYS_TCP_CONNECT` / `SYS_TCP_SEND` / `SYS_TCP_RECV` (40/41/42).

The design rule that shapes everything here: **the library is written from the
RFCs and checked against them before anything is allowed to depend on it.** A TLS
stack's interesting failures are not "could not connect" — they are "connected,
and the wrong thing was accepted". Those can only be tested against inputs fixed
in advance, so the primary gate is offline and byte-exact, and the browser
integration came after it.

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
  the chain). The engine reads the RTC (`SYS_GET_TIME`) and converts the civil
  date itself, because there is no libc behind it; the verifier takes "now" as a
  parameter so the self-test can hold it fixed against fixtures whose validity
  windows are known — a suite that reads the clock is a suite that fails in a
  few years;
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

---

## 🔬 The gate that completes a handshake (`scripts/tls_handshake_test.py`)

The self-test above drives every primitive with committed vectors, and a guest
has no server whose chain the image trusts — so for a whole release the record
layer, the key schedule and the certificate-list framing had **no test that ever
finished a handshake**. That is a large blind spot with a particular shape: a
wrong key schedule does not stop a handshake, it makes one that looks fine from
the outside and fails only when a peer refuses to decrypt.

`scripts/tls_handshake_test.py` closes it, on the host, in seconds. It compiles
the **shipped** `apps/lib/tls/tls13.c` with exactly two `#include` lines
rewritten — the kernel's `src/include/syscall.h` becomes `scripts/tls_host_shim.h`
(libc `getrandom` + the RTC), and the image's trust store becomes one generated
from a throwaway CA by the shipped `scripts/gen_tls_roots.py`. Nothing else in
the engine differs, so what runs is what ships.

It then drives a real OpenSSL TLS 1.3 server through
`scripts/tls_host_client.c` and asserts worth making:

- the handshake completes and the server reports TLS 1.3 (so the ClientHello,
  the key share and our Finished were all accepted);
- **every secret the engine derives matches the server's keylog** — both
  handshake traffic secrets and both application traffic secrets. The engine is
  built with `TLS_DEBUG_SECRETS` here, which is the only reason the comparison
  exists; it is a host-only build and the macro is defined by no release path;
- the encrypted request is decrypted by the server and the encrypted reply by
  the engine, byte for byte;
- a chain the store does not contain is refused with `TLS_ERR_VERIFY` — not with
  a protocol error — and no application data crosses in either direction.

### What the first completed handshake found: nine defects

Every one of them passed `tls_selftest.py` and `browser_https_test.py`.

| # | Defect | Why it hid |
|---|---|---|
| 1 | `Derive-Secret(Secret, "derived", "")` used a zero-length context where the RFC means `Transcript-Hash("")` — SHA-256 of the empty string (RFC 8448 pins it at `6f2615a1…`). | It poisons the **handshake secret**, so both handshake traffic secrets are wrong — but every record up to the ServerHello is in the clear, so the suite above still saw a valid ClientHello, a parseable ServerHello and a matching transcript. |
| 2 | The AEAD's additional data was omitted, on seal and on open. RFC 8446 §5.2 makes it the record header. | Outgoing records were still well-formed; only the peer's tags failed, and only once records became encrypted. |
| 3 | `split_certificates()` never consumed each `CertificateEntry`'s 2-byte extensions length. | The length check failed on every real chain; hidden behind #1, which failed earlier. |
| 4 | The certificate list reached the verifier still carrying its wire framing, so the first certificate was parsed starting at its own 3-byte length. | Same: #1 got there first. |
| 5 | MGF1 appended a **one**-byte counter where RFC 8017 requires four octets, so every RSA-PSS signature failed. | The self-test never verified a PSS signature — it only checked that SHA-384 existed "for RSA-PSS". |
| 6 | The master secret was derived from the server handshake **traffic** secret instead of from the handshake secret, which the struct did not keep. | The handshake **completes**: the Finished is checked with the handshake keys and passes. The only symptom is the peer refusing to decrypt the first request. |
| 7 | The application secrets were derived after our own Finished had been folded into the transcript. | Identical shape to #6. |
| 8 | `tls_step()` returned early once the handshake was done, so `tls_read()` never touched the socket again. | Every post-handshake byte — the reply, a ticket, a `close_notify` — sat unread for the life of the connection. The client simply reported "nothing yet" forever. |
| 9 | `tls_read()` discarded buffered plaintext when the pump stopped with an error or a close in the same call. | This is how a `Content-Length` reply arrives — the peer answers and closes in one burst — so a complete, already-decrypted response was thrown away. |

Two more were found and fixed in `scripts/browser_https_test.py` itself, and are
recorded because both are the same mistake in different clothes — a test that
tested the wrong thing:

- `ctx.wrap_socket(far, server_side=True)` performs the **whole handshake**
  before returning, and it was called before the byte-recording relay threads
  existed. The ClientHello never reached the TLS stack and the call blocked until
  the guest gave up; the suite reported it as a malformed ClientHello.
  `do_handshake_on_connect=False` is load-bearing there.
- The browsing helper sampled the serial log **after** submitting the URL, so a
  TLS refusal — which only has to reach the server's Certificate and is therefore
  fast — landed before the window being inspected, and a correct refusal was
  reported as a failure to refuse.

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

## 🌐 HTTPS in the Mini Browser

`apps/browser.c` treats an `https://` URL as the same request with a different
transport. The URL parser sets two flags — `req_tls` (what was asked for) and
`using_tls` (what the in-flight request actually is) — and every later branch
keys on `using_tls`, so one code path serves both schemes. The scheme decides the
default port (443 for `https`, 80 for `http`); a port given in the URL always
wins.

The handshake is **state 4**, and it is driven one `tls_step()` per poll from the
browser's existing loop rather than by a blocking call, so the window keeps
repainting and the app never sits inside `tls_handshake()`. `tls_step()` returns
`TLS_WANT_READ` / `TLS_WANT_WRITE` for "call me again", which is exactly the
contract a poll loop wants.

The handshake and the request are both observable on serial, because a TLS
failure inside a GUI app is otherwise invisible to a test:

```
[BROWSER] tls-ok cipher=TLS_CHACHA20_POLY1305_SHA256 host=10.0.2.2
[BROWSER] tls-fail TLS certificate not trusted
[BROWSER] done tls=1 code=200 bytes=115 body=99 host=10.0.2.2
```

`done` is emitted on every path — success, refusal and timeout alike — so a
missing marker means a request still in flight rather than a silent success.

**Response framing** is decided by the headers, in this order:
`Transfer-Encoding: chunked` (de-chunked in place before the body is measured),
then `Content-Length` (the read stops at the promised length instead of waiting
for the peer to close), then close-delimited. A reply that arrives without
`close_notify` is accepted when bytes already arrived and reported as
`[TLS] peer closed without close_notify`; a connection that ends with nothing at
all is a failure.

A refused chain renders **nothing** — the page area is never touched — and the
verdict comes from `TLS_ERR_VERIFY`, which is why the browser prints "not
trusted" for an unknown CA and a different message for a protocol error. That
distinction is deliberate: "it refused" and "it refused for the right reason"
are different claims, and only the second one is worth anything.

`scripts/browser_https_test.py` is the end-to-end check. Its TLS server holds a
throwaway chain the image deliberately does **not** trust, so the assertion that
matters is the negative one: the server must log `first_byte=0x16` **and** a
parsed `[SRV] sni=` (OpenSSL only runs its SNI callback once the ClientHello has
parsed cleanly, which is what makes a malformed extension layout observable), and
the browser must then refuse and render nothing. The same run proves plain-HTTP
framing on two control pages, asserting the *rendered body length* — a chunked
reply whose framing leaked into the render shows up as a wrong number rather than
as odd-looking pixels.

## 🚦 Not in this release

The engine's scope has not changed: **TLS 1.3 only** (no TLS 1.2, so a 1.2-only
server fails rather than being negotiated down to), **no session resumption or
PSK** (every `https://` fetch pays a full handshake), **no 0-RTT**, **no client
certificates**, **no HelloRetryRequest**, and **no certificate fetching beyond
what the server sends** (no AIA, no OCSP) — so a server that omits an
intermediate fails with `TLS_ERR_VERIFY` rather than being repaired. X25519 is
the only key exchange and the two cipher suites listed above are the only ones.

On the browser side, `apps/browser.c` makes one request per connection
(HTTP/1.0-style `GET`, no keep-alive, no HTTP/2, no cookies), and it has no
proxy or `CONNECT` support. The slirp gateway redirects port 80 only: **port 443
is not redirected**, which is why the HTTPS suite uses its own port, and the
kernel's `src/drivers/net.c` needed no change for any of this.
