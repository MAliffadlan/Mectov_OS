#!/usr/bin/env python3
"""
q3shot.py — boot the Q3 ISO, launch `q3arena <map>`, and save a real
screendump of the composited desktop so the pixels can be LOOKED at.

The suites all judge the renderer from its own histogram, which counts colour
families but says nothing about whether the window shows them. This one exists
for the questions a histogram cannot answer: "is the window black?", "is the
world upside down?", "is the HUD in the right place?". It asserts nothing — it
produces a PNG and a text summary.

    python3 scripts/q3shot.py                       # q3dm1, spawn, free walk
    python3 scripts/q3shot.py --map q3dm1 @318,2253,56,-45
    python3 scripts/q3shot.py --after 120           # let it walk, then shoot
    python3 scripts/q3shot.py --nocull --nosky --nohud
"""

import argparse
import os
import re
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import terminal_launch  # noqa: E402

SERIAL_LOG = "/tmp/mectov_q3shot_serial.log"
MON_SOCK = "/tmp/mectov_q3shot_monitor.sock"
CURSOR_PPM = "/tmp/mectov_q3shot_cursor.ppm"
SHOT_PPM = "/tmp/mectov_q3shot.ppm"
SHOT_PNG = os.path.expanduser("~/mectov_q3shot.png")

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

# QEMU sendkey takes key NAMES: '@' is shift-2, ',' is comma, '-' is minus,
# a space is spc. Same idiom as q3heavy_test.py / q3sky_test.py.
_PUNCT = {"@": ["shift-2"], ",": ["comma"], "-": ["minus"], " ": ["spc"]}


def keys_for(text):
    out = []
    for ch in text:
        out.extend(_PUNCT.get(ch, list(ch)))
    return out


