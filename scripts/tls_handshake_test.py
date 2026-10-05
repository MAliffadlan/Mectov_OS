#!/usr/bin/env python3
"""scripts/tls_handshake_test.py — drive the shipped TLS engine against a REAL
TLS 1.3 server, on the host, and check the result against the server's keylog.

Why this gate exists
--------------------
scripts/tls_selftest.py runs the engine's primitives inside the guest against
committed RFC and OpenSSL vectors, and scripts/browser_https_test.py proves the
browser wires the engine up and refuses an untrusted chain. Neither one drives a
handshake that is allowed to COMPLETE, so neither one can reach the record
layer, the key schedule or the certificate-list framing. Nine defects lived in
exactly that region, and every one of them passed both suites:

  * Derive-Secret("derived", "") used a zero-length context where the RFC means
    Transcript-Hash("") -- which poisons the handshake secret, so both handshake
    traffic secrets are wrong while the whole handshake still looks healthy.
  * the AEAD's additional data was omitted, so records sealed without covering
    the header could never authenticate on the way in;
  * the Certificate list's per-entry extensions length was never consumed;
  * the chain was handed to the verifier still carrying its wire framing;
  * MGF1's counter was one byte instead of four, breaking every RSA-PSS
    signature (which the self-test never verified at all);
  * the master secret was derived from a handshake TRAFFIC secret rather than
    from the handshake secret;
  * the application secrets were taken after our own Finished had been folded
    into the transcript;
  * tls_step() stopped reading the socket once the handshake finished, so no
    post-handshake byte was ever read; and
  * tls_read() discarded buffered plaintext when the peer closed in the same
    call, which is how every Content-Length reply arrives.

What this checks
----------------
  1. A full handshake completes against OpenSSL, and the server reports TLS 1.3
     (so the ClientHello, the key share and our Finished were all accepted).
  2. Every secret the engine derives -- both handshake traffic secrets and both
     application traffic secrets -- is byte-identical to the ones OpenSSL wrote
     to its keylog for the same connection. This is the assertion the other two
     suites cannot make, and it is what catches a wrong transcript, a wrong
     "derived" step or a wrong stage input.
  3. The request the engine encrypts is decrypted by the server, and the reply
     the server sends is decrypted by the engine, byte for byte.
  4. A chain that is NOT in the trust store is refused with the verification
     error -- not with a protocol error, and without any application data
     crossing in either direction.

The trust store is the caller's: the root CA is generated per run and turned
into a roots header with the shipped scripts/gen_tls_roots.py. The image's own
store is untouched, and the second server uses a different throwaway CA so the
refusal path is exercised with real crypto rather than a hand-made failure.

Runs on the host in a few seconds. Needs gcc with -m32 (as
scripts/tls_selftest_host.sh does) and python3-cryptography.

Exit code 0 = pass, 1 = fail, 2 = environment cannot run the suite.
"""
import argparse
import hashlib
import os
import re
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from datetime import datetime, timedelta, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

BODY = b"host TLS engine integration reply\n"

# Labels the engine logs under TLS_DEBUG_SECRETS -> the keylog label they must
# match. If either side renames anything, this table is the one place to edit.
KEYLOG_MATCH = {
    "c hs sec": "CLIENT_HANDSHAKE_TRAFFIC_SECRET",
    "s hs sec": "SERVER_HANDSHAKE_TRAFFIC_SECRET",
    "c ap sec": "CLIENT_TRAFFIC_SECRET_0",
    "s ap sec": "SERVER_TRAFFIC_SECRET_0",
}


# --------------------------------------------------------------- fixtures

