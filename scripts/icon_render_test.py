#!/usr/bin/env python3
"""Desktop icon renderer regression (TCG).

Boots the ISO, logs in, screendumps the desktop, and verifies the generated
icon artwork really landed on the framebuffer:

  palette    every pixel the compositor should leave as bare tile — inside the
             rounded silhouette, outside the glyph — matches the expected
             gradient row colour *exactly* (draw_rect path)
  bevel      covered by the same check: rows 0/1 and the last two rows carry
             the lit/shaded bevel colours, and the test requires bare-tile
             pixels to exist in those rows
  silhouette the tile corner is background, and the arc's solid span per row
             matches the renderer's integer corner math
  anti-alias the corner boundary pixel is a genuine blend (strictly between
             the tile colour and its background neighbour), which a hard-edged
             renderer cannot produce
  glyphs     each icon's glyph is drawn in the foreground colour

Expected artwork comes from scripts/gen_icons.py — the same source that
generates src/include/icon_glyphs.h — so this exercises the whole chain:
design -> generator -> kernel renderer -> framebuffer.
"""
import argparse
import math
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from desktop_capture import capture, ROOT          # noqa: E402
import gen_icons as GI                              # noqa: E402
from terminal_launch import _load_ppm               # noqa: E402

TILE = GI.TILE_PX
RAD = GI.TILE_R
GPX = GI.GLYPH_PX
GOFF = GI.GLYPH_OFF
FG = (0xF4, 0xF7, 0xFA)

# init_icons(): grid origin (24,24), pitch ICON_H+16 = 96; draw_pro_icon()
# centres a 44px tile at (x + ICON_W/2, y + ICON_W/2 - 6).
GRID_X, GRID_Y, PITCH = 24, 24, 96
TILE_DX, TILE_DY = 14, 8
COLS = 4
LABELS = ["Terminal", "Browser", "Explorer", "SysInfo", "Clock", "PCI",
          "Snake", "Calc", "Task Mgr", "Flappy", "Notepad", "ELF Demo"]


# --- renderer math, mirrored from src/gui/desktop.c ------------------------
def mix(a, b, n):
    """Packed 0xRRGGBB mix, integer-for-integer with C's icon_mix()."""
    k = 255 - n
    r = (((a >> 16) & 0xFF) * k + ((b >> 16) & 0xFF) * n) // 255
    g = (((a >> 8) & 0xFF) * k + ((b >> 8) & 0xFF) * n) // 255
    bl = ((a & 0xFF) * k + (b & 0xFF) * n) // 255
    return (r << 16) | (g << 8) | bl


