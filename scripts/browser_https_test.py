#!/usr/bin/env python3
"""scripts/browser_https_test.py — CI gate for HTTPS in the Mini Browser (v38.163).

The Mini Browser now understands https://. This suite proves two things that
matter, and one of them is the negative case:

  1. **An untrusted certificate is refused, end to end, by the browser.**
     A TLS server on the host presents a leaf issued by a throwaway CA that is
     deliberately NOT in the image's trust store. The browser must complete a
     real ClientHello (the server saw one), reach certificate verification, and
     then refuse — reporting `[BROWSER] tls-fail ... not trusted` — without
     rendering a single byte of the server's page. That is the property HTTPS
     exists for, and it is fully deterministic offline: no internet, no public
     CA, no clock skew.

  2. **The plain-HTTP path still works, including response framing.**
     The same host serves a control page twice on another port: once with
     Content-Length (the read must stop when the promised body arrives instead
     of waiting for the peer to close) and once with Transfer-Encoding: chunked
     (the body must be de-chunked before rendering). The browser reports the
     rendered body length on serial, so chunk framing that leaked into the
     render would show up as a wrong number rather than as odd-looking pixels.

Why a self-signed server is the RIGHT fixture for the positive half of (1):
the image's trust store is the real public CA bundle and nothing can add to it
at runtime, so there is no way to make the browser trust a local test server —
by design. The positive handshake path is covered offline and much more
precisely by `scripts/tls_selftest.py` (RFC/NIST vectors, OpenSSL-made
signatures, real DER chains), which is where a bug in the engine would be
caught. This suite covers the wiring: that the browser drives the engine, sends
SNI for the URL's host, and acts on the verdict.

Host networking: the guest reaches the host at the slirp gateway address
10.0.2.2, so both servers bind 127.0.0.1 on the host and the URLs are
`https://10.0.2.2:<port>/`. Port 443 is NOT redirected to the gateway, so a
custom port is used and nothing in the kernel needs to change.

Exit code 0 = pass, 1 = fail.
"""
import argparse
import os
import re
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from datetime import datetime, timedelta, timezone

SERIAL_LOG = "/tmp/mectov_https_serial.log"
MON_SOCK = "/tmp/mectov_https_monitor.sock"
DUMP = "/tmp/mectov_https_page.ppm"
TLS_LOG = "/tmp/mectov_https_server.log"
PLAIN_LOG = "/tmp/mectov_https_plain.log"
HELLO_DUMP = "/tmp/mectov_https_clienthello.hex"
FLIGHT_DUMP = "/tmp/mectov_https_serverflight.hex"
KEYLOG = "/tmp/mectov_https_keylog.txt"

TLS_PORT = 8443
PLAIN_PORT = 8080

HOST_ALIAS = "10.0.2.2"

# The control page's body, byte for byte. Asserting on its exact length is the
# point: a chunked reply that rendered its framing would report a longer body.
PLAIN_BODY = ("plain control page -- content-length framing.\n"
              "second line of the control body.\n")
CHUNK_BODY = ("chunked control page -- de-chunked before render.\n"
              "second line of the chunked body.\n")

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
# Start menu panel top (START_MENU_H = 404 since Pixel Paint); row 3 is
# "Mini Browser".
SM_Y = (768 - 28) - 404
WIN_X0, WIN_Y0 = 50, 50
URL_FIELD_Y0 = WIN_Y0 + 25


# --------------------------------------------------------------- fixtures