def make_identity(directory, ca_cn, slug):
    """A throwaway CA plus a leaf for localhost, as PEM files.

    `slug` names the files. Deriving them from the CN instead collapses both
    identities onto one set of paths -- the last one written wins, and the
    "untrusted" server then ends up serving the very CA the store has, so the
    refusal case passes and proves nothing.
    """
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID

    now = datetime.now(timezone.utc)
    ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, ca_cn)])
    ca = (x509.CertificateBuilder()
          .subject_name(ca_name).issuer_name(ca_name)
          .public_key(ca_key.public_key())
          .serial_number(x509.random_serial_number())
          .not_valid_before(now - timedelta(days=1))
          .not_valid_after(now + timedelta(days=3650))
          .add_extension(x509.BasicConstraints(ca=True, path_length=None),
                         critical=True)
          .sign(ca_key, hashes.SHA256()))

    leaf_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    leaf = (x509.CertificateBuilder()
            .subject_name(x509.Name([
                x509.NameAttribute(NameOID.COMMON_NAME, "localhost")]))
            .issuer_name(ca_name)
            .public_key(leaf_key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - timedelta(days=1))
            .not_valid_after(now + timedelta(days=365))
            .add_extension(x509.BasicConstraints(ca=False, path_length=None),
                           critical=True)
            .add_extension(x509.SubjectAlternativeName([x509.DNSName("localhost")]),
                           critical=False)
            .sign(ca_key, hashes.SHA256()))

    ca_pem = os.path.join(directory, "%s-ca.pem" % slug)
    leaf_pem = os.path.join(directory, "%s-leaf.pem" % slug)
    key_pem = os.path.join(directory, "%s-leaf.key" % slug)
    with open(ca_pem, "wb") as f:
        f.write(ca.public_bytes(serialization.Encoding.PEM))
    with open(leaf_pem, "wb") as f:
        f.write(leaf.public_bytes(serialization.Encoding.PEM)
                + ca.public_bytes(serialization.Encoding.PEM))
    with open(key_pem, "wb") as f:
        f.write(leaf_key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption()))
    return ca_pem, leaf_pem, key_pem


# --------------------------------------------------------------- the driver

def build_driver(tmp, ca_pem):
    """Compile scripts/tls_host_client.c against a copy of the engine.

    The copy differs from apps/lib/tls/tls13.c in two `#include` lines and
    nothing else: the kernel's syscall header becomes the host shim, and the
    image's trust store becomes one built from `ca_pem`. Everything else --
    including every line this gate is here to test -- is the shipped source.
    """
    src = os.path.join(tmp, "tls13_host.c")
    with open(os.path.join(ROOT, "apps/lib/tls/tls13.c")) as f:
        text = f.read()
    before = text
    text = text.replace('#include "src/include/syscall.h"',
                        '#include "tls_host_shim.h"')
    text = text.replace('#include "tls_roots.h"', '#include "tls_roots_host.h"')
    if text == before:
        raise RuntimeError("tls13.c no longer has the includes this gate "
                           "rewrites; update scripts/tls_handshake_test.py")
    with open(src, "w") as f:
        f.write(text)

    roots_dir = os.path.join(tmp, "roots")
    os.makedirs(roots_dir, exist_ok=True)
    shutil.copy(ca_pem, roots_dir)
    subprocess.run(
        [sys.executable, os.path.join(HERE, "gen_tls_roots.py"),
         "--dir", roots_dir, "--out", os.path.join(tmp, "tls_roots_host.h")],
        check=True, capture_output=True)

    out = os.path.join(tmp, "driver")
    cmd = [
        "gcc", "-m32", "-O1", "-w",
        # The debug dump is what makes the keylog comparison possible, and this
        # binary is host-only: it is never linked into the image.
        "-DTLS_DEBUG_SECRETS",
        "-I" + ROOT, "-I" + os.path.join(ROOT, "apps"),
        "-I" + os.path.join(ROOT, "apps/lib/tls"), "-I" + HERE, "-I" + tmp,
        "-o", out,
        os.path.join(HERE, "tls_host_client.c"), src,
        os.path.join(ROOT, "apps/lib/tls/tls_hash.c"),
        os.path.join(ROOT, "apps/lib/tls/tls_cipher.c"),
        os.path.join(ROOT, "apps/lib/tls/tls_pubkey.c"),
        os.path.join(ROOT, "apps/lib/tls/asn1_x509.c"),
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        raise RuntimeError("could not build the host TLS driver")
    return out


# --------------------------------------------------------------- the server

class Server:
    """A one-connection TLS 1.3 server that records what the client sent."""

    def __init__(self, cert, key, keylog):
        self.cert, self.key, self.keylog = cert, key, keylog
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.port = self.sock.getsockname()[1]
        self.request = None
        self.sent = b""
        self.handshake = None
        self.error = None
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._serve, daemon=True)

    def start(self):
        self.thread.start()
        return self

    def _serve(self):
        self.sock.settimeout(0.5)
        while not self.stop.is_set():
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(self.cert, self.key)
            ctx.minimum_version = ssl.TLSVersion.TLSv1_3
            ctx.maximum_version = ssl.TLSVersion.TLSv1_3
            # The keylog is the whole point: it is the server's own record of
            # the secrets, so the engine's derivation can be compared against
            # something it did not produce.
            ctx.keylog_filename = self.keylog
            try:
                tls = ctx.wrap_socket(conn, server_side=True)
                self.handshake = "%s %s" % (tls.version(), tls.cipher()[0])
                tls.settimeout(5.0)
                data = b""
                while b"\r\n\r\n" not in data:
                    chunk = tls.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                self.request = data.split(b"\r\n", 1)[0].decode("latin1", "replace")
                # Keep the exact bytes sent: the engine hands back everything it
                # decrypted, headers included, so this is what its digest must
                # match. Hashing the bare body here would compare a payload
                # against a whole message and fail on a correct round trip.
                self.sent = (b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                             b"Content-Length: %d\r\n\r\n" % len(BODY)) + BODY
                tls.sendall(self.sent)
                try:
                    tls.unwrap()
                except Exception:                                # noqa: BLE001
                    pass
                tls.close()
            except Exception as e:                               # noqa: BLE001
                self.error = repr(e)
                try:
                    conn.close()
                except OSError:
                    pass

    def close(self):
        self.stop.set()
        try:
            self.sock.close()
        except OSError:
            pass


