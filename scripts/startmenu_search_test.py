#!/usr/bin/env python3
"""
scripts/startmenu_search_test.py — CI test for Start menu search (v38.40).

Boots mectov.iso in QEMU, logs in, opens the Start menu and drives the
search-as-you-type filter:

  1. typing "Snake" narrows the 12-item menu to a single row and shows the
     query ("Search: Snake", amber) in the menu header.
  2. Enter launches the filtered item — the Snake game loads (`[LOADER]`
     log grows) and the menu closes.
  3. reopening and typing "zzz" shows "No results"; Backspace x3 restores
     all items.
  4. typing "lock" narrows to the two items that actually contain the
     substring (Clock + Lock); Escape clears the query and Escape again
     closes the menu.

Exit code 0 = pass, 1 = fail.
"""
import argparse
import os
import socket
import subprocess
import sys
import time

SERIAL_LOG = "/tmp/mectov_smsearch_serial.log"
MON_SOCK = "/tmp/mectov_smsearch_monitor.sock"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

# Start menu geometry on the 1024x768 desktop: 200x376 panel at x 2..202,
# above the taskbar (ty = 740). Items start at sm_y+40, 28px per row.
SM_Y = (768 - 28) - 376


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


def px_at(px, w, x, y):
    return px[y * w + x]


def count_amber(px, w, x0, y0, x1, y1):
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = px[y * w + x]
            if r > 140 and g > 110 and b < 110:
                n += 1
    return n


def item_row_text(px, w, row):
    """Count bright glyph pixels in menu item row `row` (0-based)."""
    y0 = SM_Y + 40 + row * 28
    n = 0
    for y in range(y0 + 6, y0 + 24):
        for x in range(6, 200):
            r, g, b = px[y * w + x]
            if r > 190 and g > 180 and b > 160:
                n += 1
    return n


def menu_open(px, w, h):
    """Menu panel open = black block above the taskbar at x 100."""
    r, g, b = px_at(px, w, 100, h - 120)
    return r < 12 and g < 12 and b < 12


def send_login_keys():
    for k in LOGIN_KEYS:
        mon_cmd("sendkey " + k)
        time.sleep(0.12)


def type_keys(keys):
    for k in keys:
        mon_cmd("sendkey " + k)
        time.sleep(0.12)


def open_menu():
    # Cursor is wherever the previous step left it; reset to the corner first
    # (idempotent once clamped), then move to the START button and click.
    for _ in range(4):
        mon_cmd("mouse_move -127 -127")
        time.sleep(0.2)
    mon_cmd("mouse_move 48 754")
    time.sleep(0.3)
    mon_cmd("mouse_button 1"); time.sleep(0.12); mon_cmd("mouse_button 0")
    time.sleep(0.6)


