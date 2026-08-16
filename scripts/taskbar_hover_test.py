#!/usr/bin/env python3
"""
scripts/taskbar_hover_test.py — CI test for taskbar window buttons (v38.39).

Boots mectov.iso in QEMU, logs in, and drives the taskbar window buttons:

Phase A (single Terminal): clicking the focused window's taskbar button
minimizes it (its content region turns into wallpaper) and clicking again
restores it exactly (pixel-compared to the pre-minimize dump).

Phase B (Terminal + Clock): the Clock grabs focus, so the Terminal button is
non-focused. Hovering it turns its title amber — the hover system was dead
code before v38.39 (taskbar_track_mouse was never called, and hover changes
never requested a redraw, so highlights only appeared when some other event
happened to trigger a full redraw). The focused button stays plain, moving
away clears the highlight, and the Start menu's selection bar highlights on
hover (the same dead system).

Exit code 0 = pass, 1 = fail.
"""
import argparse
import os
import socket
import subprocess
import sys
import time

import terminal_launch

SERIAL_LOG = "/tmp/mectov_hover_serial.log"
MON_SOCK = "/tmp/mectov_hover_monitor.sock"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
CLOCK_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s", "slash",
              "c", "l", "o", "c", "k", "dot", "m", "c", "t", "ret"]

# Taskbar geometry on the 1024x768 desktop: bar at y>=740; START x 4..92;
# window buttons start at x=100, 180px wide with a 4px gap.
TY = 740
BTN1 = (100, 740, 280, 768)   # first window button (Terminal)
BTN2 = (284, 740, 464, 768)   # second window button (Clock)
# Terminal content region (window at 60,40 640x400 — same sample as lock_test).
TERM_REGION = (150, 150, 550, 380)


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


def is_taskbar(px, w, h):
    r, g, b = px_at(px, w, 400, h - 14)
    return r < 12 and g < 12 and b < 12


def count_amber(px, w, x0, y0, x1, y1):
    """Amber (TB_ACTIVE 0xE0A94F-ish) pixels in a region."""
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = px[y * w + x]
            if r > 140 and g > 110 and b < 110:
                n += 1
    return n


def region_diff(px1, w1, px2, w2, x0, y0, x1, y1):
    total = n = 0
    for y in range(y0, y1, 4):
        for x in range(x0, x1, 4):
            a, b = px1[y * w1 + x], px2[y * w2 + x]
            total += abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])
            n += 1
    return total / max(n, 1)