# --------------------------------------------------------------- assertions

def run_driver(driver, port, sni="localhost", path="/", timeout=120):
    r = subprocess.run([driver, "127.0.0.1", str(port), sni, path],
                       capture_output=True, text=True, timeout=timeout)
    return r.stdout


def parse(out):
    """Engine log lines and [DRV] lines, in one dict.

    Each [DRV] line is matched explicitly. A generic key=value reader looks
    simpler and quietly mis-reads these: `handshake=0 cipher=X` would hand back
    the cipher where the verdict is expected, and `body=N sha256=H` the digest.
    """
    info = {"log": {}}
    for line in out.splitlines():
        line = line.strip()
        m = re.match(r"\[DRV\] handshake=(-?\d+) cipher=(\S+)", line)
        if m:
            info["handshake"] = m.group(1)
            info["cipher"] = m.group(2)
            continue
        m = re.match(r"\[DRV\] request=(\d+)", line)
        if m:
            info["request"] = m.group(1)
            continue
        m = re.match(r"\[DRV\] body=(\d+) sha256=([0-9a-f]+)", line)
        if m:
            info["body"] = m.group(1)
            info["sha256"] = m.group(2)
            continue
        m = re.match(r"\[DRV\] result=(\w+)", line)
        if m:
            info["result"] = m.group(1)
            continue
        m = re.match(r"\[TLS\] (.+?)\s+([0-9a-f]{64})$", line)
        if m:
            info["log"][m.group(1).strip()] = m.group(2)
    return info


def read_keylog(path):
    secrets = {}
    try:
        with open(path) as f:
            for line in f:
                parts = line.split()
                if len(parts) == 3:
                    secrets[parts[0]] = parts[2]
    except FileNotFoundError:
        pass
    return secrets


