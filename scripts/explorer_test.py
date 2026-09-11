#!/usr/bin/env python3
"""
scripts/explorer_test.py — CI test for the Ring 3 File Explorer (v38.69).

Boots mectov.iso in QEMU, logs in, opens the Start menu, launches
"File Explorer" (menu row 2) and drives the real app:

  1. Explorer window renders: dark list panel with folder icons
     (0x89B4FA) and bright text rows (0xCDD6F4) in the file list area.
  2. Clicking the first row (the seeded "home" directory) navigates
     into it — /home lists exactly one entry (user/).
  3. Clicking again drills into /home/user, an empty directory — the
     list clears to zero rows.
  4. The [<] back button (twice) returns to the root listing.

Exit code 0 = pass, 1 = fail.
"""
import argparse
import os
import socket
import subprocess
import sys
import time

SERIAL_LOG = "/tmp/mectov_explorer_serial.log"
MON_SOCK = "/tmp/mectov_explorer_monitor.sock"
DUMP1 = "/tmp/mectov_explorer_root.ppm"
DUMP2 = "/tmp/mectov_explorer_apps.ppm"
DUMP3 = "/tmp/mectov_explorer_back.ppm"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

SM_Y = (768 - 28) - 404  # Start menu panel top (START_MENU_H = 404 since Pixel Paint)

# Explorer window: requested at (100, 80) 400x340, TITLEBAR_H = 20 (theme.h),
# so the client area spans x 100..500, y 100..420.
WIN_X0, WIN_Y0 = 100, 100
WIN_X1, WIN_Y1 = 500, 420
# File list rows start at app-y 66 -> screen y 166; 22px per row.
ROW0_Y = WIN_Y0 + 66
ROW_H = 22
# Back button at app (6,3) 28x18 -> screen (106,103)..(134,121).
BACK_CX, BACK_CY = WIN_X0 + 6 + 14, WIN_Y0 + 3 + 9


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


# QEMU's monitor `mouse_move` is RELATIVE (dx dy). Track the cursor
# position ourselves and translate absolute screen coords into deltas.
cur_x, cur_y = 0, 0


def move_abs(x, y):
    global cur_x, cur_y
    mon_cmd(f"mouse_move {x - cur_x} {y - cur_y}")
    cur_x, cur_y = x, y


def send_login_keys():
    for k in LOGIN_KEYS:
        mon_cmd("sendkey " + k)
        time.sleep(0.12)


def open_menu():
    global cur_x, cur_y
    for _ in range(4):
        mon_cmd("mouse_move -127 -127")
        time.sleep(0.2)
    cur_x, cur_y = 0, 0  # clamped to the top-left corner
    move_abs(48, 754)    # START button (top-left corner -> taskbar)
    time.sleep(0.3)
    mon_cmd("mouse_button 1"); time.sleep(0.12); mon_cmd("mouse_button 0")
    time.sleep(0.6)


def click(x, y, wait=0.4):
    move_abs(x, y)
    time.sleep(0.25)
    mon_cmd("mouse_button 1"); time.sleep(0.12); mon_cmd("mouse_button 0")
    time.sleep(wait)
    # Park the cursor over the window header's right edge (over the "Ring 3"
    # badge, between the buttons) so it never covers the file list in the
    # screendump.
    move_abs(450, 110)
    time.sleep(0.2)


def count_color(px, w, x0, y0, x1, y1, rgb, tol=12):
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = px[y * w + x]
            if abs(r - rgb[0]) <= tol and abs(g - rgb[1]) <= tol and abs(b - rgb[2]) <= tol:
                n += 1
    return n


def count_list_rows(px, w):
    """Count file-list rows (y 166..408, 22px each) that have any icon or
    bright-text pixels — a row is "present" if its center band shows one."""
    rows = 0
    for r in range(11):
        y = ROW0_Y + r * ROW_H
        n = 0
        for yy in range(y + 6, y + 16):
            for x in range(106, 490):
                p = px[yy * w + x]
                if (p[0] > 190 and p[1] > 190 and p[2] > 160) or \
                   abs(p[0] - 0x89) <= 16 and abs(p[1] - 0xB4) <= 16 and abs(p[2] - 0xFA) <= 16 or \
                   abs(p[0] - 0xFF) <= 16 and abs(p[1] - 0xBB) <= 16 and abs(p[2] - 0x55) <= 16:
                    n += 1
        if n >= 4:
            rows += 1
    return rows


