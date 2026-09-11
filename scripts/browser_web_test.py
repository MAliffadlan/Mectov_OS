#!/usr/bin/env python3
"""
scripts/browser_web_test.py — CI test for the Mini Browser's hostname path
(v38.71): the full DNS -> port-80 redirect -> Web Gateway Proxy chain.

Boots mectov.iso in QEMU with user-mode networking (slirp), starts the Web
Gateway Proxy (scripts/gateway.py) in DETERMINISTIC FAKE MODE (env
MECTOV_GATEWAY_FAKE_TEXT), logs in, opens the Start menu, launches
"Mini Browser" (menu row 3) and drives it:

  1. Clears the URL field and types the hostname "localhost" — a REAL
     hostname that resolves through the kernel's DNS client (UDP query to
     the slirp DNS server 10.0.2.3, which forwards to the host resolver,
     which answers from /etc/hosts — ZERO internet dependency, so the test
     is deterministic on any machine including CI).
  2. The browser connects to the resolved IP on port 80; the kernel
     redirects port 80 to the Web Gateway Proxy (10.0.2.2:8888).
  3. The proxy (fake mode) answers with a canned page that echoes back the
     Host/Path the guest sent, so we can assert they survived the whole
     trip, and logs "[FAKE] Host: ... Path: ..." to its stdout.
  4. The browser renders the reply; the page area must show several text
     rows and the status bar must be populated.

Assertions: serial log shows the DNS query for "localhost", a resolved A
record, the port-80 redirect and no PANIC/WATCHDOG; the gateway log shows
the fake reply with Host: localhost Path: /; the screendump shows >= 4
text rows and green status-bar text.

Exit code 0 = pass, 1 = fail.
"""
import argparse
import os
import socket
import subprocess
import sys
import time

SERIAL_LOG = "/tmp/mectov_web_serial.log"
MON_SOCK = "/tmp/mectov_web_monitor.sock"
DUMP = "/tmp/mectov_web_page.ppm"
GATEWAY_LOG = "/tmp/mectov_web_gateway.log"

FAKE_PAGE = ("FAKE PAGE — deterministic test page.\\n"
             "This line proves the full guest->gateway chain works.\\n"
             "End of fake page.")

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

SM_Y = (768 - 28) - 404  # Start menu panel top (START_MENU_H = 404 since Pixel Paint)

# Browser window: created at (50,50) 520x380; WM carves a 20px titlebar +
# 1px frame -> client area on screen spans x 51..548, y 71..428 (518x358).
WIN_X0, WIN_Y0 = 50, 50
WIN_X1, WIN_Y1 = 570, 430
URL_FIELD_Y0, URL_FIELD_Y1 = WIN_Y0 + 25, WIN_Y0 + 47   # client 4..26
PAGE_Y0, PAGE_Y1 = WIN_Y0 + 61, WIN_Y0 + 363            # client 40..342
STATUS_Y0, STATUS_Y1 = WIN_Y0 + 363, WIN_Y0 + 379       # client 342..358


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
    """Count 16px text rows with >= 4 dark pixels in the page area."""
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

    for p in (SERIAL_LOG, MON_SOCK, DUMP, GATEWAY_LOG):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    # Kill any stale gateway, then start ours in deterministic fake mode.
    subprocess.run(["pkill", "-f", "gateway.py"], capture_output=True)
    time.sleep(0.5)
    env = dict(os.environ)
    env["MECTOV_GATEWAY_FAKE_TEXT"] = FAKE_PAGE
    gw = subprocess.Popen(["python3", "-u", "scripts/gateway.py"],
                          stdout=open(GATEWAY_LOG, "w"),
                          stderr=subprocess.STDOUT, env=env)
    time.sleep(0.8)

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
        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.12)
        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("FAIL: login did not complete")
            return 1
        print("[OK] booted + logged in")
        time.sleep(3)  # let the net stack finish its bring-up (DHCP/ARP)

        open_menu()
        # Row 3 in the start menu = "Mini Browser"
        row_y = SM_Y + 40 + 3 * 28 + 14
        click(100, row_y, wait=2.5)

        # Click the URL field, clear the default text, type "localhost"
        click(WIN_X0 + 200, URL_FIELD_Y0 + 9, wait=0.5)
        type_keys(["backspace"] * 14)
        type_keys(list("localhost"))
        mon_cmd("sendkey ret")  # start the fetch
        time.sleep(0.5)
        move_abs(450, 110)  # park cursor off the page area

        # Wait for the exchange: DNS resolve -> connect -> gateway reply -> EOF
        if not wait_for_in_file(SERIAL_LOG, "peer closed", 45):
            print("FAIL: guest->gateway exchange never completed")
            return 1
        time.sleep(2.5)
        if not screendump(DUMP):
            print("FAIL: no screendump")
            return 1
        w, h, px = load_ppm(DUMP)

        # 1. The kernel must have done a real DNS query for the hostname
        try:
            with open(SERIAL_LOG, "r", errors="replace") as f:
                s = f.read()
        except FileNotFoundError:
            s = ""
        if "Sending DNS query for domain: localhost" not in s:
            print("FAIL: no DNS query for localhost in serial log")
            return 1
        if "DNS A-record resolved successfully" not in s:
            print("FAIL: DNS never resolved localhost")
            return 1
        if "Redirecting HTTP port 80 to Web Gateway Proxy" not in s:
            print("FAIL: port-80 redirect to gateway did not fire")
            return 1
        print("[OK] DNS query -> resolve -> port-80 redirect chain verified")

        # 2. The gateway (fake mode) must have seen the guest's Host header
        try:
            with open(GATEWAY_LOG) as f:
                g = f.read()
        except FileNotFoundError:
            g = ""
        if "Host: localhost" not in g or "Path: /" not in g:
            print(f"FAIL: gateway never saw the guest request ({g!r})")
            return 1
        if "FAKE" not in g:
            print("FAIL: gateway did not answer in fake mode")
            return 1
        print("[OK] gateway received GET with Host: localhost Path: /")

        # 3. The reply must be rendered in the page area (>= 4 text rows)
        rows = count_dark_rows(px, w)
        if rows < 4:
            print(f"FAIL: page rows={rows} — expected >= 4 (rendered reply)")
            return 1
        print(f"[OK] page rendered parsed ({rows} rows)")

        # 4. Status bar populated (green text on dark strip)
        green = count_color(px, w, WIN_X0, STATUS_Y0, WIN_X1 - 12, STATUS_Y1,
                            (0xA6, 0xE3, 0xA1))
        if green < 10:
            print(f"FAIL: status bar not rendered (green px={green})")
            return 1
        print(f"[OK] status bar rendered (green px={green})")

        # 5. No PANIC/WATCHDOG anywhere
        if "PANIC" in s or "WATCHDOG" in s:
            print("FAIL: PANIC/WATCHDOG in serial log")
            return 1
        print("[OK] no PANIC/WATCHDOG")
        print("PASS: browser hostname -> DNS -> port 80 -> gateway -> render")
        return 0
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()
        gw.terminate()
        try:
            gw.wait(timeout=5)
        except subprocess.TimeoutExpired:
            gw.kill()


if __name__ == "__main__":
    sys.exit(main())