def main():
    ap = argparse.ArgumentParser()
    # check.py hands every suite a --timeout; this one is host-only and finishes
    # in seconds, so the value bounds each driver run rather than the suite.
    ap.add_argument("--timeout", type=int, default=120,
                    help="seconds to allow a single driver run")
    ap.add_argument("--keep", action="store_true",
                    help="keep the build directory for inspection")
    args = ap.parse_args()

    if shutil.which("gcc") is None:
        print("[SKIP] gcc is not installed")
        return 2
    try:
        import cryptography                                    # noqa: F401
    except ImportError:
        print("[SKIP] python3-cryptography is not installed")
        return 2

    failures = []
    tmp = tempfile.mkdtemp(prefix="mct_tls_host_")
    try:
        trusted_ca, trusted_leaf, trusted_key = make_identity(
            tmp, "Mectov Host Trusted CA", "trusted")
        _, untrusted_leaf, untrusted_key = make_identity(
            tmp, "Mectov Host Untrusted CA", "untrusted")

        try:
            driver = build_driver(tmp, trusted_ca)
        except RuntimeError as e:
            print("[FAIL] %s" % e)
            return 1
        print("[OK] built the host driver from the shipped engine source")

        # ---- 1. the positive path, checked against the server's keylog ----
        keylog = os.path.join(tmp, "keylog.txt")
        srv = Server(trusted_leaf, trusted_key, keylog).start()
        time.sleep(0.3)
        out = run_driver(driver, srv.port, timeout=args.timeout)
        time.sleep(0.3)
        srv.close()
        info = parse(out)
        secrets = read_keylog(keylog)

        if info.get("handshake") != "0":
            print("[FAIL] the handshake did not complete: %s" %
                  info.get("handshake", "(no verdict line)"))
            for line in out.splitlines():
                if line.startswith("[TLS]"):
                    print("       " + line)
            failures.append("handshake")
        elif not str(srv.handshake).startswith("TLSv1.3"):
            print("[FAIL] the server negotiated %r, not TLS 1.3" % (srv.handshake,))
            failures.append("version")
        else:
            print("[OK] handshake completed; server says %s; engine says %s" %
                  (srv.handshake, info.get("cipher")))

        # The assertion no other suite can make.
        if "handshake" not in failures:
            if not secrets:
                print("[FAIL] the server wrote no keylog (is it OpenSSL 1.1+?)")
                failures.append("keylog missing")
            for tag, key in KEYLOG_MATCH.items():
                got, want = info["log"].get(tag), secrets.get(key)
                if got is None:
                    print("[FAIL] the engine never logged %r" % tag)
                    failures.append(tag)
                elif got != want:
                    print("[FAIL] %s does not match the server's %s" % (tag, key))
                    print("       engine: %s" % got)
                    print("       server: %s" % want)
                    failures.append(tag)
                else:
                    print("[OK] %s matches the server's %s" % (tag, key))

            # And the payloads, in both directions.
            if srv.request != "GET / HTTP/1.0":
                print("[FAIL] the server saw request %r" % (srv.request,))
                failures.append("request")
            elif info.get("body") == "0":
                print("[FAIL] the engine decrypted no reply body")
                failures.append("body")
            else:
                want = hashlib.sha256(srv.sent).hexdigest()
                if info.get("sha256") != want:
                    print("[FAIL] the reply body did not survive the round trip")
                    print("       engine sha256: %s" % info.get("sha256"))
                    print("       server sha256: %s" % want)
                    failures.append("body")
                else:
                    print("[OK] request decrypted by the server, %s-byte reply "
                          "decrypted by the engine" % info.get("body"))

        # ---- 2. a chain the store does not contain must be REFUSED ----
        bad_log = os.path.join(tmp, "keylog_bad.txt")
        bad = Server(untrusted_leaf, untrusted_key, bad_log).start()
        time.sleep(0.3)
        out = run_driver(driver, bad.port, timeout=args.timeout)
        time.sleep(0.3)
        bad.close()
        info = parse(out)
        # TLS_ERR_VERIFY is -3. Anything else -- a protocol error, a timeout, a
        # crash -- is a different bug wearing the same "it refused" costume.
        if info.get("handshake") != "-3":
            print("[FAIL] an untrusted chain gave %s, expected -3 (TLS_ERR_VERIFY)"
                  % info.get("handshake", "(no verdict line)"))
            for line in out.splitlines():
                if line.startswith("[TLS]"):
                    print("       " + line)
            failures.append("refusal verdict")
        elif bad.request is not None:
            print("[FAIL] the server received a request over a refused chain")
            failures.append("refusal leaked data")
        else:
            print("[OK] an untrusted chain was refused with TLS_ERR_VERIFY and "
                  "no application data")

        if failures:
            print("[FAIL] tls_handshake_test: %s" % ", ".join(failures))
            return 1
        print("[OK] tls_handshake_test: the engine talks to a real TLS 1.3 "
              "server and derives the same keys it does")
        return 0
    finally:
        if args.keep:
            print("[i] build directory kept at %s" % tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