def log_size():
    try:
        return os.path.getsize(SERIAL_LOG)
    except OSError:
        return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--kvm", action="store_true",
                    help="run with -enable-kvm (much faster, needs /dev/kvm)")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    qemu_cmd = [
        "qemu-system-i386",
        "-vga", "std",
        "-cdrom", args.iso,
        "-m", "128",
        "-smp", "4",
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        "-net", "none",
        "-snapshot",
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    if args.kvm:
        qemu_cmd.insert(1, "-enable-kvm")
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached login screen")
            return 1
        print("[OK] booted to login screen")

        send_login_keys()
        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] logged in, desktop running")
        time.sleep(1.5)

        # ---- 1. Open menu + type "Snake" -> single filtered row ----
        open_menu()
        screendump("/tmp/mectov_sms_open.ppm")
        w0, h0, px0 = load_ppm("/tmp/mectov_sms_open.ppm")
        if not menu_open(px0, w0, h0):
            print("[FAIL] start menu did not open")
            return 1
        print("[OK] start menu open")

        type_keys(["s", "n", "a", "k", "e"])
        time.sleep(0.6)
        screendump("/tmp/mectov_sms_snake.ppm")
        w1, h1, px1 = load_ppm("/tmp/mectov_sms_snake.ppm")
        # Query shown in amber in the header line (second line of the header).
        # x starts at 36: the amber avatar circle ends at x 34, the query
        # text starts at x 38.
        a_hdr = count_amber(px1, w1, 36, SM_Y + 18, 202, SM_Y + 27)
        print(f"[i] amber search text in header: {a_hdr}")
        if a_hdr < 20:
            print("[FAIL] search query not shown in the menu header")
            return 1
        # Only the Snake Game row should have content; rows 1..3 must be empty.
        r0 = item_row_text(px1, w1, 0)
        r1 = item_row_text(px1, w1, 1)
        r2 = item_row_text(px1, w1, 2)
        print(f"[i] item rows after 'Snake': r0={r0} r1={r1} r2={r2}")
        if r0 < 30:
            print("[FAIL] filtered row 0 has no item text")
            return 1
        if r1 > 20 or r2 > 20:
            print("[FAIL] non-matching items still visible after filter")
            return 1
        print("[OK] 'Snake' filtered the menu to one item + query shown")

        # ---- 2. Enter launches the filtered item (Snake) ----
        size_before = log_size()
        mon_cmd("sendkey ret")
        deadline = time.time() + 30
        launched = False
        while time.time() < deadline:
            if log_size() > size_before:
                with open(SERIAL_LOG, "r", errors="replace") as f:
                    f.seek(size_before)
                    if "[LOADER] start" in f.read():
                        launched = True
                        break
            time.sleep(0.5)
        if not launched:
            print("[FAIL] Enter did not launch the filtered item (Snake)")
            return 1
        print("[OK] Enter launched the filtered item")
        time.sleep(1.0)
        screendump("/tmp/mectov_sms_after_enter.ppm")
        w2, h2, px2 = load_ppm("/tmp/mectov_sms_after_enter.ppm")
        if menu_open(px2, w2, h2):
            print("[FAIL] menu still open after Enter")
            return 1
        print("[OK] menu closed after launch")

        # ---- 3. "zzz" -> No results; Backspace x3 restores all ----
        open_menu()
        type_keys(["z", "z", "z"])
        time.sleep(0.6)
        screendump("/tmp/mectov_sms_none.ppm")
        w3, h3, px3 = load_ppm("/tmp/mectov_sms_none.ppm")
        r0 = item_row_text(px3, w3, 0)
        # "No results" is dim gray (TB_TEXT_DIM), so look for it separately.
        dim = 0
        y0 = SM_Y + 40
        for y in range(y0 + 6, y0 + 24):
            for x in range(80, 130):
                r, g, b = px_at(px3, w3, x, y)
                if 90 < r < 190 and 80 < g < 180 and 70 < b < 170:
                    dim += 1
        print(f"[i] 'zzz': bright row0={r0} dim-text={dim}")
        if r0 > 20:
            print("[FAIL] items still visible for a no-match query")
            return 1
        if dim < 15:
            print("[FAIL] 'No results' message not shown")
            return 1
        print("[OK] no-match query shows 'No results'")

        for _ in range(3):
            mon_cmd("sendkey backspace")
            time.sleep(0.15)
        time.sleep(0.5)
        screendump("/tmp/mectov_sms_restore.ppm")
        w4, h4, px4 = load_ppm("/tmp/mectov_sms_restore.ppm")
        r0 = item_row_text(px4, w4, 0)
        r1 = item_row_text(px4, w4, 1)
        r2 = item_row_text(px4, w4, 2)
        print(f"[i] after backspace x3: r0={r0} r1={r1} r2={r2}")
        if r0 < 30 or r1 < 30 or r2 < 30:
            print("[FAIL] items did not all return after clearing the query")
            return 1
        print("[OK] backspace cleared the query, all items back")

        # ---- 4. "lock" -> Clock + Lock; Escape clears, Escape closes ----
        type_keys(["l", "o", "c", "k"])
        time.sleep(0.6)
        screendump("/tmp/mectov_sms_lock.ppm")
        w5, h5, px5 = load_ppm("/tmp/mectov_sms_lock.ppm")
        r0 = item_row_text(px5, w5, 0)
        r1 = item_row_text(px5, w5, 1)
        r2 = item_row_text(px5, w5, 2)
        print(f"[i] 'lock': r0={r0} r1={r1} r2={r2}")
        if r0 < 30 or r1 < 30 or r2 > 20:
            print("[FAIL] 'lock' should show exactly Clock + Lock (2 rows)")
            return 1
        print("[OK] 'lock' narrowed to the two matching items (Clock, Lock)")

        mon_cmd("sendkey esc")
        time.sleep(0.5)
        screendump("/tmp/mectov_sms_esc1.ppm")
        w6, h6, px6 = load_ppm("/tmp/mectov_sms_esc1.ppm")
        a_hdr2 = count_amber(px6, w6, 36, SM_Y + 18, 202, SM_Y + 27)
        if a_hdr2 > 10:
            print("[FAIL] Escape did not clear the search query")
            return 1
        if not menu_open(px6, w6, h6):
            print("[FAIL] menu closed on first Escape (should only clear query)")
            return 1
        print("[OK] first Escape cleared the query, menu stayed open")

        mon_cmd("sendkey esc")
        time.sleep(0.5)
        screendump("/tmp/mectov_sms_esc2.ppm")
        w7, h7, px7 = load_ppm("/tmp/mectov_sms_esc2.ppm")
        if menu_open(px7, w7, h7):
            print("[FAIL] second Escape did not close the menu")
            return 1
        print("[OK] second Escape closed the menu")

        time.sleep(1)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