FOLDER_BLUE = (0x89, 0xB4, 0xFA)
FILE_ORANGE = (0xFF, 0xBB, 0x55)
TEXT_BRIGHT = (0xCD, 0xD6, 0xF4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--kvm", action="store_true",
                    help="run with -enable-kvm (much faster, needs /dev/kvm)")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK, DUMP1, DUMP2, DUMP3):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    qemu_cmd = [
        "qemu-system-i386",
        "-cpu", "qemu32,+nx",
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
        time.sleep(1.5)

        open_menu()
        # Row 2 in the start menu = "File Explorer" (Terminal=0, Notepad=1).
        row_y = SM_Y + 40 + 2 * 28 + 14
        click(100, row_y, wait=2.5)

        if not screendump(DUMP1):
            print("FAIL: screendump 1 (root listing) did not appear")
            return 1
        w, h, px = load_ppm(DUMP1)

        folders = count_color(px, w, WIN_X0, ROW0_Y, WIN_X1, WIN_Y1, FOLDER_BLUE)
        textpx = count_color(px, w, WIN_X0, ROW0_Y, WIN_X1, WIN_Y1, TEXT_BRIGHT, tol=30)
        root_rows = count_list_rows(px, w)
        if folders < 40 or textpx < 200 or root_rows < 4:
            print(f"FAIL: explorer root view missing (folders={folders}, text={textpx}, rows={root_rows})")
            return 1
        print(f"OK: explorer root listing rendered ({root_rows} rows, folder px={folders})")

        # Click the first row ("home" dir, seeded first at root) -> /home,
        # which contains exactly one subdirectory (user/).
        click(WIN_X0 + 200, ROW0_Y + ROW_H // 2, wait=1.5)
        if not screendump(DUMP2):
            print("FAIL: screendump 2 (/home) did not appear")
            return 1
        w, h, px = load_ppm(DUMP2)
        home_rows = count_list_rows(px, w)
        if home_rows != 1:
            print(f"FAIL: /home should list exactly 1 entry (user/) got rows={home_rows}")
            return 1
        print(f"OK: navigated into /home — single entry (user/) rendered")

        # Click that entry -> /home/user, an empty directory.
        click(WIN_X0 + 200, ROW0_Y + ROW_H // 2, wait=1.5)
        if not screendump(DUMP3):
            print("FAIL: screendump 3 (/home/user) did not appear")
            return 1
        w, h, px = load_ppm(DUMP3)
        empty_rows = count_list_rows(px, w)
        if empty_rows != 0:
            print(f"FAIL: /home/user should be empty, got rows={empty_rows}")
            return 1
        print("OK: navigated into empty /home/user — list cleared")

        # Back button twice -> root again.
        click(BACK_CX, BACK_CY, wait=1.2)
        click(BACK_CX, BACK_CY, wait=1.2)
        if not screendump(DUMP3):
            print("FAIL: screendump 4 (back to root) did not appear")
            return 1
        w, h, px = load_ppm(DUMP3)
        back_rows = count_list_rows(px, w)
        if back_rows < 4:
            print(f"FAIL: back button did not return to root (rows={back_rows})")
            return 1
        print(f"OK: back button returned to root ({back_rows} rows)")

        # No panic anywhere in the log.
        with open(SERIAL_LOG, "r", errors="replace") as f:
            log = f.read()
        for bad in ("PANIC", "KERNEL STACK OVERFLOW", "watchdog"):
            if bad.lower() in log.lower():
                print(f"FAIL: '{bad}' found in serial log")
                return 1
        print("PASS: File Explorer smoke (launch + navigate + back, no panic)")
        return 0
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()


if __name__ == "__main__":
    sys.exit(main())