def make_server_identity(directory):
    """A throwaway CA plus a leaf for 10.0.2.2 and localhost.

    Written to a temp dir and never committed: a checked-in private key for a
    name the guest will connect to is a liability, and the chain being
    untrusted is the whole point of the test.
    """
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID

    now = datetime.now(timezone.utc)
    ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Mectov HTTPS Test CA")])
    ca = (x509.CertificateBuilder()
          .subject_name(ca_name).issuer_name(ca_name)
          .public_key(ca_key.public_key())
          .serial_number(x509.random_serial_number())
          .not_valid_before(now - timedelta(days=1))
          .not_valid_after(now + timedelta(days=3650))
          .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
          .sign(ca_key, hashes.SHA256()))

    leaf_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    leaf = (x509.CertificateBuilder()
            .subject_name(x509.Name([
                x509.NameAttribute(NameOID.COMMON_NAME, HOST_ALIAS)]))
            .issuer_name(ca_name)
            .public_key(leaf_key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - timedelta(days=1))
            .not_valid_after(now + timedelta(days=365))
            .add_extension(x509.BasicConstraints(ca=False, path_length=None),
                           critical=True)
            .add_extension(x509.SubjectAlternativeName([
                x509.IPAddress(__import__("ipaddress").ip_address(HOST_ALIAS)),
                x509.DNSName("localhost")]), critical=False)
            .sign(ca_key, hashes.SHA256()))

    cert_path = os.path.join(directory, "leaf.pem")
    key_path = os.path.join(directory, "leaf.key")
    with open(cert_path, "wb") as f:
        f.write(leaf.public_bytes(serialization.Encoding.PEM))
        f.write(ca.public_bytes(serialization.Encoding.PEM))
    with open(key_path, "wb") as f:
        f.write(leaf_key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption()))
    return cert_path, key_path


# --------------------------------------------------------------- servers

def serve_tls(sock, cert_path, key_path, log_path, stop):
    """Accept, log the first wire byte, then hand the socket to TLS.

    The first-byte log is the evidence that the BROWSER emitted a TLS
    ClientHello: 0x16 is the Handshake record type. Without it, "the browser
    refused the certificate" would be indistinguishable from "the browser never
    spoke TLS at all".
    """
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert_path, key_path)
    # Ground truth for the key schedule. OpenSSL writes the traffic secrets it
    # actually used here, so when the client cannot decrypt the server's
    # encrypted flight the disagreement can be located exactly -- is the
    # ECDHE share wrong, or the transcript, or the label expansion? -- instead
    # of guessed at.
    if os.environ.get("MECTOV_HTTPS_KEYLOG", "1") == "1":
        ctx.keylog_filename = KEYLOG
    log = open(log_path, "w", buffering=1)

    def on_sni(_sock, name, _ctx):
        # OpenSSL only invokes this after the ClientHello has parsed cleanly and
        # the server_name extension has been found, so a logged name is proof
        # that the extension layout is right -- which is exactly what a
        # malformed ClientHello fails on (the first version of this suite saw
        # BAD_EXTENSION here).
        log.write("[SRV] sni=%s\n" % name)

    ctx.set_servername_callback(on_sni)
    sock.settimeout(1.0)
    while not stop.is_set():
        try:
            conn, addr = sock.accept()
        except socket.timeout:
            continue
        except OSError:
            break
        try:
            conn.settimeout(5.0)
            # MSG_PEEK does not consume, so the record can be recorded AND still
            # handed to the TLS stack. When a ClientHello is malformed the hex
            # dump is the only artefact that says where, so it is written on
            # every run rather than behind a flag.
            head = conn.recv(2048, socket.MSG_PEEK)
            log.write("[SRV] connection from %s first_byte=0x%02x len=%d\n"
                      % (addr[0], head[0] if head else 0, len(head)))
            if head:
                with open(HELLO_DUMP, "w") as f:
                    f.write(head.hex())
            # Run the handshake over a socketpair so the server's own bytes can
            # be recorded as they go out. Without them a client that cannot
            # decrypt the server's encrypted flight is undiagnosable: the
            # transcript hash covers ServerHello, and the ECDHE share lives in
            # it, so an offline reproduction needs those exact bytes.
            try:
                near, far = socket.socketpair()
                # do_handshake_on_connect=False is load-bearing: wrap_socket()
                # otherwise runs the whole handshake right here, BEFORE the relay
                # threads below exist, so the ClientHello sitting in conn's
                # buffer never reaches the TLS stack and this call blocks until
                # the guest gives up. The handshake is driven explicitly, after
                # the relay is running.
                tls = ctx.wrap_socket(far, server_side=True,
                                      do_handshake_on_connect=False)
            except (ssl.SSLError, OSError) as e:
                log.write("[SRV] tls setup failed: %s\n" % e)
                conn.close()
                continue
            captured = bytearray()
            stop_relay = threading.Event()

            def to_client():
                try:
                    while not stop_relay.is_set():
                        data = near.recv(4096)
                        if not data:
                            break
                        captured.extend(data)
                        conn.sendall(data)
                except OSError:
                    pass
                try:
                    conn.shutdown(socket.SHUT_WR)
                except OSError:
                    pass

            def to_server():
                try:
                    while not stop_relay.is_set():
                        data = conn.recv(4096)
                        if not data:
                            break
                        near.sendall(data)
                except OSError:
                    pass

            relay_out = threading.Thread(target=to_client, daemon=True)
            relay_in = threading.Thread(target=to_server, daemon=True)
            relay_out.start()
            relay_in.start()
            try:
                tls.do_handshake()
            except (ssl.SSLError, OSError) as e:
                # Expected: the client aborts once it has rejected the chain.
                log.write("[SRV] tls handshake ended: %s\n" % e)
                with open(FLIGHT_DUMP, "w") as f:
                    f.write(bytes(captured).hex())
                stop_relay.set()
                conn.close()
                continue
            log.write("[SRV] tls handshake completed cipher=%s\n" % (tls.cipher(),))
            try:
                tls.settimeout(5.0)
                req = tls.recv(4096)
                log.write("[SRV] request: %r\n" % req[:200])
            except (ssl.SSLError, OSError) as e:
                log.write("[SRV] read failed: %s\n" % e)
            tls.close()
        except Exception as e:                                   # noqa: BLE001
            log.write("[SRV] error: %s\n" % e)
            try:
                conn.close()
            except Exception:                                    # noqa: BLE001
                pass
    log.close()


