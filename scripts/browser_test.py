#!/usr/bin/env python3
"""
scripts/browser_test.py — CI test for the Ring 3 Mini Browser (v38.70).

Boots mectov.iso in QEMU with user-mode networking (slirp), runs a tiny
deterministic HTTP server on the host, logs in, opens the Start menu,
launches "Mini Browser" (menu row 3) and drives the real app:

  1. Types the IP-literal URL "10.0.2.2:<port>" (slirp host gateway) and
     presses ENTER — the browser must skip DNS and connect directly.
  2. The server asserts the request is a well-formed HTTP fetch:
     GET / with Host: 10.0.2.2:<port> and User-Agent: MectovBrowser/3.0
  3. After the response arrives the page area must show the PARSED body —
     the raw response (HTTP headers + HTML source) would fill >= 15 text
     rows, the rendered page fills at most 12.
  4. The status bar (green text on the dark strip) must be populated.

Exit code 0 = pass, 1 = fail.
"""
import argparse
import http.server
import os
import socket
import subprocess
import sys
import threading
import time

SERIAL_LOG = "/tmp/mectov_browser_serial.log"
MON_SOCK = "/tmp/mectov_browser_monitor.sock"
DUMP1 = "/tmp/mectov_browser_typed.ppm"
DUMP2 = "/tmp/mectov_browser_page.ppm"
SERVER_LOG = "/tmp/mectov_browser_http.log"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

SM_Y = (768 - 28) - 404  # Start menu panel top (START_MENU_H = 404 since Pixel Paint)

# Browser window: created at (50,50) 520x380, TITLEBAR_H = 20 -> client area
# Window is created at (50,50) sized 520x380. The WM carves a 20px titlebar
# plus a 1px frame on every side, so the client area on screen spans
# x 51..548, y 71..428 (518x358). The status strip is the last 16 client rows
# (client y 342..357 -> screen y 413..428).
WIN_X0, WIN_Y0 = 50, 50
WIN_X1, WIN_Y1 = 570, 430
URL_FIELD_Y0, URL_FIELD_Y1 = WIN_Y0 + 25, WIN_Y0 + 47   # client 4..26
PAGE_Y0, PAGE_Y1 = WIN_Y0 + 61, WIN_Y0 + 363            # client 40..342
STATUS_Y0, STATUS_Y1 = WIN_Y0 + 363, WIN_Y0 + 379       # client 342..358

PAGE_HTML = b"""<html><head><title>Mectov Test Page</title></head>
<body>
<h1>Mectov HTTP Test</h1>
<p>This paragraph is long enough to wrap into several display rows after the
HTML tags are stripped by the mini browser renderer. ABCDEFG 0123456789.</p>
<p>Entities: A &amp; B &lt;ok&gt;.</p>
<a href="/">a link</a>
</body>
</html>
"""


class TestHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        with open(SERVER_LOG, "a") as f:
            f.write(fmt % args + "\n")

    def do_GET(self):
        with open(SERVER_LOG, "a") as f:
            f.write("METHOD=%s PATH=%s\n" % (self.command, self.path))
            f.write("HOST=%s\n" % self.headers.get("Host", ""))
            f.write("UA=%s\n" % self.headers.get("User-Agent", ""))
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(PAGE_HTML)))
        self.end_headers()
        self.wfile.write(PAGE_HTML)


def start_server(port):
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), TestHandler)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    return srv


def find_free_port():
    for p in range(8317, 8330):
        try:
            s = socket.socket()
            s.bind(("127.0.0.1", p))
            s.close()
            return p
        except OSError:
            continue
    raise RuntimeError("no free test port")


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


def mon_cmd(cmd, wait=0.15):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(wait)
        s.close()
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")


def screendump(path):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    mon_cmd(f"screendump {path}")
    deadline = time.time() + 5
    while time.time() < deadline:
        if os.path.exists(path):
            return True
        time.sleep(0.2)
    return False


def load_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = map(int, line.split())
        f.readline()
        data = f.read()
    px = []
    for i in range(0, len(data), 3):
        px.append((data[i], data[i + 1], data[i + 2]))
    return w, h, px


cur_x, cur_y = 0, 0


def move_abs(x, y):
    global cur_x, cur_y
    mon_cmd(f"mouse_move {x - cur_x} {y - cur_y}")
    cur_x, cur_y = x, y


def send_login_keys():
    for k in LOGIN_KEYS:
        mon_cmd("sendkey " + k)
        time.sleep(0.12)


def type_keys(keys):
    for k in keys:
        mon_cmd("sendkey " + k)
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
    move_abs(48, 754)  # START button
    time.sleep(0.3)
    mon_cmd("mouse_button 1")
    time.sleep(0.12)
    mon_cmd("mouse_button 0")
    time.sleep(0.6)


def count_dark_rows(px, w):
    """Count 16px text rows with >=4 dark pixels in the page area."""
    rows = 0
    y = PAGE_Y0
    while y + 16 <= PAGE_Y1:
        n = 0
        for yy in range(y, y + 16, 2):
            for x in range(WIN_X0 + 8, WIN_X1 - 20, 2):
                p = px[yy * w + x]
                if p[0] < 90 and p[1] < 90 and p[2] < 90:
                    n += 1
        if n >= 4:
            rows += 1
        y += 16
    return rows


