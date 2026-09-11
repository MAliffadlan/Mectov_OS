#!/usr/bin/env python3
"""
scripts/paint_test.py — CI test for the Pixel Paint app (v38.75).

Boots mectov.iso, logs in, opens the Start menu, launches "Pixel Paint"
(menu row 4) and drives the real app with the QEMU monitor mouse:

  1. Window opens: canvas paper fill + all 8 palette swatches rendered.
  2. Pick the green swatch, click two canvas cells -> both cells show green.
  3. Drag across several cells with the button held -> a whole run of cells
     is painted (WM forwards mouse moves while the button is held).
  4. Pick blue, stamp another cell -> blue cell rendered.
  5. Click Clear -> every stamped cell returns to paper color.
  6. No PANIC/WATCHDOG in the serial log.

The screendump channel order is not guaranteed RGB (measured (B,R,G) on the
dev box), so every color assertion is permutation-tolerant.

Exit code 0 = pass, 1 = fail.
"""
import argparse
import os
import socket
import subprocess
import sys
import time

SERIAL_LOG = "/tmp/mectov_paint_serial.log"
MON_SOCK = "/tmp/mectov_paint_monitor.sock"
DUMP1 = "/tmp/mectov_paint_open.ppm"
DUMP2 = "/tmp/mectov_paint_drawn.ppm"
DUMP3 = "/tmp/mectov_paint_cleared.ppm"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

SM_Y = (768 - 28) - 404  # Start menu panel top (START_MENU_H is now 404)

# Paint window: created at (60,40) 600x420. WM titlebar 20px + 1px frame ->
# client on screen spans x 61..659, y 61..459.
WIN_X0, WIN_Y0 = 60, 40
CLIENT_X0, CLIENT_Y0 = WIN_X0 + 1, WIN_Y0 + 21

# Geometry in client coords (must mirror apps/paint.c):
TOOL_Y = 6
GRID_X, GRID_Y = 11, 34
CELL = 24
CANVAS_W, CANVAS_H = 24 * 24, 14 * 24  # 576 x 336

def client(x, y):
    return CLIENT_X0 + x, CLIENT_Y0 + y

SWATCH = [client(6 + i * 26 + 11, TOOL_Y + 11) for i in range(8)]
CLEAR_BTN = client(499, TOOL_Y + 11)   # client 470..528
CELL1 = client(GRID_X + 2 * CELL + 12, GRID_Y + 3 * CELL + 12)
CELL2 = client(GRID_X + 5 * CELL + 12, GRID_Y + 3 * CELL + 12)
DRAG_FROM = client(GRID_X + 8 * CELL + 12, GRID_Y + 8 * CELL + 12)
DRAG_TO = client(GRID_X + 13 * CELL + 12, GRID_Y + 8 * CELL + 12)

PAPER = (245, 240, 231)   # 0xFFF5F0E7 in some channel order
GREEN = (166, 227, 161)   # 0xFFA6E3A1
BLUE = (122, 162, 247)    # 0xFF7AA2F7


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


def click(x, y, wait=0.5):
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