def serve_plain(sock, log_path, stop):
    """A control HTTP server: `/len` uses Content-Length, `/chunked` chunks."""
    log = open(log_path, "w", buffering=1)
    sock.settimeout(1.0)
    while not stop.is_set():
        try:
            conn, _addr = sock.accept()
        except socket.timeout:
            continue
        except OSError:
            break
        try:
            conn.settimeout(5.0)
            req = b""
            while b"\r\n\r\n" not in req and len(req) < 4096:
                chunk = conn.recv(1024)
                if not chunk:
                    break
                req += chunk
            first = req.split(b"\r\n", 1)[0].decode("latin1", "replace")
            log.write("[SRV] %s\n" % first)
            if "/chunked" in first:
                body = CHUNK_BODY.encode()
                head = (b"HTTP/1.1 200 OK\r\n"
                        b"Content-Type: text/html\r\n"
                        b"Transfer-Encoding: chunked\r\n\r\n")
                out = head
                for i in range(0, len(body), 16):
                    piece = body[i:i + 16]
                    out += b"%x\r\n" % len(piece) + piece + b"\r\n"
                out += b"0\r\n\r\n"
            else:
                body = PLAIN_BODY.encode()
                out = (b"HTTP/1.0 200 OK\r\n"
                       b"Content-Type: text/html\r\n"
                       b"Content-Length: %d\r\n\r\n" % len(body)) + body
            conn.sendall(out)
            conn.close()
        except OSError as e:
            log.write("[SRV] error: %s\n" % e)
            try:
                conn.close()
            except Exception:                                    # noqa: BLE001
                pass
    log.close()


# --------------------------------------------------------------- harness

def wait_for_in_file(path, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, "r", errors="replace") as f:
                if needle in f.read():
                    return True
        except (FileNotFoundError, OSError):
            pass
        time.sleep(0.5)
    return False


def read_log(path):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read()
    except (FileNotFoundError, OSError):
        return ""


def mon_cmd(cmd, wait=0.15):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(wait)
        s.close()
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")


cur_x, cur_y = 0, 0


def move_abs(x, y):
    global cur_x, cur_y
    mon_cmd(f"mouse_move {x - cur_x} {y - cur_y}")
    cur_x, cur_y = x, y


