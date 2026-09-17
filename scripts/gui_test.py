#!/usr/bin/env python3
"""scripts/gui_test.py — M9 GUI-1 desktop gate (screenshot proof).

The 64-bit port had no display at all before M9, so the desktop is judged (and
regression-guarded) entirely on pixels:

  1. markers   — serial says what the kernel decided: PS/2 aux on, the console
                 re-homed into a window, and the desktop's own geometry line.
  2. chrome    — at the geometry the kernel reported: the top bar and taskbar
                 bands are panel-coloured with amber accents, the window frame
                 hairline sits exactly on its edge, the client is the panel
                 fill, and the wallpaper is the gradient behind it (two samples
                 far apart differ — a flat fill would mean the gradient broke).
  3. cursor    — the 32-bit desktop's arrow sprite is composited at the
                 reported position, and it is the ONLY arrow on screen: a
                 leaked sprite (a scroll copying the pixels, a stale
                 save-under) would show up as a second hit.
  4. motion    — a relative mouse_move lands the arrow at exactly
                 (x+dx, y+dy) through IRQ12, the vacated spot is pixel-clean
                 (no white/#111 outline left behind), and the console keeps
                 printing into the window meanwhile (its text rows still match
                 the serial log through the same font check cons_test uses).

QEMU's `mouse_move` is RELATIVE, so the script tracks the cursor itself.
Exit 0 PASS, nonzero FAIL. Dumps kept at .check/gui_*.ppm for eyeballing.
"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
from qmp import QMP
import cons_test as ct  # font parsing + glyph-exact row comparison
from terminal_launch import _load_ppm, cursor_at

ISO = "mectov64.iso"
SERIAL = "serial_gui.log"
SOCK = "/tmp/qmp_gui"
FONT = "src/drivers/font8x16.c"

# M9 chrome palette (mirror of k64/gui64.c / src/gui/login.c)
PANEL = (0x16, 0x13, 0x0F)
LINE = (0x2C, 0x28, 0x21)
AMBER = (0xE0, 0xA9, 0x4F)
AMBER_BRT = (0xF5, 0xC5, 0x66)

DESKTOP_RE = re.compile(
    r"gui: desktop up win=(\d+),(\d+),(\d+),(\d+) "
    r"client=(\d+),(\d+),(\d+),(\d+) topbar=(\d+) taskbar=(\d+) "
    r"cursor=(\d+),(\d+)")


def boot():
    cmd = ["qemu-system-x86_64", "-machine", "q35"]
    if os.path.exists("/dev/kvm"):
        cmd += ["-cpu", "host", "-enable-kvm"]
    else:
        cmd += ["-cpu", "qemu64,+nx"]
    cmd += ["-m", "256", "-smp", "4", "-cdrom", ISO,
            "-serial", f"file:{SERIAL}", "-no-reboot", "-display", "none",
            "-qmp", f"unix:{SOCK},server=on,wait=off"]
    for p in (SOCK, SERIAL):
        try:
            os.unlink(p)
        except OSError:
            pass
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)


def wait_for(path, needle, timeout, since=0, poll=1.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with open(path, errors="replace") as f:
                f.seek(since)
                if needle in f.read():
                    return True
        except OSError:
            pass
        time.sleep(poll)
    return False


def dump(q, path):
    try:
        os.unlink(path)
    except OSError:
        pass
    q.hmp(f"screendump {os.path.abspath(path)}")
    for _ in range(40):
        if os.path.exists(path) and os.path.getsize(path) > 1000:
            time.sleep(0.3)  # let the (async) write settle
            return True
        time.sleep(0.25)
    return False


def sample(pix, w, x, y):
    o = (y * w + x) * 3
    return (pix[o], pix[o + 1], pix[o + 2])


def find_arrows(path):
    """Every arrow-shaped hit on the screen (see terminal_launch.cursor_at)."""
    w, h, pix = _load_ppm(path)
    hits = []
    for y in range(0, h - 24):
        for x in range(0, w - 16):
            o = (y * w + x) * 3
            if pix[o] == 0x11 and cursor_at(path, x, y):
                hits.append((x, y))
    # the detector also matches one row below the tip: keep the topmost
    arrows = []
    for x, y in hits:
        if not any(ax == x and ay == y - 1 for ax, ay in hits):
            arrows.append((x, y))
    return arrows


def count_color(pix, w, h, rgb, x0, y0, x1, y1):
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            if sample(pix, w, x, y) == rgb:
                n += 1
    return n


def main():
    if not os.path.exists(ISO):
        print("gui_test: build the ISO first (make iso64)")
        return 1
    os.makedirs(".check", exist_ok=True)
    qemu = boot()
    try:
        if not wait_for(SERIAL, "gui: desktop up", 180):
            print("gui_test FAIL: no desktop marker")
            return 1
        time.sleep(3)  # let the console stream a few lines into the window
        with open(SERIAL, errors="replace") as f:
            serial = f.read()
        m = DESKTOP_RE.search(serial)
        if not m:
            print("gui_test FAIL: desktop geometry line malformed")
            print("   ", [ln for ln in serial.split("\n") if "gui:" in ln][:2])
            return 1
        (wx, wy, ww, wh, cx, cy, cw, ch, topbar, taskbar,
         cur0x, cur0y) = (int(g) for g in m.groups())

        q = QMP(SOCK)
        rc = 0

        # ---- 1. markers ----
        checks = [
            ("mouse: PS/2 aux on" in serial, "ps2 aux marker"),
            (f"cons: rehomed {cw // 8}x{ch // 16} cells" in serial,
             "console rehom" "ed into the window"),
            (topbar == 24 and taskbar == 28, "chrome band heights"),
        ]
        for ok, name in checks:
            print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
            rc = rc or (not ok)
        print(f"        win={wx},{wy},{ww},{wh} client={cx},{cy},{cw},{ch} "
              f"cursor0={cur0x},{cur0y}")

        # ---- 2. chrome pixels ----
        d1 = os.path.join(".check", "gui_initial.ppm")
        if not dump(q, d1):
            print("gui_test FAIL: no screendump")
            return 1
        w, h, pix = _load_ppm(d1)
        amber_top = count_color(pix, w, h, AMBER, 0, 0, w, topbar)
        amber_btm = count_color(pix, w, h, AMBER, 0, h - taskbar, w, h) + \
            count_color(pix, w, h, AMBER_BRT, 0, h - taskbar, w, h)
        checks = [
            (sample(pix, w, 60, 12) == PANEL, "top bar is panel fill"),
            (sample(pix, w, 36, h - 14) == PANEL, "taskbar is panel fill"),
            (amber_top > 50, f"top bar amber ink ({amber_top}px)"),
            (amber_btm > 50, f"taskbar amber ink ({amber_btm}px)"),
            (sample(pix, w, wx, wy + wh // 2) == LINE, "window frame hairline"),
            (sample(pix, w, cx + cw // 2, cy + 2) in (PANEL,), "client is panel"),
            (sample(pix, w, cx - 8, topbar + 20) !=
             sample(pix, w, cx - 8, h - taskbar - 20), "wallpaper gradient"),
        ]
        for ok, name in checks:
            print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
            rc = rc or (not ok)

        # ---- 3. cursor at the reported position, and only there ----
        arrows = find_arrows(d1)
        cur_ok = arrows == [(cur0x, cur0y)]
        print(f"  [{'PASS' if cur_ok else 'FAIL'}] cursor at {cur0x},{cur0y} "
              f"(arrows found: {arrows})")
        rc = rc or (not cur_ok)

        # ---- 4. relative motion through IRQ12, with a clean vacated spot ----
        dx, dy = 137, 61
        q.hmp(f"mouse_move {dx} {dy}")
        time.sleep(1.5)
        d2 = os.path.join(".check", "gui_moved.ppm")
        if not dump(q, d2):
            print("gui_test FAIL: no screendump after mouse_move")
            return 1
        arrows2 = find_arrows(d2)
        want = (cur0x + dx, cur0y + dy)
        moved_ok = arrows2 == [want]
        print(f"  [{'PASS' if moved_ok else 'FAIL'}] cursor at {want} after "
              f"mouse_move {dx} {dy} (arrows found: {arrows2})")
        rc = rc or (not moved_ok)

        w2, h2, pix2 = _load_ppm(d2)
        box = [sample(pix2, w2, x, y)
               for y in range(cur0y, cur0y + 24)
               for x in range(cur0x, cur0x + 16)]
        clean_ok = 0xFFFFFF not in box and 0x111111 not in box
        print(f"  [{'PASS' if clean_ok else 'FAIL'}] vacated spot is clean "
              f"(no arrow ghost left at {cur0x},{cur0y})")
        rc = rc or (not clean_ok)

        # ---- 5. the console kept streaming inside the window ----
        with open(SERIAL, errors="replace") as f:
            serial2 = f.read()
        lines = [ln.rstrip("\r") for ln in serial2.split("\n") if ln.strip()]
        font = ct.load_font(FONT)
        cols, rows = cw // ct.CELL_W, ch // ct.CELL_H
        rendered = []
        for ln in lines:
            mk = ct.render_line(font, ln[:cols])
            if mk and sum(bin(b).count("1") for b in mk) >= 8:
                rendered.append((ln, mk))
        matched = 0
        for r in range(rows):
            mask = ct.row_mask(pix2, w2, cols, cy + r * ct.CELL_H, cx, PANEL)
            if sum(bin(b).count("1") for b in mask) < 8:
                continue
            for ln, mk in rendered:
                if len(mk) <= len(mask) and mask[:len(mk)] == mk \
                        and not any(mask[len(mk):]):
                    matched += 1
                    break
        stream_ok = matched >= 8
        print(f"  [{'PASS' if stream_ok else 'FAIL'}] live text in the window "
              f"({matched} glyph-exact rows)")
        rc = rc or (not stream_ok)

        q.close()
        print(f"gui_test: dumps = {d1}, {d2}")
        print("PASS" if rc == 0 else "FAIL")
        return rc
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            qemu.kill()


if __name__ == "__main__":
    sys.exit(main())