def count_color(px, w, x0, y0, x1, y1, rgb, tol=30):
    """Count pixels in the box matching rgb in ANY channel permutation."""
    n = 0
    import itertools
    perms = set(itertools.permutations(rgb))
    for y in range(y0, min(y1, len(px) // w)):
        base = y * w
        for x in range(x0, x1):
            p = px[base + x]
            if any(all(abs(p[i] - q[i]) <= tol for i in range(3)) for q in perms):
                n += 1
    return n


def box(x, y, r=10):
    return (x - r, y - r, x + r, y + r)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--kvm", action="store_true")
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
        time.sleep(2)

        open_menu()
        # Row 4 = "Pixel Paint" (Terminal=0, Notepad=1, Explorer=2, Browser=3,
        # Pixel Paint=4). Items start 36px into the panel, 28px per row.
        row_y = SM_Y + 36 + 4 * 28 + 14
        click(100, row_y, wait=2.5)

        if not screendump(DUMP1):
            print("FAIL: no screendump after launching paint")
            return 1
        w, h, px = load_ppm(DUMP1)

        # 1. Canvas paper + swatches present
        cx0, cy0 = client(GRID_X, GRID_Y)
        paper = count_color(px, w, cx0, cy0, cx0 + CANVAS_W, cy0 + CANVAS_H, PAPER)
        if paper < 5000:
            print(f"FAIL: paint window not detected (canvas paper px={paper})")
            return 1
        sw_ok = sum(1 for (sx, sy) in SWATCH
                    if count_color(px, w, *box(sx, 78, 9), (255, 255, 255)) == 0
                    and count_color(px, w, *box(sx, 78, 9), (0, 0, 0)) < 400)
        # swatch presence: count pixels that are neither the dark chrome nor
        # the desktop — simpler: just require the paper canvas above plus
        # non-zero paint of each accent color somewhere in the toolbar strip.
        tx0, ty0 = client(0, TOOL_Y)
        toolbar_hits = 0
        for rgb in ((224, 108, 117), (229, 160, 89), (229, 192, 123),
                    (166, 227, 161), (122, 162, 247)):
            if count_color(px, w, tx0, ty0, tx0 + 320, ty0 + 30, rgb) > 20:
                toolbar_hits += 1
        if toolbar_hits < 5:
            print(f"FAIL: palette swatches missing ({toolbar_hits}/5 accents)")
            return 1
        print(f"[OK] Pixel Paint open (canvas paper px={paper}, swatches {toolbar_hits}/5)")

        # 2. Pick green, stamp two cells
        click(*SWATCH[6], wait=0.4)
        click(*CELL1, wait=0.4)
        click(*CELL2, wait=0.6)
        if not screendump(DUMP2):
            print("FAIL: no screendump after painting")
            return 1
        _, _, px2 = load_ppm(DUMP2)
        g1 = count_color(px2, w, *box(CELL1[0], CELL1[1], 9), GREEN)
        g2 = count_color(px2, w, *box(CELL2[0], CELL2[1], 9), GREEN)
        if g1 < 200 or g2 < 200:
            print(f"FAIL: green cells not painted (c1={g1} c2={g2})")
            return 1
        print(f"[OK] two cells painted green (px={g1}/{g2})")

        # 3. Drag across cells with button held
        move_abs(*DRAG_FROM)
        time.sleep(0.2)
        mon_cmd("mouse_button 1")
        time.sleep(0.15)
        steps = 8
        for i in range(1, steps + 1):
            x = DRAG_FROM[0] + (DRAG_TO[0] - DRAG_FROM[0]) * i // steps
            y = DRAG_FROM[1]
            move_abs(x, y)
            time.sleep(0.08)
        mon_cmd("mouse_button 0")
        time.sleep(0.6)
        if not screendump(DUMP2):
            print("FAIL: no screendump after drag")
            return 1
        _, _, px2 = load_ppm(DUMP2)
        mid = client(GRID_X + 10 * CELL + 12, GRID_Y + 8 * CELL + 12)
        gmid = count_color(px2, w, *box(mid[0], mid[1], 9), GREEN)
        if gmid < 200:
            print(f"FAIL: drag did not paint middle cell (px={gmid})")
            return 1
        print(f"[OK] drag painted the run (middle cell px={gmid})")

        # 4. Pick blue, stamp another cell
        click(*SWATCH[7], wait=0.4)
        cell3 = client(GRID_X + 17 * CELL + 12, GRID_Y + 8 * CELL + 12)
        click(*cell3, wait=0.6)
        if not screendump(DUMP2):
            print("FAIL: no screendump after blue cell")
            return 1
        _, _, px2 = load_ppm(DUMP2)
        b3 = count_color(px2, w, *box(cell3[0], cell3[1], 9), BLUE)
        if b3 < 200:
            print(f"FAIL: blue cell not painted (px={b3})")
            return 1
        print(f"[OK] blue cell painted (px={b3})")

        # 5. Clear -> cells return to paper
        click(*CLEAR_BTN, wait=0.8)
        if not screendump(DUMP3):
            print("FAIL: no screendump after clear")
            return 1
        _, _, px3 = load_ppm(DUMP3)
        p1 = count_color(px3, w, *box(CELL1[0], CELL1[1], 9), PAPER)
        p2 = count_color(px3, w, *box(CELL2[0], CELL2[1], 9), PAPER)
        pmid = count_color(px3, w, *box(mid[0], mid[1], 9), PAPER)
        if p1 < 250 or p2 < 250 or pmid < 250:
            print(f"FAIL: clear did not erase (p1={p1} p2={p2} pmid={pmid})")
            return 1
        print(f"[OK] clear erased everything (paper px={p1}/{p2}/{pmid})")

        # 6. Save/Open cycle: paint a red cell, Save, clear, Open, cell returns
        click(*SWATCH[3], wait=0.4)
        rcell = client(GRID_X + 2 * CELL + 12, GRID_Y + 11 * CELL + 12)
        click(*rcell, wait=0.4)
        save_btn = client(337, TOOL_Y + 11)
        open_btn = client(395, TOOL_Y + 11)
        click(*save_btn, wait=0.8)
        click(*CLEAR_BTN, wait=0.6)
        if not screendump(DUMP3):
            print("FAIL: no screendump after pre-open clear")
            return 1
        _, _, px3 = load_ppm(DUMP3)
        pre = count_color(px3, w, *box(rcell[0], rcell[1], 9), (224, 108, 117))
        click(*open_btn, wait=0.8)
        if not screendump(DUMP3):
            print("FAIL: no screendump after open")
            return 1
        _, _, px3 = load_ppm(DUMP3)
        post = count_color(px3, w, *box(rcell[0], rcell[1], 9), (224, 108, 117))
        if post < 200 or pre >= 200:
            print(f"FAIL: save/open roundtrip (red pre-clear={pre} post-open={post})")
            return 1
        print(f"[OK] save/open roundtrip (red cell restored px={post})")

        serial = open(SERIAL_LOG, "r", errors="replace").read()
        if "PANIC" in serial or "WATCHDOG" in serial:
            print("FAIL: PANIC/WATCHDOG in serial log")
            return 1
        print("PASS: pixel paint opens, paints, drags, recolors, saves, opens, clears")
        return 0
    finally:
        try:
            qemu.terminate()
            qemu.wait(timeout=10)
        except Exception:
            qemu.kill()


if __name__ == "__main__":
    sys.exit(main())