# QEMU's sendkey wants SCANCODE NAMES, not characters: `sendkey .` or
# `sendkey /` is an unrecognised name and is silently dropped, which turns a
# typed URL into a different URL rather than into a visible error. Hardening
# this cost one debugging round: "https://10.0.2.2:8443/" arrived as
# "https100228443" and the suite then watched the browser resolve a hostname
# nobody asked for.
KEY_NAMES = {
    " ": "spc",
    "/": "slash",
    ".": "dot",
    ":": "shift-semicolon",
    "-": "minus",
    "_": "shift-minus",
}


def type_keys(keys):
    for k in keys:
        mon_cmd("sendkey " + KEY_NAMES.get(k, k))
        time.sleep(0.1)


def click(x, y, wait=0.4):
    move_abs(x, y)
    time.sleep(0.25)
    mon_cmd("mouse_button 1")
    time.sleep(0.12)
    mon_cmd("mouse_button 0")
    time.sleep(wait)


def open_menu():
    global cur_x, cur_y
    for _ in range(4):
        mon_cmd("mouse_move -127 -127")
        time.sleep(0.2)
    cur_x, cur_y = 0, 0
    move_abs(48, 754)
    time.sleep(0.3)
    mon_cmd("mouse_button 1")
    time.sleep(0.12)
    mon_cmd("mouse_button 0")
    time.sleep(0.6)