def count_color(px, w, x0, y0, x1, y1, rgb, tol=25):
    # The screendump channel order is not guaranteed RGB (measured (B,R,G) on
    # this setup), so accept any permutation of the target channels.
    perms = {(rgb[0], rgb[1], rgb[2]), (rgb[0], rgb[2], rgb[1]),
             (rgb[1], rgb[0], rgb[2]), (rgb[1], rgb[2], rgb[0]),
             (rgb[2], rgb[0], rgb[1]), (rgb[2], rgb[1], rgb[0])}
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            p = px[y * w + x]
            for t in perms:
                if (abs(p[0] - t[0]) <= tol and abs(p[1] - t[1]) <= tol
                        and abs(p[2] - t[2]) <= tol):
                    n += 1
                    break
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--kvm", action="store_true")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK, DUMP1, DUMP2, SERVER_LOG):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    port = find_free_port()
    srv = start_server(port)
    print(f"[*] test HTTP server on 127.0.0.1:{port}")

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
    ]
    if args.kvm:
        qemu_cmd += ["-enable-kvm"]

    qemu = subprocess.Popen(
        qemu_cmd + ["-monitor", f"unix:{MON_SOCK},server,nowait"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", 90):
            print("FAIL: kernel never reached login screen")
            return 1
        send_login_keys()
        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("FAIL: login did not complete")
            return 1
        print("[OK] booted + logged in")
        time.sleep(3)  # let the net stack finish its bring-up (DHCP/ARP)

        open_menu()
        # Row 3 in the start menu = "Mini Browser"
        # (Terminal=0, Notepad=1, File Explorer=2, Mini Browser=3)
        row_y = SM_Y + 40 + 3 * 28 + 14
        click(100, row_y, wait=2.5)

        if not screendump(DUMP1):
            print("FAIL: no screendump after launching browser")
            return 1
        w, h, px = load_ppm(DUMP1)

        # URL field must show the default text (dark pixels in the field)
        url_dark = count_color(px, w, WIN_X0 + 44, URL_FIELD_Y0, WIN_X1 - 6, URL_FIELD_Y1,
                               (0x11, 0x11, 0x11), tol=40)
        if url_dark < 30:
            print(f"FAIL: browser window not detected (url field px={url_dark})")
            return 1
        print(f"[OK] Mini Browser window open (url field px={url_dark})")

        # Click the URL field, clear the default text, type the IP-literal URL
        click(WIN_X0 + 200, URL_FIELD_Y0 + 9, wait=0.5)
        type_keys(["backspace"] * 14)
        # Type the actual URL (port is dynamic — find_free_port may skip 8317
        # if it is still in TIME_WAIT from a previous run).
        keys = []
        for ch in f"10.0.2.2:{port}":
            if ch == ".":
                keys.append("dot")
            elif ch == ":":
                keys.append("shift-semicolon")
            else:
                keys.append(ch)
        type_keys(keys)
        if not screendump(DUMP1):
            print("FAIL: no screendump after typing URL")
            return 1
        w, h, px = load_ppm(DUMP1)
        typed_dark = count_color(px, w, WIN_X0 + 44, URL_FIELD_Y0, WIN_X1 - 6, URL_FIELD_Y1,
                                 (0x11, 0x11, 0x11), tol=40)
        print(f"[*] URL field px after typing: {typed_dark}")

        mon_cmd("sendkey ret")  # start the fetch
        time.sleep(0.5)
        move_abs(450, 110)  # park cursor off the page area
        time.sleep(0.2)

        # Poll until the page content changes from the welcome screen
        rows = -1
        deadline = time.time() + 25
        while time.time() < deadline:
            if not screendump(DUMP2):
                continue
            w, h, px = load_ppm(DUMP2)
            rows = count_dark_rows(px, w)
            if rows >= 4:  # welcome screen shows 3 rows
                break
            time.sleep(1.0)

        print(f"[*] page text rows: {rows}")

        # 1. Fetch really happened (server saw a well-formed request)
        try:
            with open(SERVER_LOG) as f:
                srv_log = f.read()
        except FileNotFoundError:
            srv_log = ""
        got_get = f"METHOD=GET PATH=/" in srv_log
        got_host = f"HOST=10.0.2.2:{port}" in srv_log
        got_ua = "MectovBrowser" in srv_log
        if not (got_get and got_host and got_ua):
            print(f"FAIL: request log incomplete (GET={got_get} HOST={got_host} UA={got_ua})")
            print(srv_log or "(server saw no request)")
            return 1
        print("[OK] server received GET / with correct Host + User-Agent")

        # 2. Body rendered — parsed output is small; raw (unparsed) is >= 15 rows
        if rows < 4 or rows > 12:
            print(f"FAIL: page rows={rows} — expected 4..12 "
                  f"(raw/unparsed render would be >=15)")
            return 1
        print(f"[OK] page rendered parsed (rows={rows})")

        # 3. Status bar populated (green text on dark strip)
        w, h, px = load_ppm(DUMP2)
        green = count_color(px, w, WIN_X0, STATUS_Y0, WIN_X1 - 12, STATUS_Y1,
                            (0xA6, 0xE3, 0xA1))
        if green < 10:
            print(f"FAIL: status bar not rendered (green px={green})")
            return 1
        print(f"[OK] status bar rendered (green px={green})")

        # 4. IP-literal URL must have skipped the DNS path entirely
        try:
            with open(SERIAL_LOG, "r", errors="replace") as f:
                s = f.read()
            if "Sending DNS query" in s:
                print("FAIL: DNS query sent for an IP-literal URL")
                return 1
            if "PANIC" in s or "WATCHDOG" in s:
                print("FAIL: PANIC/WATCHDOG in serial log")
                return 1
        except FileNotFoundError:
            pass
        print("[OK] IP-literal URL skipped DNS; no PANIC/WATCHDOG")
        print("PASS: browser fetched + rendered over real HTTP")
        return 0
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()
        srv.shutdown()


if __name__ == "__main__":
    sys.exit(main())