def send_login_keys():
    for k in LOGIN_KEYS:
        mon_cmd("sendkey " + k)
        time.sleep(0.12)


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
        if not terminal_launch.launch_terminal(
                mon_cmd, SERIAL_LOG, "/tmp/mectov_hover_cursor1.ppm"):
            print("[FAIL] terminal did not launch")
            return 1
        print("[OK] terminal launched")
        wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30)
        time.sleep(1.0)

        # ---- Phase A: minimize / restore via the taskbar button ----
        # Focus the terminal: cursor at icon (60,64) -> inside window (360,240).
        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)
        screendump("/tmp/mectov_hover_base.ppm")
        w0, h0, px0 = load_ppm("/tmp/mectov_hover_base.ppm")
        if not is_taskbar(px0, w0, h0):
            print("[FAIL] no taskbar in baseline")
            return 1
        print("[OK] desktop reference captured")

        # Click the focused terminal's button (cursor (360,240) -> (190,754)).
        mon_cmd("mouse_move -170 514")
        time.sleep(0.2)
        mon_cmd("mouse_button 1"); time.sleep(0.12); mon_cmd("mouse_button 0")
        time.sleep(0.6)
        screendump("/tmp/mectov_hover_min.ppm")
        wm, hm, pxm = load_ppm("/tmp/mectov_hover_min.ppm")
        d_min = region_diff(px0, w0, pxm, wm, *TERM_REGION)
        print(f"[i] terminal region diff after minimize click: {d_min:.1f}")
        if d_min < 30:
            print("[FAIL] terminal region unchanged — minimize did not happen")
            return 1
        print("[OK] clicking the focused button minimized the terminal")

        # Click again to restore (cursor still on the button).
        mon_cmd("mouse_button 1"); time.sleep(0.12); mon_cmd("mouse_button 0")
        time.sleep(0.6)
        screendump("/tmp/mectov_hover_rest.ppm")
        wr, hr, pxr = load_ppm("/tmp/mectov_hover_rest.ppm")
        d_rest = region_diff(px0, w0, pxr, wr, *TERM_REGION)
        print(f"[i] terminal region diff after restore click: {d_rest:.1f}")
        if d_rest >= 30:
            print("[FAIL] terminal region did not return — restore did not happen")
            return 1
        print("[OK] clicking again restored the terminal")

        # ---- Phase B: hover highlights with two windows ----
        # Open Clock from the terminal shell: the new window grabs focus, so
        # the Terminal button becomes non-focused.
        for k in CLOCK_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.12)
        time.sleep(3.5)
        print("[OK] clock launched (second window)")

        # Move the cursor off the taskbar before the baseline dump.
        # (190,754) -> (360,240): (170,-514)
        mon_cmd("mouse_move 170 -514")
        time.sleep(0.4)
        screendump("/tmp/mectov_hover_b2.ppm")
        wb, hb, pxb = load_ppm("/tmp/mectov_hover_b2.ppm")
        if count_amber(pxb, wb, 4, TY, 464, 768) > 10:
            print("[FAIL] amber visible on taskbar without hover")
            return 1

        def has_title(px, w, region):
            x0, y0, x1, y1 = region
            n = 0
            for y in range(y0 + 6, y1 - 4):
                for x in range(x0 + 20, x1 - 4):
                    r, g, b = px_at(px, w, x, y)
                    if r > 190 and g > 180 and b > 160:
                        n += 1
            return n > 30
        if not has_title(pxb, wb, BTN1) or not has_title(pxb, wb, BTN2):
            print("[FAIL] expected two window buttons on the taskbar")
            return 1
        print("[OK] two window buttons visible, no stray amber")

        # Hover the NON-focused Terminal button: (360,240) -> (190,754).
        mon_cmd("mouse_move -170 514")
        time.sleep(0.4)
        screendump("/tmp/mectov_hover_hov.ppm")
        w1, h1, px1 = load_ppm("/tmp/mectov_hover_hov.ppm")
        a1 = count_amber(px1, w1, *BTN1)
        a2 = count_amber(px1, w1, *BTN2)
        print(f"[i] hover BTN1: amber btn1={a1} btn2={a2}")
        if a1 < 20:
            print("[FAIL] non-focused window button did not highlight on hover")
            return 1
        print("[OK] non-focused window button highlights on hover")
        if a2 > 10:
            print("[FAIL] focused button should not highlight (already active)")
            return 1
        print("[OK] focused window button stays plain (hover suppressed)")

        # Move away: highlight must clear. (190,754) -> (40,254).
        mon_cmd("mouse_move -150 -500")
        time.sleep(0.4)
        screendump("/tmp/mectov_hover_off.ppm")
        w2, h2, px2 = load_ppm("/tmp/mectov_hover_off.ppm")
        off = count_amber(px2, w2, 4, TY, 464, 768)
        print(f"[i] amber after move-away: {off}")
        if off > 10:
            print("[FAIL] hover highlight did not clear after moving away")
            return 1
        print("[OK] hover highlight clears on move-away")

        # ---- Start menu item hover (same dead hover system) ----
        # Open the menu: (40,254) -> START (48,754): (8,500), click.
        mon_cmd("mouse_move 8 500")
        time.sleep(0.3)
        mon_cmd("mouse_button 1"); time.sleep(0.12); mon_cmd("mouse_button 0")
        time.sleep(0.6)
        # Hover item 2: menu 376px tall above the taskbar; items start at
        # sm_y+40 with 28px rows.
        sm_y = (768 - 28) - 376
        item2_y = sm_y + 40 + 2 * 28 + 14
        mon_cmd("mouse_move 52 -%d" % (754 - item2_y))
        time.sleep(0.4)
        screendump("/tmp/mectov_hover_sm.ppm")
        w3, h3, px3 = load_ppm("/tmp/mectov_hover_sm.ppm")
        a_sm = count_amber(px3, w3, 3, item2_y - 14, 201, item2_y + 14)
        print(f"[i] start menu selection bar on item 2: {a_sm}")
        if a_sm < 200:
            print("[FAIL] start menu item did not highlight on hover")
            return 1
        print("[OK] start menu item highlights on hover")

        # Move to item 0 WITHIN the menu (both rows above the taskbar): the
        # bar must follow. Before v38.40 this glitched — taskbar_draw
        # early-returned when the dirty rect stayed above the taskbar strip,
        # so the highlight stayed stuck on the previous row.
        item0_y = sm_y + 40 + 14
        mon_cmd("mouse_move 0 -%d" % (item2_y - item0_y))
        time.sleep(0.4)
        screendump("/tmp/mectov_hover_sm2.ppm")
        w4, h4, px4 = load_ppm("/tmp/mectov_hover_sm2.ppm")
        a0 = count_amber(px4, w4, 3, item0_y - 14, 201, item0_y + 14)
        a2b = count_amber(px4, w4, 3, item2_y - 14, 201, item2_y + 14)
        print(f"[i] after in-menu move: bar0={a0} bar2={a2b}")
        if a0 < 200:
            print("[FAIL] bar did not follow the mouse to item 0 (stuck)")
            return 1
        if a2b > 100:
            print("[FAIL] old bar position did not clear (stuck highlight)")
            return 1
        print("[OK] hover follows the mouse within the menu (no stuck bar)")

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