def browse(url, timeout):
    """Type `url` into the address bar and wait for the request to finish.

    Returns the serial text after the attempt. The marker is the browser's own
    end-of-request line, which every path emits -- success, TLS refusal and
    timeout alike -- so a missing marker means the request is still in flight.
    """
    # The offset is taken BEFORE the request is triggered, not after. A refusal
    # is quick -- the handshake only has to reach the server's Certificate --
    # so sampling the log once the URL is submitted steps straight past the
    # marker this function exists to wait for, and the caller then sees an
    # empty window and calls a correct refusal a failure.
    start = len(read_log(SERIAL_LOG))
    click(WIN_X0 + 200, URL_FIELD_Y0 + 9, wait=0.5)
    type_keys(["backspace"] * 40)
    type_keys(list(url))
    mon_cmd("sendkey ret")
    time.sleep(0.5)
    move_abs(450, 110)          # park the cursor off the page area
    deadline = time.time() + timeout
    while time.time() < deadline:
        tail = read_log(SERIAL_LOG)[start:]
        if "[BROWSER] done" in tail or "tls-fail" in tail:
            time.sleep(1.5)     # let the final repaint land
            return read_log(SERIAL_LOG)[start:]
        time.sleep(0.5)
    return read_log(SERIAL_LOG)[start:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK, DUMP, TLS_LOG, PLAIN_LOG, HELLO_DUMP,
              FLIGHT_DUMP, KEYLOG):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    tmpdir = tempfile.mkdtemp(prefix="mectov_https_")
    try:
        cert_path, key_path = make_server_identity(tmpdir)
    except ImportError:
        print("[FAIL] python3-cryptography is not installed (needed to make the "
              "test server's chain)")
        return 2

    stop = threading.Event()
    tls_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tls_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tls_sock.bind(("127.0.0.1", TLS_PORT))
    tls_sock.listen(4)

    plain_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    plain_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    plain_sock.bind(("127.0.0.1", PLAIN_PORT))
    plain_sock.listen(4)

    threads = [
        threading.Thread(target=serve_tls,
                         args=(tls_sock, cert_path, key_path, TLS_LOG, stop),
                         daemon=True),
        threading.Thread(target=serve_plain,
                         args=(plain_sock, PLAIN_LOG, stop), daemon=True),
    ]
    for t in threads:
        t.start()
    print(f"[OK] TLS server on {HOST_ALIAS}:{TLS_PORT} (untrusted chain), "
          f"plain control on {HOST_ALIAS}:{PLAIN_PORT}")

    qemu_cmd = [
        "qemu-system-i386",
        "-cpu", "qemu32,+nx",
        "-vga", "std",
        "-cdrom", args.iso,
        "-m", "128",
        "-smp", "4",
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        "-net", "nic,model=rtl8139",
        "-net", "user",
        "-snapshot",
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    failure = None
    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", 90):
            print("[FAIL] kernel never reached login screen")
            return 1
        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.12)
        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] booted + logged in")
        time.sleep(3)   # let the net stack finish its bring-up (DHCP/ARP)

        open_menu()
        row_y = SM_Y + 40 + 3 * 28 + 14   # row 3 = "Mini Browser"
        click(100, row_y, wait=2.5)

        # ---- 1. HTTPS against a chain the image does not trust -------------
        out = browse(f"https://{HOST_ALIAS}:{TLS_PORT}/", 60)
        srv = read_log(TLS_LOG)

        if not re.search(r"first_byte=0x16", srv):
            print("[FAIL] the test server never saw a TLS ClientHello from the guest")
            print("       server log:\n" + (srv or "(empty)"))
            failure = "no ClientHello"
        elif not re.search(r"\[SRV\] sni=", srv):
            print("[FAIL] the ClientHello did not parse on a real server")
            print("       server log:\n" + (srv or "(empty)"))
            failure = "ClientHello rejected"
        elif "[BROWSER] tls-fail" not in out:
            print("[FAIL] the browser did not refuse the untrusted certificate")
            print("       browser said:\n" + (out or "(nothing)"))
            failure = "untrusted certificate accepted"
        elif "not trusted" not in out:
            print("[FAIL] the browser refused, but for the wrong reason:")
            for ln in out.splitlines():
                if "tls-fail" in ln:
                    print("       " + ln.strip())
            failure = "wrong refusal reason"
        elif "[BROWSER] done" in out:
            print("[FAIL] the browser rendered a page from a refused connection")
            failure = "rendered after refusal"
        else:
            print("[OK] ClientHello reached the server and the browser refused "
                  "the untrusted chain")

        # ---- 2. Plain HTTP control, Content-Length framing ----------------
        if failure is None:
            out = browse(f"http://{HOST_ALIAS}:{PLAIN_PORT}/len", 40)
            m = re.search(r"\[BROWSER\] done tls=0 code=(\d+) bytes=(\d+) "
                          r"body=(\d+)", out)
            if not m:
                print("[FAIL] the Content-Length control never completed")
                print("       browser said:\n" + (out or "(nothing)"))
                failure = "content-length control"
            elif int(m.group(1)) != 200:
                print(f"[FAIL] Content-Length control returned code {m.group(1)}")
                failure = "content-length control status"
            elif int(m.group(3)) != len(PLAIN_BODY):
                print(f"[FAIL] Content-Length body was {m.group(3)} bytes, "
                      f"expected {len(PLAIN_BODY)}")
                failure = "content-length body length"
            else:
                print(f"[OK] plain HTTP: 200, body {m.group(3)} bytes "
                      f"(read stopped at Content-Length)")

        # ---- 3. Plain HTTP control, chunked framing -----------------------
        if failure is None:
            out = browse(f"http://{HOST_ALIAS}:{PLAIN_PORT}/chunked", 40)
            m = re.search(r"\[BROWSER\] done tls=0 code=(\d+) bytes=(\d+) "
                          r"body=(\d+)", out)
            if not m:
                print("[FAIL] the chunked control never completed")
                print("       browser said:\n" + (out or "(nothing)"))
                failure = "chunked control"
            elif int(m.group(3)) != len(CHUNK_BODY):
                print(f"[FAIL] chunked body decoded to {m.group(3)} bytes, "
                      f"expected {len(CHUNK_BODY)} (wire was {m.group(2)})")
                failure = "chunked de-framing"
            else:
                print(f"[OK] chunked reply de-framed: wire {m.group(2)} bytes "
                      f"-> body {m.group(3)} bytes")

        if failure is None and "[PANIC]" in read_log(SERIAL_LOG):
            print("[FAIL] kernel panic on the run")
            failure = "panic"
        if failure is None:
            print("[OK] no kernel panic")
        return 0 if failure is None else 1
    finally:
        stop.set()
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
        for s in (tls_sock, plain_sock):
            try:
                s.close()
            except OSError:
                pass
        for t in threads:
            t.join(timeout=3)
        import shutil
        shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
