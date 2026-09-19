#!/usr/bin/env python3
"""font_render_test.py — verify antialiased text (v38.97) on the real desktop.

Boots the OS, screenshots the desktop, and checks the icon labels (known
geometry: grid origin (24,24), pitch 96, 44px tile centred at
(x + ICON_W/2, y + ICON_W/2 - 6), label row directly under the tile) for:

  1. glyph ink exists in every label row (text actually rendered), and
  2. EDGE pixels between ink and background are PARTIAL blends — the
     signature of real antialiasing. The legacy 1-bit font produced only
     fg/bg colours; AA coverage (v38.97) leaves intermediate values.

This closes the visual gap where icons were vector-smooth but text stayed
blocky.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from desktop_capture import capture  # noqa: E402

# icon grid (mirrors src/gui/desktop.c init_icons + draw_icon geometry)
# ICON_W=72, ICON_H=80, grid pitch ICON_H+16=96, label at ic->y + ICON_W - 4
GRID_X, GRID_Y, PITCH, LABEL_DY, CELL_W = 24, 24, 96, 68, 72

LABELS = ["Terminal", "Browser", "Explorer", "SysInfo", "Clock", "PCI",
          "Snake", "Calc", "Task Mgr", "Flappy", "Notepad", "ELF Demo"]


def read_ppm(path):
    with open(path, "rb") as f:
        d = f.read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--out", default="/tmp/mectov_font_desktop.ppm")
    args = ap.parse_args()

    ok = capture(args.out, boot_timeout=args.timeout)
    if not ok:
        print("[FAIL] boot/capture failed")
        return 1

    w, h, raw = read_ppm(args.out)

    def px(x, y):
        i = (y * w + x) * 3
        return (raw[i], raw[i + 1], raw[i + 2])

    def lum(p):
        return (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000

    failures = []
    checked_partial = 0
    for idx, label in enumerate(LABELS):
        col = idx % 4
        row = idx // 4
        cx = GRID_X + col * PITCH + CELL_W // 2  # label centred in the cell
        ty = GRID_Y + row * PITCH + LABEL_DY     # label band, 16px tall
        # label text sits in a 16px band under the tile; scan it
        inks, partials = 0, 0
        band = []
        for y in range(ty, ty + 16):
            for x in range(cx - 48, cx + 48):
                p = px(x, y)
                l = lum(p)
                band.append(l)
                if l > 150:
                    inks += 1
                elif 60 < l < 150:
                    partials += 1  # neither background (<60) nor full ink
        if inks < 8:
            failures.append(f"{label}: no glyph ink ({inks} px)")
            continue
        # AA signature: at least 4 partial pixels across the band (glyph
        # edges). The 1-bit font could never produce these.
        if partials < 4:
            failures.append(f"{label}: no AA edge pixels ({partials} partial, {inks} ink)")
        checked_partial += partials

    if failures:
        for f in failures:
            print(f"[FAIL] {f}")
        return 1

    print(f"[OK] all {len(LABELS)} labels render with AA edges "
          f"({checked_partial} partial-coverage px total)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