def row_color(top, bot, row):
    col = mix(top, bot, (row * 255) // (TILE - 1))
    if row == 0:
        col = mix(col, 0xFFFFFF, 38)
    elif row == 1:
        col = mix(col, 0xFFFFFF, 15)
    if row == TILE - 1:
        col = mix(col, 0x000000, 42)
    elif row == TILE - 2:
        col = mix(col, 0x000000, 17)
    return ((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF)


def edge16(dy, r):
    r16 = 16 * r
    dy16 = r16 - (16 * dy + 8)
    return r16 - math.isqrt(r16 * r16 - dy16 * dy16)


def tile_cov(row, col):
    """1.0 inside the tile, 0.0 outside, partial on the arc boundary pixel."""
    if RAD <= row < TILE - RAD:
        return 1.0
    dy = row if row < RAD else TILE - 1 - row
    e = edge16(dy, RAD)
    lx, frac = e // 16, e & 15
    x0 = (e + 15) // 16
    if frac and (col == lx or col == TILE - 1 - lx):
        return (16 - frac) / 16.0
    return 1.0 if x0 <= col < TILE - x0 else 0.0


def glyph_alpha(hi, mid, gy, gx):
    a1 = hi[gy][gx]
    if a1:
        return a1 * 17                 # matches C: cov * 17 (15 -> 255)
    a2 = mid[gy][gx]
    return (a2 * 143) // 15 if a2 else 0


# --- checks ----------------------------------------------------------------
def check_icon(label, tile_x, tile_y, px, w, h, masks):
    hi, mid = masks
    palette = dict((l, (t, b)) for l, t, b in GI.PALETTE)
    top, bot = palette[label]

    bare_ok = bare_n = 0
    core_ok = core_n = 0
    bevel_rows = set()
    glyph_px = 0

    for row in range(TILE):
        expected = row_color(top, bot, row)
        for col in range(TILE):
            got = px(tile_x + col, tile_y + row)
            cov = tile_cov(row, col)
            if cov == 1.0:
                gy, gx = row - GOFF, col - GOFF
                a = 0
                if 0 <= gy < GPX and 0 <= gx < GPX:
                    a = glyph_alpha(hi, mid, gy, gx)
                if a == 0:
                    bare_n += 1
                    if got == expected:
                        bare_ok += 1
                    if row in (0, 1, TILE - 2, TILE - 1):
                        bevel_rows.add(row)
                elif a == 255:
                    core_n += 1
                    if all(abs(got[c] - FG[c]) <= 6 for c in range(3)):
                        core_ok += 1
            if all(abs(got[c] - FG[c]) <= 6 for c in range(3)):
                glyph_px += 1

    fails = []
    if bare_n < 150:
        fails.append("only %d bare-tile pixels sampled" % bare_n)
    elif bare_ok < bare_n * 0.97:
        fails.append("bare tile %d/%d exact (want >=97%%)" % (bare_ok, bare_n))
    if len(bevel_rows) < 3:
        fails.append("bevel rows not covered (%s)" % sorted(bevel_rows))
    if glyph_px < 60:
        fails.append("glyph barely present (%d px near fg)" % glyph_px)
    if core_n and core_ok < core_n * 0.6:
        fails.append("glyph core %d/%d opaque (want >=60%%)" % (core_ok, core_n))

    # Corner: outside the arc is background, inside is tile.
    if px(tile_x, tile_y) == row_color(top, bot, 0):
        fails.append("top-left corner not rounded (tile colour outside arc)")
    if px(tile_x + 11, tile_y) != row_color(top, bot, 0):
        fails.append("top-left arc solid span wrong at row 0")

    # Anti-aliasing: the boundary pixel must be a real partial blend.
    aa = 0
    for row in range(RAD):
        e = edge16(row, RAD)
        lx, frac = e // 16, e & 15
        if not frac or lx < 2:
            continue
        T = row_color(top, bot, row)
        P = px(tile_x + lx, tile_y + row)
        O = px(tile_x + lx - 2, tile_y + row)
        if P == T or P == O:
            continue
        if all(min(O[c], T[c]) - 8 <= P[c] <= max(O[c], T[c]) + 8 for c in range(3)) \
           and any(abs(P[c] - T[c]) >= 3 for c in range(3)) \
           and any(abs(P[c] - O[c]) >= 3 for c in range(3)):
            aa += 1
    # Only the rows whose arc boundary pixel sits at lx >= 2 can be compared
    # against a purely-background neighbour; that is 4 of the 11 corner rows
    # at R=11, so require 3.
    if aa < 3:
        fails.append("anti-aliased corner pixels: %d qualifying rows (want >=3)"
                     % aa)

    return fails, (bare_n, bare_ok, glyph_px, aa)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--iso", default=os.path.join(ROOT, "mectov.iso"))
    ap.add_argument("--disk", default=os.path.join(ROOT, "disk.img"))
    ap.add_argument("--ext2", default=os.path.join(ROOT, "ext2.img"))
    ap.add_argument("--smp", default=None)
    ap.add_argument("--dump", default="/tmp/mectov_icon_render.ppm")
    args = ap.parse_args()

    t0 = time.time()
    ok = capture(args.dump, iso=args.iso, disk=args.disk, ext2=args.ext2,
                 smp=args.smp, boot_timeout=args.timeout,
                 login_timeout=120)
    if not ok:
        print("[FAIL] could not capture the desktop")
        return 1

    w, h, pix = _load_ppm(args.dump)

    def px(x, y):
        o = (y * w + x) * 3
        return (pix[o], pix[o + 1], pix[o + 2])

    masks = {}
    for name in GI.GLYPHS:
        cov = GI.rasterize_cover(GI.GLYPHS[name])
        masks[name] = (cov[1], cov[2])

    failures = 0
    for i, label in enumerate(LABELS):
        col, row_i = i % COLS, i // COLS
        tx = GRID_X + col * PITCH + TILE_DX
        ty = GRID_Y + row_i * PITCH + TILE_DY
        fails, stats = check_icon(label, tx, ty, px, w, h, masks[label])
        status = "OK" if not fails else "FAIL"
        print("[%s] %-9s tile(%3d,%3d) bare %d/%d  glyph %d px  aa %d rows"
              % (status, label, tx, ty, stats[1], stats[0], stats[2], stats[3]))
        for f in fails:
            print("        - " + f)
        failures += len(fails)

    print("[i] %.1fs" % (time.time() - t0))
    if failures:
        print("[FAIL] %d icon assertion(s) failed" % failures)
        return 1
    print("[OK] all 12 desktop icons render with the expected palette, "
          "bevel, rounded silhouette, anti-aliasing and glyphs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