def read_file(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for_in_file(path, needle, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if needle in read_file(path):
            return True
        time.sleep(0.5)
    return False


def mon_cmd(cmd, settle=None):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(0.05)
        s.recv(65536)
        s.close()
    except OSError:
        pass
    if settle:
        time.sleep(settle)


def sendkey(key):
    mon_cmd("sendkey " + key)


def screendump(path=SHOT_PPM):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    mon_cmd(f"screendump {path}")
    for _ in range(30):
        if os.path.exists(path) and os.path.getsize(path) > 64:
            return True
        time.sleep(0.3)
    return False


def load_ppm(path):
    """Minimal binary P6 reader. Returns (w, h, rows-of-rgb-bytes)."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError(f"not a P6 PPM: {data[:8]!r}")
    # header: P6 <w> <h> <maxval> then a single whitespace byte
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":                     # comment
            while i < len(data) and data[i:i + 1] != b"\n":
                i += 1
            continue
        start = i
        while i < len(data) and not data[i:i + 1].isspace():
            i += 1
        fields.append(int(data[start:i]))
    i += 1
    w, h, _ = fields
    return w, h, data[i:i + w * h * 3]


def describe(path):
    w, h, rgb = load_ppm(path)
    px = w * h
    black = near_black = white = mid = 0
    # a 4-bit-per-channel distinct count, same shape the renderer uses
    seen = set()
    for i in range(0, px, max(1, px // 40000)):
        o = i * 3
        r, g, b = rgb[o], rgb[o + 1], rgb[o + 2]
        if r < 12 and g < 12 and b < 12:
            black += 1
        elif r > 235 and g > 235 and b > 235:
            white += 1
        else:
            mid += 1
        if r > 240 and g > 240 and b > 240:
            near_black += 1
        seen.add(((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4))
    n = len(range(0, px, max(1, px // 40000)))
    print(f"[q3shot] dump {w}x{h} -> {path}")
    print(f"         sampled {n} px: black {100*black//n}%  "
          f"mid {100*mid//n}%  white {100*white//n}%  "
          f"distinct(4bit) {len(seen)}")
    try:
        from PIL import Image
        Image.open(path).save(SHOT_PNG)
        print(f"         PNG written to {SHOT_PNG}")
    except ImportError:
        print("         (Pillow not installed — no PNG; the PPM is viewable "
              "with any image viewer)")
    return w, h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--map", default="q3dm1")
    ap.add_argument("--pose", default="", help="@x,y,z,yaw[,pitch] pin")
    ap.add_argument("--after", type=int, default=0,
                    help="seconds to let the demo walk before the dump")
    ap.add_argument("--kvm", action="store_true")
    for flag in ("nocull", "nosky", "nohud", "nolimit"):
        ap.add_argument(f"--{flag}", action="store_true")
    args = ap.parse_args()

    staged = f"build/q3data/baseq3/maps/{args.map}.bsp"
    if not os.path.exists(staged):
        print(f"[SKIP] {staged} not staged")
        return 0

    for p in (SERIAL_LOG, MON_SOCK, CURSOR_PPM, SHOT_PPM):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    qemu_cmd = [
        "qemu-system-i386",
        *(["-cpu", "host"] if args.kvm else ["-cpu", "qemu32,+nx"]),
        "-vga", "std", "-cdrom", args.iso, "-m", "512", "-smp", "2",
        "-display", "none", "-serial", f"file:{SERIAL_LOG}", "-net", "none",
        "-snapshot",
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    if args.kvm:
        qemu_cmd.insert(1, "-enable-kvm")
    print(f"[q3shot] {'KVM' if args.kvm else 'TCG'}  map={args.map} "
          f"pose={args.pose or '(demo walk)'}")
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] never reached login")
            return 1
        for k in LOGIN_KEYS:
            sendkey(k)
            time.sleep(0.15)
        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] logged in")

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(mon_cmd, SERIAL_LOG, CURSOR_PPM):
            print("[FAIL] terminal never ready")
            return 1
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] terminal never ready")
            return 1
        time.sleep(1.0)
        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)

        cmdline = "q3arena " + args.map
        for flag, on in (("nocull", args.nocull), ("nosky", args.nosky),
                         ("nohud", args.nohud),
                         ("nolimit", args.nolimit)):
            if on:
                cmdline += " " + flag
        if args.pose:
            cmdline += " " + args.pose
        print(f"[q3shot] typing: {cmdline}")

        keys = keys_for(cmdline) + ["ret"]
        for _ in range(len(keys) + 8):
            sendkey("backspace")
        for k in keys:
            sendkey(k)
            time.sleep(0.12)

        # Time the phases the user actually waits through. The load is 94
        # hand-decoded textures plus the bytecode's own init, and it runs BEFORE
        # the first frame — long enough that "still loading" is
        # indistinguishable from "frozen" if nothing says so.
        t_typed = time.time()
        if not wait_for_in_file(SERIAL_LOG, "world mesh:", 180):
            print("[FAIL] the render mesh was never built")
            return 1
        t_mesh = time.time()
        if not wait_for_in_file(SERIAL_LOG, "entered the game", 600):
            print("[FAIL] the module never announced the client")
            return 1
        t_enter = time.time()
        # First real frame: textures decoded, lightmaps sampled, first draw.
        if not wait_for_in_file(SERIAL_LOG, "pixels frame=", 180):
            print("[FAIL] no rendered frames yet")
            return 1
        t_first = time.time()

        ntex = len(re.findall(r"\[Q3ARENA\] tex \d+ ", read_file(SERIAL_LOG)))
        print(f"[q3shot] TIMING (from the command being typed):")
        print(f"           collision world + render mesh : {t_mesh - t_typed:6.1f} s")
        print(f"           {ntex} textures + module init    : "
              f"{t_enter - t_mesh:6.1f} s")
        print(f"           first rendered frame           : "
              f"{t_first - t_enter:6.1f} s")
        print(f"           TOTAL before anything is on screen: "
              f"{t_first - t_typed:6.1f} s")
        if args.after:
            print(f"[q3shot] walking {args.after}s before the dump...")
            time.sleep(args.after)

        if not screendump():
            print("[FAIL] screendump produced nothing")
            return 1
        describe(SHOT_PPM)

        log = read_file(SERIAL_LOG)
        fr = re.findall(r"pos=\((-?\d+),(-?\d+),(-?\d+)\).*?drawn=(\d+) tris=(\d+)", log)
        if fr:
            print(f"[q3shot] last rendered frame: pos={fr[-1][0:3]} "
                  f"drawn={fr[-1][3]} tris={fr[-1][4]}")
        return 0
    finally:
        try:
            qemu.terminate()
            qemu.wait(timeout=10)
        except Exception:
            try:
                qemu.kill()
            except Exception:
                pass


if __name__ == "__main__":
    sys.exit(main())