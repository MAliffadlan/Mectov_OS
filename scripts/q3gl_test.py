#!/usr/bin/env python3
"""
scripts/q3gl_test.py — end-to-end VISUAL test for the v38.102 TinyGL port
(Q3 phase 2).

Boots the Q3 ISO, logs in, launches the Terminal, types `q3gl` and asserts:

  1. serial: [Q3GL] window id= / scene ready / frame=N markers — the TinyGL
     task opened its WM window and is producing frames on a live timer.
  2. screendump pixels: the gears window shows REAL rendered geometry —
     three distinct gear body colors (blue/red/green from the demo's
     materials) plus highlight pixels (lit specular edges). Pure
     letterbox/no-render (all dark) fails. The color axes are asymmetric
     (blue/bred > br/g) to reject a composited flat desktop as a false
     positive. Screendumps come from the QEMU monitor, so the assertion is
     against the actual composited desktop — the same pixels a user sees.

The first two captures land after ~15 frames of rotation; rotation continues
while the asserts run, so two later frames must DIFFER (the scene is
animated, not a frozen framebuffer).

Usage:
    python3 scripts/q3gl_test.py [--timeout 420] [--iso mectov-q3.iso]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

import terminal_launch

SERIAL_LOG = "/tmp/mectov_q3gl_serial.log"
MON_SOCK = "/tmp/mectov_q3gl_monitor.sock"
SHOT1 = "/tmp/q3gl_shot1.ppm"
SHOT2 = "/tmp/q3gl_shot2.ppm"
SHOT3 = "/tmp/q3gl_shot3.ppm"
CURSOR_PPM = "/tmp/mectov_q3gl_cursor.ppm"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
Q3GL_KEYS = ["q", "3", "g", "l", "ret"]

READY_MARKER = "scene ready"
FRAME_MARKER = "[Q3GL] frame="


def read_file(path):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read()
    except (FileNotFoundError, OSError):
        return ""


def wait_for_in_file(path, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if needle in read_file(path):
            return True
        time.sleep(1)
    return False


def mon_cmd(cmd, settle=None):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(0.15 if settle is None else settle)
        s.close()
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")


def type_line(keys, retries=3, ready_marker=None, timeout=60):
    for _ in range(retries):
        for _ in range(24):
            mon_cmd("sendkey backspace")
        for k in keys:
            mon_cmd("sendkey " + k)
            time.sleep(0.12)
        mon_cmd("sendkey ret")
        if wait_for_in_file(SERIAL_LOG, ready_marker, timeout):
            return True
        time.sleep(1.0)
    return False


def screendump(path, settle=1.0):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    mon_cmd(f"screendump {path}", settle=settle)
    return os.path.exists(path) and os.path.getsize(path) > 1000


def load_ppm_pixels(path):
    """Return (width, height, bytes RGB) for a P6 ppm.

    QEMU's screendump header is exactly `P6\n<w> <h>\n255\n` — but parse it
    robustly (whitespace-separated tokens after P6) instead of relying on a
    fixed line count.
    """
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError(f"not a P6 ppm: {path}")
    pos = 2
    vals = []
    while len(vals) < 3:
        # skip whitespace and comments
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if pos < len(data) and data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos] != 0x0A:
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        vals.append(int(data[start:pos]))
    w, h, maxval = vals
    pos += 1  # single whitespace after maxval
    payload = data[pos:pos + w * h * 3]
    return w, h, payload


def assert_gears_pixels(path):
    """(ok, detail): the shot must contain rendered gear-like content.

    Center-window strip (where the gears window lands) must show gear body
    colors and lit highlights. The axes are asymmetric so a flat desktop
    (wallpaper gradient or terminal black) cannot pass by accident.
    """
    w, h, px = load_ppm_pixels(path)
    x0, x1 = w // 4, 3 * w // 4
    y0, y1 = h // 4, 3 * h // 4
    blue = red = green = 0
    bright = 0
    sample = 0
    for y in range(y0, y1, 2):
        base = (y * w) * 3
        for x in range(x0, x1, 2):
            o = base + x * 3
            r, g, b = px[o], px[o + 1], px[o + 2]
            sample += 1
            if b > r + 50 and b > g + 40:
                blue += 1
            elif r > b + 50 and r > g + 40:
                red += 1
            elif g > r + 40 and g > b + 40:
                green += 1
            if r + g + b > 560:
                bright += 1
    detail = (f"blue={blue} red={red} green={green} bright={bright} "
              f"(sampled {sample})")
    # Gear green sweeps across poses (sometimes mostly hidden behind the
    # large red gear), so its floor is lower; blue+red dominate the scene.
    ok = blue >= 200 and red >= 100 and green >= 60 and bright >= 150
    return ok, detail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=420)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK, SHOT1, SHOT2, SHOT3):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    qemu = subprocess.Popen([
        "qemu-system-i386",
        "-cpu", "qemu32,+nx",
        "-vga", "std",
        "-cdrom", args.iso,
        "-m", "512",
        "-smp", "2",
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        "-net", "none",
        "-snapshot",
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached login screen")
            for line in read_file(SERIAL_LOG).splitlines()[-25:]:
                print(line[:130])
            return 1
        print("[OK] booted to login screen")

        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)

        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] logged in, desktop running")

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(
                mon_cmd, SERIAL_LOG, CURSOR_PPM):
            print("[FAIL] terminal did not launch (icon double-click missed?)")
            return 1
        print("[OK] terminal launched")
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] terminal never became ready")
            return 1
        time.sleep(1.0)

        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)

        if not type_line(Q3GL_KEYS, retries=2, ready_marker=READY_MARKER,
                         timeout=60):
            print("[FAIL] q3gl never reported scene ready")
            for line in read_file(SERIAL_LOG).splitlines()[-30:]:
                print(line[:130])
            return 1
        print("[OK] q3gl scene ready (TinyGL context + WM window)")

        if not wait_for_in_file(SERIAL_LOG, FRAME_MARKER, 60):
            print("[FAIL] no [Q3GL] frame= markers (render loop not ticking)")
            return 1
        print("[OK] render loop ticking")

        # Frame 1 for the pixel assertion — give the scene a few seconds of
        # rotation so the gears sweep through a representative pose.
        time.sleep(8)
        if not screendump(SHOT1):
            print("[FAIL] screendump 1 failed")
            return 1
        ok, detail = assert_gears_pixels(SHOT1)
        print(f"     shot1: {detail}")
        if not ok:
            print("[FAIL] shot1 does not show rendered gears")
            return 1
        print("[OK] gears window shows rendered geometry (blue/red/green bodies)")

        # Animation: two shots ~3s apart must differ in the window region.
        time.sleep(3)
        if not screendump(SHOT2):
            print("[FAIL] screendump 2 failed")
            return 1
        time.sleep(3)
        if not screendump(SHOT3):
            print("[FAIL] screendump 3 failed")
            return 1
        _, _, p2 = load_ppm_pixels(SHOT2)
        _, _, p3 = load_ppm_pixels(SHOT3)
        # Sample the window band every ~100 bytes (~33 px): 6s of rotation
        # repaints hundreds of pixels, while clock/taskbar noise alone stays
        # well under that. Threshold 80 ≈ 25+ changing pixels.
        diff = sum(1 for a, b in zip(p2[::99], p3[::99]) if a != b)
        if diff < 80:
            print(f"[FAIL] scene appears frozen (sampled diff={diff})")
            return 1
        print(f"[OK] scene is animated (sampled diff={diff})")

        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive with the TinyGL gears running")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
